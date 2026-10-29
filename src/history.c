/*
 * Shared command history.
 *
 * The history lives in a POSIX shared-memory segment (shm_open + mmap) so every tab - whether it
 * is a thread (thread mode) or a separate process (process mode) - sees the same ring buffer.
 * A process-shared POSIX semaphore (sem_init(..., pshared=1, 1)) serialises all access.
 * The last HIST_MAX commands are persisted in ~/.terminator_history (appended on every command,
 * compacted on exit), like bash does with ~/.bash_history.
 */
#include "terminator.h"
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    sem_t lock;
    long  total;                       /* commands ever stored (sequence number of newest) */
    char  items[HIST_MAX][HIST_LEN];   /* ring buffer: item i lives in slot i % HIST_MAX   */
} HistShm;

static HistShm *H;
static char hist_path[PATH_MAX];

static void lock(void)   { while (sem_wait(&H->lock) == -1 && errno == EINTR) {} }
static void unlock(void) { sem_post(&H->lock); }

static void store(const char *cmd)       /* caller holds the lock */
{
    char *slot = H->items[H->total % HIST_MAX];
    snprintf(slot, HIST_LEN, "%s", cmd);
    H->total++;
}

int hist_init(void)
{
    char name[64];
    snprintf(name, sizeof name, "/terminator_hist_%d", (int)getpid());
    size_t sz = sizeof(HistShm);
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    void *m = MAP_FAILED;
    if (fd >= 0) {
        shm_unlink(name);                /* stays alive while mapped; nothing left behind */
        if (ftruncate(fd, (off_t)sz) == 0)
            m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    }
    if (m == MAP_FAILED)                 /* fallback: anonymous shared mapping */
        m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    H = m;
    if (sem_init(&H->lock, 1, 1) == -1) return -1;
    H->total = 0;

    const char *home = getenv("HOME");
    snprintf(hist_path, sizeof hist_path, "%s/.terminator_history", home && *home ? home : "/tmp");
    FILE *f = fopen(hist_path, "r");
    if (f) {
        char line[4096];
        lock();
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\n') line[--n] = 0;
            else { int ch; while ((ch = fgetc(f)) != EOF && ch != '\n') {} }   /* discard overlong tail */
            if (n) store(line);
        }
        unlock();
        fclose(f);
    }
    return 0;
}

void hist_add(const char *cmd)
{
    if (!H) return;
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (!*cmd) return;
    lock();
    store(cmd);
    int fd = open(hist_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd >= 0) {
        char line[HIST_LEN + 2];
        int n = snprintf(line, sizeof line, "%s\n", H->items[(H->total - 1) % HIST_MAX]);
        if (n > 0 && write(fd, line, (size_t)n) < 0) {}
        close(fd);
    }
    unlock();
}

long hist_total(void) { if (!H) return 0; lock(); long t = H->total; unlock(); return t; }
long hist_count(void) { long t = hist_total(); return t > HIST_MAX ? HIST_MAX : t; }

int hist_get_abs(long seq, char *out, size_t sz)
{
    if (!H) return -1;
    lock();
    int ok = -1;
    if (seq >= 1 && seq <= H->total && seq > H->total - HIST_MAX) {
        snprintf(out, sz, "%s", H->items[(seq - 1) % HIST_MAX]);
        ok = 0;
    }
    unlock();
    return ok;
}

int hist_get_back(long n, char *out, size_t sz)
{
    if (!H) return -1;
    lock();
    long seq = H->total - n + 1;
    int ok = -1;
    if (n >= 1 && seq >= 1 && seq > H->total - HIST_MAX) {
        snprintf(out, sz, "%s", H->items[(seq - 1) % HIST_MAX]);
        ok = 0;
    }
    unlock();
    return ok;
}

/* length of the longest common substring of a and b (rolling-row DP) */
static int lcs_len(const char *a, const char *b)
{
    size_t m = strlen(a), n = strlen(b);
    int best = 0;
    if (!m || !n) return 0;
    int *prev = calloc(n + 1, sizeof(int)), *cur = calloc(n + 1, sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return 0; }
    for (size_t i = 1; i <= m; i++) {
        for (size_t j = 1; j <= n; j++) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) best = cur[j];
            } else cur[j] = 0;
        }
        int *t = prev; prev = cur; cur = t;
    }
    free(prev); free(cur);
    return best;
}

/*
 * Ctrl+R semantics:
 *  a) the most recent command that equals the term; else
 *  b) the command(s) with the longest common substring with the term, if that is > 2 chars
 *     (newest first, duplicates removed); else
 *  c) nothing (returns 0).
 */
int hist_search(const char *term, char ***res)
{
    *res = NULL;
    if (!H || !*term) return 0;
    lock();
    long cnt = H->total > HIST_MAX ? HIST_MAX : H->total;
    long first = H->total - cnt;                    /* seq-1 of oldest */
    int found = 0;
    char **out = calloc(64, sizeof *out);
    if (!out) { unlock(); return 0; }

    for (long k = H->total - 1; k >= first; k--)
        if (!strcmp(H->items[k % HIST_MAX], term)) {
            out[0] = strdup(term);
            found = 1;
            break;
        }
    if (!found) {
        int best = 0;
        int *lens = malloc((size_t)cnt * sizeof(int));
        if (lens) {
            for (long i = 0; i < cnt; i++) {
                lens[i] = lcs_len(term, H->items[(first + i) % HIST_MAX]);
                if (lens[i] > best) best = lens[i];
            }
            if (best > 2) {
                for (long i = cnt - 1; i >= 0 && found < 63; i--) {
                    if (lens[i] != best) continue;
                    const char *s = H->items[(first + i) % HIST_MAX];
                    int dup = 0;
                    for (int d = 0; d < found; d++) if (!strcmp(out[d], s)) { dup = 1; break; }
                    if (!dup) out[found++] = strdup(s);
                }
            }
            free(lens);
        }
    } else found = 1;
    unlock();
    if (!found) { free(out); return 0; }
    *res = out;
    return found;
}

void hist_save(void)
{
    if (!H) return;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", hist_path);
    lock();
    FILE *f = fopen(tmp, "w");
    if (f) {
        long cnt = H->total > HIST_MAX ? HIST_MAX : H->total;
        for (long i = H->total - cnt; i < H->total; i++) fprintf(f, "%s\n", H->items[i % HIST_MAX]);
        if (fclose(f) == 0) rename(tmp, hist_path); else unlink(tmp);
    }
    unlock();
}

void hist_shutdown(void)
{
    if (!H) return;
    hist_save();
}
