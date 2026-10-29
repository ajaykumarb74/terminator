/* Terminator entry point:  ./terminator            thread mode (default)
 *                          ./terminator useProcess=True   process mode */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "terminator.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

int gui_main(int use_process);

int main(int argc, char **argv)
{
    int proc = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "useProcess=", 11)) proc = !strcasecmp(argv[i] + 11, "true") || !strcmp(argv[i] + 11, "1");
        else { fprintf(stderr, "usage: %s [useProcess=True]\n", argv[0]); return 2; }
    }
    /* Terminator itself must never be stopped by job-control signals or broken pipes. */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (hist_init() < 0) fprintf(stderr, "terminator: warning: shared history unavailable\n");
    return gui_main(proc);
}
