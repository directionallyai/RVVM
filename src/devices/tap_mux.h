/*
tap_mux.h - Multiplexed control channel (console + guest TCP streams) over stdin/stdout
Copyright (C) 2026  Directionally

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_TAP_MUX_H
#define RVVM_TAP_MUX_H

#include "chardev.h"
#include "networking.h"
#include "tap_api.h"

/*
 * With `-netmux`, the process talks to its embedder over stdin/stdout, in both directions, with
 * framed messages, and opens no host sockets and no listener for the guest:
 *
 *   - the guest console (UART) is stream 0;
 *   - a connection the guest makes (to any IPv4 address) is a stream the embedder answers, so the
 *     embedder decides where it goes, or refuses it;
 *   - the embedder can open a connection into a guest port (a stream the guest sees as an inbound
 *     TCP connection from the gateway).
 *
 * Frame: u32 big-endian length N, then N bytes: u8 type, u32 big-endian stream id, payload (N - 5).
 *
 *   type 1 HELLO     both ways     payload: u8 version (1). Sent once by this side when it is ready.
 *   type 2 OPEN_IN   embedder->vm  payload: u16 guest port. Ids chosen by the embedder: 1..0x7FFFFFFF.
 *   type 3 OPEN_OUT  vm->embedder  payload: u8 family (4), 4 bytes address, u16 port. Ids have the top bit set.
 *   type 4 DATA      both ways     payload: bytes (at most 65536). Stream 0 is the console.
 *   type 5 CREDIT    both ways     payload: u32 count. The receiver of DATA allows this many more bytes.
 *   type 6 FIN       both ways     the sender will send no more DATA (half close).
 *   type 7 RESET     both ways     abort the stream (refuse an OPEN_OUT, or a connection that failed).
 *
 * Each direction of each stream (not the console) starts with 65536 bytes of credit.
 * All integers are big-endian. IPv6 and UDP are not carried: UDP (DNS) keeps using host sockets.
 */

typedef struct tap_mux tap_mux_t;

// Take stdin/stdout for the control channel. Nothing else may use them afterwards. Starts no threads.
PUBLIC tap_mux_t* tap_mux_create(void);

// Console character device (stream 0), for the UART. Valid until the machine is freed.
PUBLIC chardev_t* tap_mux_console(tap_mux_t* mux);

// Start the reader and writer threads and say HELLO. `tap` may be NULL (console only).
PUBLIC bool tap_mux_start(tap_mux_t* mux, tap_dev_t* tap);

// tap_user.c -> mux: the guest opened a TCP connection to `dst`. Returns the socket the guest side
// should use as if it were the connection (its address is set to `dst`), or NULL to refuse.
net_sock_t* tap_mux_open_out(tap_mux_t* mux, const net_addr_t* dst);

// mux -> tap_user.c: a stream was opened into the guest. `sock` is the guest-facing end.
bool tap_user_inbound(tap_dev_t* tap, net_sock_t* sock, uint16_t guest_port);

// tap_user.c: the mux that serves this interface
void tap_user_set_mux(tap_dev_t* tap, tap_mux_t* mux);

#endif
