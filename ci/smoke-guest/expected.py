#!/usr/bin/env python3
"""Prints the line init.c must print (same xorshift64 workload, ITERS = 2000000)."""
M = (1 << 64) - 1
x, s = 88172645463325252, 0
for i in range(2000000):
    x ^= (x << 13) & M
    x ^= x >> 7
    x ^= (x << 17) & M
    s = (s + x * (i | 1)) & M
print("RVVM-SMOKE-OK x=%016x sum=%016x" % (x, s))
