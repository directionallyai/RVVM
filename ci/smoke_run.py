#!/usr/bin/env python3
"""Boots the CI smoke guest (ci/smoke-guest) under a built rvvm binary and checks its output.
usage: smoke_run.py RVVM GUESTDIR [extra rvvm args...]   (GUESTDIR holds fw_jump.bin, Image, expected.txt)
Exit status 0 only if the guest printed the expected line and rvvm exited with status 0 within the time limit."""
import os, subprocess, sys, time

rvvm, guest, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
expected = open(os.path.join(guest, "expected.txt")).read().strip()
cmd = [rvvm, os.path.join(guest, "fw_jump.bin"), "-k", os.path.join(guest, "Image"), "-m", "256M", "-nogui", "-nonet"] + extra
print("run:", " ".join(cmd), flush=True)
t = time.time()
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    out, _ = p.communicate(timeout=int(os.environ.get("SMOKE_TIMEOUT", "180")))
except subprocess.TimeoutExpired:
    p.kill()
    out, _ = p.communicate()
    print(out.decode(errors="replace")[-3000:])
    sys.exit("FAIL: timed out")
text = out.decode(errors="replace")
print("exit status %s after %.1f s" % (p.returncode, time.time() - t))
if expected not in text:
    print(text[-3000:])
    sys.exit("FAIL: expected line not found: " + expected)
if p.returncode != 0:
    print(text[-3000:])
    sys.exit("FAIL: rvvm exited with status %s" % p.returncode)
print("OK:", expected)
