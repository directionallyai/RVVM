/* PID 1 of the CI smoke guest: runs a small deterministic CPU workload (so the JIT has something to translate),
 * prints the result, then powers the machine off. The host side checks the printed line and RVVM's exit status. */
#include <stdint.h>
#include <stdio.h>
#include <sys/reboot.h>
#include <unistd.h>

#define ITERS 2000000u

int main(void) {
    uint64_t x = 88172645463325252ULL, sum = 0;
    for (uint32_t i = 0; i < ITERS; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        sum += x * (i | 1);
    }
    printf("RVVM-SMOKE-OK x=%016llx sum=%016llx\n", (unsigned long long)x, (unsigned long long)sum);
    fflush(stdout);
    sync();
    reboot(RB_POWER_OFF);
    for (;;) pause();
}
