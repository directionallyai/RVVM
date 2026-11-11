/*
tap_mux.c - Multiplexed control channel (console + guest TCP streams) over stdin/stdout
Copyright (C) 2026  Directionally

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "tap_mux.h"

#include "mem_ops.h"
#include "ringbuf.h"
#include "rvtimer.h"
#include "spinlock.h"
#include "threading.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define MUX_VERSION     1
#define MUX_MAX_PAYLOAD 65536
#define MUX_CHUNK       16384
#define MUX_WINDOW      65536
#define MUX_CONSOLE_ID  0
#define MUX_OUT_ID_BIT  0x80000000U
#define MUX_WAKE_ID     0xFFFFFFFFU

#define MUX_HELLO    1
#define MUX_OPEN_IN  2
#define MUX_OPEN_OUT 3
#define MUX_DATA     4
#define MUX_CREDIT   5
#define MUX_FIN      6
#define MUX_RESET    7
#define MUX_DNS_Q    8
#define MUX_DNS_R    9

typedef struct mux_stream {
    struct mux_stream* next;
    uint32_t           id;
    net_sock_t*        peer;       // The end this layer owns; the guest side holds the other end
    uint32_t           out_credit; // Bytes we may still send to the embedder
    uint8_t*           pend;       // Embedder data the peer socket has not accepted yet
    size_t             pend_len;
    size_t             pend_off;
    bool               polled;
    uint32_t           poll_flags;
    bool               host_fin;   // The embedder will send no more (FIN received)
    bool               host_fin_applied;
    bool               guest_fin;  // The guest will send no more (FIN sent to the embedder)
    bool               peer_eof;
} mux_stream_t;

typedef struct mux_out {
    struct mux_out* next;
    size_t          size;
    uint8_t         data[];
} mux_out_t;

struct tap_mux {
    spinlock_t    lock;
    tap_dev_t*    tap;
    net_poll_t*   poll;
    mux_stream_t* streams;
    uint32_t      next_out;
    bool          started;

    // Frames waiting to be written. Only the writer thread writes stdout, so no other thread can
    // block on an embedder that has stopped reading.
    mux_out_t*    out_head;
    mux_out_t*    out_tail;
    net_sock_t*   wake[2];

    ringbuf_t     con_rx; // Embedder -> guest console
    ringbuf_t     con_tx; // Guest console -> embedder
    uint32_t      con_flags;
    chardev_t     console;
};

/*
 * Frame output. One fwrite() per frame: stdio holds its lock for the whole call, so frames from
 * different threads cannot interleave.
 */

// Writes one frame to stdout. One fwrite() per frame. Only the writer thread (and start-up) calls it.
static void mux_write_frame(const uint8_t* frame, size_t size)
{
    fwrite(frame, 1, size, stdout);
}

// Queue a frame for the writer thread
static void mux_send(tap_mux_t* mux, uint8_t type, uint32_t id, const void* payload, size_t len)
{
    mux_out_t* out = safe_calloc(sizeof(mux_out_t) + len + 9, 1);
    out->size = len + 9;
    write_uint32_be_m(out->data, (uint32_t)(len + 5));
    out->data[4] = type;
    write_uint32_be_m(out->data + 5, id);
    if (len) memcpy(out->data + 9, payload, len);
    spin_lock(&mux->lock);
    if (mux->out_tail) mux->out_tail->next = out; else mux->out_head = out;
    mux->out_tail = out;
    spin_unlock(&mux->lock);
    if (mux->wake[1]) net_tcp_send(mux->wake[1], "x", 1);
}

// Writer thread: write everything queued, in order
static void mux_flush(tap_mux_t* mux)
{
    spin_lock(&mux->lock);
    mux_out_t* out = mux->out_head;
    mux->out_head = mux->out_tail = NULL;
    spin_unlock(&mux->lock);
    bool any = out != NULL;
    while (out) {
        mux_out_t* next = out->next;
        mux_write_frame(out->data, out->size);
        free(out);
        out = next;
    }
    if (any) fflush(stdout);
}

static bool mux_read_exact(void* buf, size_t size)
{
    return fread(buf, 1, size, stdin) == size;
}

/*
 * Console (stream 0)
 */

static uint32_t con_flags_locked(tap_mux_t* mux)
{
    uint32_t flags = 0;
    if (ringbuf_avail(&mux->con_rx)) flags |= CHARDEV_RX;
    if (ringbuf_space(&mux->con_tx)) flags |= CHARDEV_TX;
    return flags;
}

static uint32_t con_poll(chardev_t* dev)
{
    tap_mux_t* mux   = dev->data;
    uint32_t   flags = 0;
    spin_lock(&mux->lock);
    flags = con_flags_locked(mux);
    spin_unlock(&mux->lock);
    return flags;
}

static size_t con_read(chardev_t* dev, void* buf, size_t nbytes)
{
    tap_mux_t* mux = dev->data;
    spin_lock(&mux->lock);
    size_t ret = ringbuf_read(&mux->con_rx, buf, nbytes);
    spin_unlock(&mux->lock);
    return ret;
}

static size_t con_write(chardev_t* dev, const void* buf, size_t nbytes)
{
    tap_mux_t* mux = dev->data;
    spin_lock(&mux->lock);
    bool   was_empty = ringbuf_avail(&mux->con_tx) == 0;
    size_t ret       = ringbuf_write(&mux->con_tx, buf, nbytes);
    spin_unlock(&mux->lock);
    if (ret && was_empty && mux->wake[1]) net_tcp_send(mux->wake[1], "x", 1); // Wake the writer thread
    return ret;
}

static void con_update(chardev_t* dev)
{
    tap_mux_t* mux = dev->data;
    spin_lock(&mux->lock);
    uint32_t flags = con_flags_locked(mux);
    uint32_t fresh = flags & ~mux->con_flags;
    mux->con_flags = flags;
    spin_unlock(&mux->lock);
    if (fresh) chardev_notify(dev, fresh);
}

static void con_remove(chardev_t* dev)
{
    UNUSED(dev); // Lives as long as the process
}

PUBLIC tap_mux_t* tap_mux_create(void)
{
    tap_mux_t* mux = safe_new_obj(tap_mux_t);
    ringbuf_create(&mux->con_rx, 4096);
    ringbuf_create(&mux->con_tx, 16384);
    mux->next_out         = 1;
    mux->console.data     = mux;
    mux->console.poll     = con_poll;
    mux->console.read     = con_read;
    mux->console.write    = con_write;
    mux->console.update   = con_update;
    mux->console.remove   = con_remove;
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOFBF, 1 << 17);
    return mux;
}

PUBLIC chardev_t* tap_mux_console(tap_mux_t* mux)
{
    return &mux->console;
}

/*
 * Streams. Everything below runs with mux->lock held, except the sends noted.
 */

static mux_stream_t* stream_find(tap_mux_t* mux, uint32_t id)
{
    for (mux_stream_t* s = mux->streams; s; s = s->next) {
        if (s->id == id) return s;
    }
    return NULL;
}

static void stream_sync_poll(tap_mux_t* mux, mux_stream_t* s)
{
    bool          want_recv = s->out_credit > 0 && !s->peer_eof;
    bool          want_send = s->pend_len > s->pend_off;
    net_event_t   event     = {.data = (void*)(size_t)s->id, .flags = want_send ? NET_POLL_SEND : 0};
    if (!want_recv && !want_send) {
        if (s->polled) net_poll_remove(mux->poll, s->peer);
        s->polled = false;
    } else if (!s->polled) {
        s->polled     = net_poll_add(mux->poll, s->peer, &event);
        s->poll_flags = event.flags;
    } else if (s->poll_flags != event.flags) {
        net_poll_mod(mux->poll, s->peer, &event);
        s->poll_flags = event.flags;
    }
}

static void stream_free(tap_mux_t* mux, mux_stream_t* s)
{
    for (mux_stream_t** link = &mux->streams; *link; link = &(*link)->next) {
        if (*link == s) {
            *link = s->next;
            break;
        }
    }
    if (s->polled) net_poll_remove(mux->poll, s->peer);
    net_sock_close(s->peer);
    free(s->pend);
    free(s);
}

static mux_stream_t* stream_add(tap_mux_t* mux, uint32_t id, net_sock_t* peer)
{
    mux_stream_t* s = safe_new_obj(mux_stream_t);
    s->id           = id;
    s->peer         = peer;
    s->out_credit   = MUX_WINDOW;
    s->next         = mux->streams;
    mux->streams    = s;
    stream_sync_poll(mux, s);
    return s;
}

// Frees the stream once both directions are finished
static bool stream_reap(tap_mux_t* mux, mux_stream_t* s)
{
    if (s->host_fin_applied && s->guest_fin) {
        stream_free(mux, s);
        return true;
    }
    return false;
}

// Push pending embedder data into the peer socket; apply a deferred FIN. Returns bytes written.
// A negative result means the peer is gone.
static int64_t stream_flush(mux_stream_t* s)
{
    int64_t written = 0;
    while (s->pend_off < s->pend_len) {
        int32_t ret = net_tcp_send(s->peer, s->pend + s->pend_off, s->pend_len - s->pend_off);
        if (ret > 0) {
            s->pend_off += (size_t)ret;
            written += ret;
        } else if (ret == NET_ERR_BLOCK || ret == 0) {
            break;
        } else {
            return -1;
        }
    }
    if (s->pend_off == s->pend_len) {
        free(s->pend);
        s->pend = NULL;
        s->pend_len = s->pend_off = 0;
        if (s->host_fin && !s->host_fin_applied) {
            net_tcp_shutdown(s->peer);
            s->host_fin_applied = true;
        }
    }
    return written;
}

/*
 * Frames from the embedder
 */

static void mux_handle_open_in(tap_mux_t* mux, uint32_t id, const uint8_t* payload, size_t len)
{
    net_sock_t* pair[2] = {NULL, NULL};
    uint16_t    port    = len >= 2 ? read_uint16_be_m(payload) : 0;
    bool        ok      = port && mux->tap && (id & MUX_OUT_ID_BIT) == 0 && id != MUX_CONSOLE_ID;
    bool        added   = false;
    ok = ok && net_tcp_sockpair(pair);
    if (ok) {
        net_sock_set_blocking(pair[0], false);
        net_sock_set_blocking(pair[1], false);
        spin_lock(&mux->lock);
        if (stream_find(mux, id)) {
            ok = false;
        } else {
            stream_add(mux, id, pair[1]);
            added = true;
        }
        spin_unlock(&mux->lock);
        if (!ok) {
            net_sock_close(pair[0]);
            net_sock_close(pair[1]);
        }
    }
    if (ok && !tap_user_inbound(mux->tap, pair[0], port)) {
        net_sock_close(pair[0]); // The peer end sees the close and the stream is reset below
        ok = false;
    }
    if (!ok) {
        spin_lock(&mux->lock);
        mux_stream_t* s = stream_find(mux, id);
        if (s && added) stream_free(mux, s);
        spin_unlock(&mux->lock);
        mux_send(mux, MUX_RESET, id, NULL, 0);
    }
}

static void mux_handle_data(tap_mux_t* mux, uint32_t id, const uint8_t* payload, size_t len)
{
    uint32_t credit = 0;
    bool     reset  = false;
    if (id == MUX_CONSOLE_ID) {
        spin_lock(&mux->lock);
        ringbuf_write(&mux->con_rx, payload, len);
        spin_unlock(&mux->lock);
        chardev_notify(&mux->console, CHARDEV_RX);
        return;
    }
    spin_lock(&mux->lock);
    mux_stream_t* s = stream_find(mux, id);
    if (s && len && !s->host_fin) {
        size_t done = 0;
        if (s->pend_len == 0) {
            while (done < len) {
                int32_t ret = net_tcp_send(s->peer, payload + done, len - done);
                if (ret > 0) {
                    done += (size_t)ret;
                } else if (ret == NET_ERR_BLOCK || ret == 0) {
                    break;
                } else {
                    reset = true;
                    break;
                }
            }
        }
        if (!reset && done < len) {
            size_t rest = len - done;
            if (s->pend_off) { // Compact first: the realloc below may shrink the buffer
                memmove(s->pend, s->pend + s->pend_off, s->pend_len - s->pend_off);
                s->pend_len -= s->pend_off;
                s->pend_off = 0;
            }
            s->pend = safe_realloc(s->pend, s->pend_len + rest);
            memcpy(s->pend + s->pend_len, payload + done, rest);
            s->pend_len += rest;
        }
        credit = (uint32_t)done;
        if (reset) {
            stream_free(mux, s);
        } else {
            stream_sync_poll(mux, s);
        }
    }
    spin_unlock(&mux->lock);
    if (reset) {
        mux_send(mux, MUX_RESET, id, NULL, 0);
    } else if (credit) {
        uint8_t c[4];
        write_uint32_be_m(c, credit);
        mux_send(mux, MUX_CREDIT, id, c, 4);
    }
}

static void mux_handle_credit(tap_mux_t* mux, uint32_t id, const uint8_t* payload, size_t len)
{
    if (len < 4) return;
    uint32_t more = read_uint32_be_m(payload);
    spin_lock(&mux->lock);
    mux_stream_t* s = stream_find(mux, id);
    if (s) {
        uint64_t sum = (uint64_t)s->out_credit + more;
        s->out_credit = sum > 0xFFFFFFFFULL ? 0xFFFFFFFFU : (uint32_t)sum;
        stream_sync_poll(mux, s);
    }
    spin_unlock(&mux->lock);
}

static void mux_handle_fin(tap_mux_t* mux, uint32_t id)
{
    spin_lock(&mux->lock);
    mux_stream_t* s = stream_find(mux, id);
    if (s && !s->host_fin) {
        s->host_fin = true;
        if (s->pend_len == s->pend_off) {
            net_tcp_shutdown(s->peer);
            s->host_fin_applied = true;
            stream_reap(mux, s);
        }
    }
    spin_unlock(&mux->lock);
}

static void mux_handle_reset(tap_mux_t* mux, uint32_t id)
{
    spin_lock(&mux->lock);
    mux_stream_t* s = stream_find(mux, id);
    if (s) stream_free(mux, s);
    spin_unlock(&mux->lock);
}

static void* mux_reader_thread(void* arg)
{
    tap_mux_t* mux = arg;
    uint8_t*   body = safe_calloc(MUX_MAX_PAYLOAD + 5, 1);
    while (true) {
        uint8_t header[4];
        if (!mux_read_exact(header, sizeof(header))) break;
        uint32_t length = read_uint32_be_m(header);
        if (length < 5 || length > MUX_MAX_PAYLOAD + 5) {
            rvvm_error("netmux: bad frame length %u", length);
            break;
        }
        if (!mux_read_exact(body, length)) break;
        uint8_t        type    = body[0];
        uint32_t       id      = read_uint32_be_m(body + 1);
        const uint8_t* payload = body + 5;
        size_t         plen    = length - 5;
        switch (type) {
            case MUX_HELLO:   break;
            case MUX_OPEN_IN: mux_handle_open_in(mux, id, payload, plen); break;
            case MUX_DATA:    mux_handle_data(mux, id, payload, plen); break;
            case MUX_CREDIT:  mux_handle_credit(mux, id, payload, plen); break;
            case MUX_FIN:     mux_handle_fin(mux, id); break;
            case MUX_RESET:   mux_handle_reset(mux, id); break;
            case MUX_DNS_R:   if (mux->tap) tap_user_dns_reply(mux->tap, id, payload, plen); break;
            default:          break; // Unknown types are ignored
        }
    }
    // The embedder went away: there is nobody left to serve
    rvvm_info("netmux: control channel closed, exiting");
    exit(0);
    return NULL;
}

/*
 * Frames to the embedder: the guest's console output and the guest's data on each stream
 */

static void mux_poll_stream(tap_mux_t* mux, uint32_t id, uint32_t flags, bool* stalled)
{
    uint8_t  data[MUX_CHUNK];
    size_t   data_len = 0;
    int      trailer  = 0; // MUX_FIN or MUX_RESET to send after the data
    spin_lock(&mux->lock);
    mux_stream_t* s = stream_find(mux, id);
    if (s) {
        if (flags & NET_POLL_SEND) {
            int64_t written = stream_flush(s);
            if (written < 0) {
                trailer = MUX_RESET;
            } else if (written > 0) {
                uint8_t c[4];
                write_uint32_be_m(c, (uint32_t)written);
                spin_unlock(&mux->lock);
                mux_send(mux, MUX_CREDIT, id, c, 4);
                spin_lock(&mux->lock);
                s = stream_find(mux, id);
            }
        }
        if (s && !trailer) {
            if (s->out_credit > 0 && !s->peer_eof) {
                size_t   want = s->out_credit < sizeof(data) ? s->out_credit : sizeof(data);
                int32_t  ret  = net_tcp_recv(s->peer, data, want);
                if (ret > 0) {
                    data_len = (size_t)ret;
                    s->out_credit -= (uint32_t)ret;
                } else if (ret == NET_ERR_DISCONNECT) {
                    s->peer_eof = true;
                    trailer     = MUX_FIN;
                } else if (ret != NET_ERR_BLOCK) {
                    trailer = MUX_RESET;
                }
            } else if (!s->peer_eof) {
                *stalled = true; // Readable but no credit: do not spin on it
            }
        }
        if (s) {
            if (trailer == MUX_RESET) {
                stream_free(mux, s);
            } else {
                if (trailer == MUX_FIN) s->guest_fin = true;
                stream_sync_poll(mux, s);
                if (trailer == MUX_FIN) stream_reap(mux, s);
            }
        }
    }
    spin_unlock(&mux->lock);
    if (data_len) mux_send(mux, MUX_DATA, id, data, data_len);
    if (trailer) mux_send(mux, (uint8_t)trailer, id, NULL, 0);
}

static void* mux_writer_thread(void* arg)
{
    tap_mux_t*  mux = arg;
    net_event_t events[64];
    while (true) {
        bool   stalled = false;
        size_t count   = net_poll_wait(mux->poll, events, 64, 1000); // Woken by the wake socket when there is output
        for (size_t i = 0; i < count; ++i) {
            uint32_t id = (uint32_t)(size_t)events[i].data;
            if (id == MUX_WAKE_ID) {
                char tmp[256];
                net_tcp_recv(mux->wake[0], tmp, sizeof(tmp));
            } else {
                mux_poll_stream(mux, id, events[i].flags, &stalled);
            }
        }

        // Console output
        uint8_t con[MUX_CHUNK];
        spin_lock(&mux->lock);
        size_t n = ringbuf_read(&mux->con_tx, con, sizeof(con));
        spin_unlock(&mux->lock);
        if (n) {
            mux_send(mux, MUX_DATA, MUX_CONSOLE_ID, con, n);
            con_update(&mux->console);
        }
        mux_flush(mux);
        if (stalled) sleep_ms(2);
    }
    return NULL;
}

PUBLIC bool tap_mux_start(tap_mux_t* mux, tap_dev_t* tap)
{
    if (!mux || mux->started) return false;
    mux->poll = net_poll_create();
    if (!mux->poll || !net_tcp_sockpair(mux->wake)) return false;
    net_sock_set_blocking(mux->wake[0], false);
    net_sock_set_blocking(mux->wake[1], false);
    net_event_t wake_event = {.data = (void*)(size_t)MUX_WAKE_ID};
    if (!net_poll_add(mux->poll, mux->wake[0], &wake_event)) return false;
    mux->started = true;
    mux->tap     = tap;
    if (tap) tap_user_set_mux(tap, mux);
    uint8_t version = MUX_VERSION;
    mux_send(mux, MUX_HELLO, 0, &version, 1);
    mux_flush(mux);
    thread_create(mux_writer_thread, mux);
    thread_create(mux_reader_thread, mux);
    return true;
}

/*
 * The guest opened a connection
 */

net_sock_t* tap_mux_open_out(tap_mux_t* mux, const net_addr_t* dst)
{
    net_sock_t* pair[2] = {NULL, NULL};
    if (!mux || !mux->started || dst->type != NET_TYPE_IPV4 || !net_tcp_sockpair(pair)) return NULL;
    net_sock_set_blocking(pair[0], false);
    net_sock_set_blocking(pair[1], false);

    spin_lock(&mux->lock);
    uint32_t id = mux->next_out++ | MUX_OUT_ID_BIT;
    if (mux->next_out == 0) mux->next_out = 1;
    stream_add(mux, id, pair[1]);
    spin_unlock(&mux->lock);

    *net_sock_addr(pair[0]) = *dst; // The tap matches guest segments by this address

    uint8_t payload[7];
    payload[0] = 4;
    memcpy(payload + 1, dst->ip, 4);
    write_uint16_be_m(payload + 5, dst->port);
    mux_send(mux, MUX_OPEN_OUT, id, payload, sizeof(payload));
    return pair[0];
}

void tap_mux_dns_query(tap_mux_t* mux, uint32_t id, const uint8_t* data, size_t size)
{
    mux_send(mux, MUX_DNS_Q, id, data, size);
}
