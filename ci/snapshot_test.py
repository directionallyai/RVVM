#!/usr/bin/env python3
"""Snapshot tests for a built headless rvvm, with the ticker guest from ci/smoke-guest (it prints RVVM-TICK <n> every 200 ms).
usage: snapshot_test.py RVVM GUESTDIR [extra rvvm args...]   (GUESTDIR holds fw_jump.bin and Image-tick)

Checks, in order:
  1. -snapshot: run, stop with Ctrl+A x on the console, exit status 0, <base>.state written, <base>.ram exactly as big as RAM
  2. -snapload: resumes and the counter carries on (CPU, RAM, timers and console survive), RAM mapped (not read), the state
     is kept and the RAM image is not modified
  3. two -snapload copies at once, independent, image still not modified
  4. -snapshot again: resumes, runs on, saves over the mapped image; the RAM image changed
  5. -snapload after that: starts from where step 4 stopped
  6. a snapshot does not load into a machine with different RAM: fails cleanly, no hang
Exit status 0 only if every check passed."""
import hashlib, os, re, shutil, subprocess, sys, tempfile, threading, time

rvvm, guest, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
MEM_MB = 256
FW, IMAGE = os.path.join(guest, "fw_jump.bin"), os.path.join(guest, "Image-tick")
WORK = tempfile.mkdtemp(prefix="rvvm-snap-")
BASE = os.path.join(WORK, "s")
STATE, RAM = BASE + ".state", BASE + ".ram"
LIMIT = int(os.environ.get("SNAP_TIMEOUT", "120"))
vms, failures = [], []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)
    return ok


class VM:
    def __init__(self, flag, mem=MEM_MB):
        cmd = [rvvm, FW, "-k", IMAGE, "-m", "%dM" % mem, "-nogui", "-nonet", "-v", flag, BASE] + extra
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.out = bytearray()
        self.ticks = []
        self.t0 = time.time()
        vms.append(self)
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        pending = b""
        for chunk in iter(lambda: self.p.stdout.read1(4096), b""):
            self.out.extend(chunk)
            pending += chunk
            *lines, pending = pending.split(b"\n")
            for line in lines:
                m = re.search(rb"RVVM-TICK (\d+)", line)
                if m:
                    self.ticks.append(int(m.group(1)))

    def wait_ticks(self, n, timeout=LIMIT):
        end = time.time() + timeout
        while len(self.ticks) < n and time.time() < end and self.p.poll() is None:
            time.sleep(0.05)
        return len(self.ticks) >= n

    def text(self):
        return self.out.decode(errors="replace")

    def stop(self):
        """Ctrl+A x on the console: the machine halts and, with -snapshot, is saved. Returns the exit status or None."""
        try:
            self.p.stdin.write(b"\x01x")
            self.p.stdin.flush()
        except OSError:
            pass
        try:
            return self.p.wait(timeout=LIMIT)
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        time.sleep(0.2)

    def rss_anon_mb(self):
        try:
            for line in open("/proc/%d/status" % self.p.pid):
                if line.startswith("RssAnon"):
                    return int(line.split()[1]) // 1024
        except OSError:
            pass
        return None


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def size(path):
    return os.path.getsize(path) if os.path.exists(path) else -1


def consecutive(ticks):
    return all(b == a + 1 for a, b in zip(ticks, ticks[1:]))


def resumed_ok(vm, floor, what, need=5):
    """The copy printed `need` ticks, the first one close to where the snapshot was taken, then +1 each (timers work)."""
    got = vm.wait_ticks(need)
    t = vm.ticks[:need]
    check(got, "%s: printed %d ticks" % (what, need))
    if got:
        check(floor - 1 <= t[0] <= floor + 3, "%s: carries on from tick %d (first tick %d)" % (what, floor, t[0]))
        check(consecutive(t), "%s: ticks are consecutive %s" % (what, t))
    return got


try:
    print("work dir", WORK, flush=True)

    # 1. cold run with -snapshot, stopped from the console
    a = VM("-snapshot")
    check(a.wait_ticks(6), "1. cold boot printed 6 ticks")
    rc = a.stop()
    check(rc == 0, "1. stopped from the console, exit status %s" % rc)
    last1 = max(a.ticks) if a.ticks else 0
    check(size(STATE) > 0, "1. %s written (%d bytes)" % (os.path.basename(STATE), size(STATE)))
    check(size(RAM) == MEM_MB << 20, "1. %s is exactly RAM (%d bytes)" % (os.path.basename(RAM), size(RAM)))
    ram1, state1 = sha(RAM), size(STATE)

    # 2. -snapload resumes it, RAM mapped, nothing consumed or modified
    b = VM("-snapload")
    resumed_ok(b, last1, "2. -snapload")
    check("not mapped" not in b.text(), "2. RAM image mapped, not read")
    anon = b.rss_anon_mb()
    if anon is not None:
        check(anon < MEM_MB // 2, "2. anonymous memory %d MB, well under the %d MB of RAM" % (anon, MEM_MB))
    b.kill()
    check(size(STATE) == state1, "2. -snapload kept the state (%d bytes)" % size(STATE))
    check(sha(RAM) == ram1, "2. -snapload left the RAM image alone")

    # 3. two copies at once
    c, d = VM("-snapload"), VM("-snapload")
    resumed_ok(c, last1, "3. copy 1", 4)
    resumed_ok(d, last1, "3. copy 2", 4)
    check(c.p.poll() is None and d.p.poll() is None, "3. both copies are still running")
    c.kill()
    d.kill()
    check(sha(RAM) == ram1, "3. image unchanged after two copies")

    # 4. -snapshot resumes (consumes the state), runs on, saves over the mapped image
    e = VM("-snapshot")
    resumed_ok(e, last1, "4. -snapshot resume")
    check("not mapped" not in e.text(), "4. RAM image mapped, not read")
    e.wait_ticks(len(e.ticks) + 5)
    rc = e.stop()
    check(rc == 0, "4. stopped from the console, exit status %s" % rc)
    last4 = max(e.ticks) if e.ticks else 0
    check(last4 > last1 + 3, "4. ran on (tick %d -> %d)" % (last1, last4))
    check(size(STATE) > 0, "4. state saved again (%d bytes)" % size(STATE))
    check(size(RAM) == MEM_MB << 20, "4. RAM image still exactly RAM (%d bytes)" % size(RAM))
    check(sha(RAM) != ram1, "4. RAM image was rewritten with the new state")

    # 5. the new snapshot continues from step 4
    f = VM("-snapload")
    resumed_ok(f, last4, "5. -snapload after the save over a mapped image")
    f.kill()

    # 6. different RAM size: must fail, not hang or crash
    g = VM("-snapload", mem=MEM_MB * 2)
    try:
        rc = g.p.wait(timeout=LIMIT)
    except subprocess.TimeoutExpired:
        rc = None
    check(rc not in (None, 0), "6. a snapshot does not load into a machine with other RAM (exit status %s)" % rc)
    check(not g.ticks, "6. the other machine printed no ticks")
    g.kill()
finally:
    for v in vms:
        v.kill()
    shutil.rmtree(WORK, ignore_errors=True)

print("\n%d check(s) failed" % len(failures) if failures else "\nall snapshot checks passed")
for f_ in failures:
    print(" -", f_)
sys.exit(1 if failures else 0)
