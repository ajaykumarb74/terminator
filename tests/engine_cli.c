/* Headless driver: feeds lines from stdin to one shell engine (a thread) through the same
 * IPC protocol the GUI uses, and prints the engine's output.  Used by tests/run_tests.sh.
 *   plain line   send as a command line, wait until the engine is idle again
 *   ~line        send without waiting         @sleep MS   pause
 *   @c / @z / @d send Ctrl+C / Ctrl+Z / Ctrl+D   @wait   wait until idle                    */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include "../src/terminator.h"
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int c2e[2], e2c[2];
static IpcReader rd;
static int idle = 1, got_status;

static void *thr(void *a) { (void)a; engine_run(c2e[0], e2c[1], "."); return NULL; }

static int pump(int tmo)
{
    struct pollfd p = { e2c[0], POLLIN, 0 };
    if (poll(&p, 1, tmo) <= 0) return 0;
    if (ipc_fill(e2c[0], &rd, 1) < 0) return -1;
    char t, buf[65536]; uint32_t n;
    while (ipc_pop(&rd, &t, buf, sizeof buf, &n)) {
        if (t == MSG_OUT) fwrite(buf, 1, n, stdout);
        else if (t == MSG_STATUS) { TabStatus *s = (TabStatus *)buf; idle = !s->busy; got_status = 1; }
        else if (t == MSG_BIND) printf("[BIND:%.*s]\n", (int)n, buf);
        else if (t == MSG_EXIT) { printf("[EXIT]\n"); return -1; }
    }
    fflush(stdout);
    return 1;
}
static void wait_idle(void)
{
    for (int i = 0; i < 400; i++) { if (pump(50) < 0) exit(0); if (idle) { pump(20); return; } }
    printf("[TIMEOUT]\n");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    hist_init();
    if (pipe(c2e) || pipe(e2c)) return 1;
    pthread_t t;
    pthread_create(&t, NULL, thr, NULL);
    for (int i = 0; i < 100 && !got_status; i++) pump(50);   /* wait for the engine's first status */
    char line[2048];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        if (!strncmp(line, "@sleep ", 7)) { int ms = atoi(line + 7); for (int e = 0; e < ms; e += 20) if (pump(20) < 0) return 0; continue; }
        if (!strcmp(line, "@c")) { idle = 0; ipc_send(c2e[1], MSG_INT, "", 0); wait_idle(); continue; }
        if (!strcmp(line, "@z")) { ipc_send(c2e[1], MSG_TSTP, "", 0); wait_idle(); continue; }
        if (!strcmp(line, "@d")) { ipc_send(c2e[1], MSG_EOF, "", 0); continue; }
        if (!strcmp(line, "@wait")) { wait_idle(); continue; }
        int nowait = line[0] == '~';
        idle = 0;
        ipc_send(c2e[1], MSG_LINE, line + nowait, (uint32_t)strlen(line + nowait));
        if (!nowait) wait_idle();
    }
    ipc_send(c2e[1], MSG_QUIT, "", 0);
    close(c2e[1]);
    pthread_join(t, NULL);
    pump(50);
    return 0;
}
