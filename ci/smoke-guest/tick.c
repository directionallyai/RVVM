/* PID 1 of the CI snapshot guest: prints "RVVM-TICK <n>" every 200 ms and never exits, so a test can stop the machine
 * mid-run, resume it from its snapshot and check that the counter carries on (CPU, RAM, timers and the console all
 * have to survive). The counter lives only in guest RAM. */
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void) {
    struct timespec ts = {0, 200 * 1000 * 1000};
    for (unsigned n = 0;; n++) {
        printf("RVVM-TICK %u\n", n);
        fflush(stdout);
        nanosleep(&ts, NULL);
    }
}
