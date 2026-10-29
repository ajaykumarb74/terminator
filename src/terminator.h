/*
 * Terminator - shared definitions.
 *
 * Architecture (see DESIGN.md):
 *   gui.c       X11 window, tabs, line editor, hotkeys, selections   (only code that touches X11)
 *   shell.c     per-tab shell engine: parser, expansion, jobs, builtins, scripts
 *   history.c   shared-memory history guarded by a process-shared semaphore
 *   complete.c  file-name completion helpers
 *   ipc.c       tiny framed message protocol used on the GUI <-> engine pipes
 *   main.c      start-up, thread mode / process mode launching
 */
#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define PROMPT      "user@terminator> "
#define CPROMPT     "> "

#define INSZ        1024        /* longest line a user can type              */
#define MAX_TABS    8
#define HIST_MAX    10000       /* commands kept in the history file         */
#define HIST_LEN    1024        /* longest stored command                    */
#define HIST_SHOW   1000        /* commands shown by the `history` builtin   */

/* ---------------- IPC protocol: [type:1][len:4][payload] ---------------- */
/* GUI -> engine */
#define MSG_LINE    'L'   /* a line the user entered (command or stdin of a running job) */
#define MSG_INT     'c'   /* Ctrl+C; payload = the unsent input text (for echoing)       */
#define MSG_TSTP    'z'   /* Ctrl+Z                                                      */
#define MSG_EOF     'D'   /* Ctrl+D while a job runs: close its stdin                    */
#define MSG_QUIT    'Q'   /* close the tab                                               */
/* engine -> GUI */
#define MSG_OUT     'O'   /* terminal output                                             */
#define MSG_CLEAR   'C'   /* clear the transcript                                        */
#define MSG_STATUS  'S'   /* TabStatus                                                   */
#define MSG_EXIT    'X'   /* the shell asked to exit (tab should close)                  */
#define MSG_BIND    'K'   /* `bind ...` builtin: payload = arguments                     */

typedef struct {
    int  busy;            /* a foreground command is running   */
    int  cont;            /* waiting for continuation (">")    */
    char cwd[PATH_MAX];
} TabStatus;

typedef struct {
    unsigned char *buf;
    size_t len, cap, off;
} IpcReader;

int  ipc_write_all(int fd, const void *d, size_t n);
int  ipc_send(int fd, char type, const void *d, uint32_t n);
/* read up to ~maxbytes; >0 bytes read, 0 = nothing available (EAGAIN), -1 = EOF/error */
int  ipc_fill(int fd, IpcReader *r, size_t maxbytes);
/* pop one complete frame (payload copied, truncated to dstsz) */
int  ipc_pop(IpcReader *r, char *type, void *dst, size_t dstsz, uint32_t *plen);
void ipc_free(IpcReader *r);

/* ---------------- shell engine ---------------- */
/* Runs one tab's shell until it is told to quit. Blocks. Used both as a thread body
 * (thread mode) and as the body of a forked process (process mode). */
void engine_run(int ctl_fd, int out_fd, const char *cwd);

/* ---------------- history (shared memory + semaphore) ---------------- */
int  hist_init(void);                       /* create shm, load history file          */
void hist_add(const char *cmd);             /* thread/process safe                    */
long hist_total(void);                      /* sequence number of the newest entry    */
long hist_count(void);                      /* entries currently held                 */
int  hist_get_abs(long seq, char *out, size_t sz);   /* seq is 1-based                */
int  hist_get_back(long n, char *out, size_t sz);    /* n=1 -> newest                 */
/* Ctrl+R search; returns number of results (malloc'd strings in *res), 0 = no match */
int  hist_search(const char *term, char ***res);
void hist_save(void);                       /* rewrite history file with last 10000   */
void hist_shutdown(void);

/* ---------------- file name completion ---------------- */
/* Returns number of candidates (sorted); *out is a malloc'd array of malloc'd full words. */
int  complete_candidates(const char *cwd, const char *word, char ***out);
size_t complete_lcp(char **list, int n);
void complete_escape(const char *s, char *out, size_t outsz);
void strv_free_list(char **v, int n);

#endif
