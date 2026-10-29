/* File-name auto-completion helpers (Task 10). */
#include "terminator.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

void strv_free_list(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/*
 * `word` is the (already unescaped) text before the cursor. It is split at the last '/' into a
 * directory part and a name prefix. Candidates are returned as full replacement words
 * (dir part + entry name, with a trailing '/' for directories), sorted alphabetically.
 */
int complete_candidates(const char *cwd, const char *word, char ***out)
{
    *out = NULL;
    const char *slash = strrchr(word, '/');
    char dirpart[PATH_MAX] = "", prefix[NAME_MAX + 1];
    if (slash) {
        size_t dl = (size_t)(slash - word) + 1;
        if (dl >= sizeof dirpart) return 0;
        memcpy(dirpart, word, dl);
        dirpart[dl] = 0;
        snprintf(prefix, sizeof prefix, "%s", slash + 1);
    } else snprintf(prefix, sizeof prefix, "%s", word);

    char dir[PATH_MAX * 2];
    if (!dirpart[0]) snprintf(dir, sizeof dir, "%s", cwd);
    else if (dirpart[0] == '/') snprintf(dir, sizeof dir, "%s", dirpart);
    else if (dirpart[0] == '~' && (dirpart[1] == '/' || !dirpart[1])) {
        const char *h = getenv("HOME");
        snprintf(dir, sizeof dir, "%s/%s", h ? h : "/", dirpart + (dirpart[1] ? 2 : 1));
    } else snprintf(dir, sizeof dir, "%s/%s", cwd, dirpart);

    DIR *d = opendir(dir);
    if (!d) return 0;
    char **list = NULL;
    int n = 0, cap = 0;
    size_t pl = strlen(prefix);
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
        if (nm[0] == '.' && prefix[0] != '.') continue;
        if (strncmp(nm, prefix, pl)) continue;
        char full[PATH_MAX * 2 + NAME_MAX + 8];
        snprintf(full, sizeof full, "%s/%s", dir, nm);
        struct stat st;
        int isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        char *w = malloc(strlen(dirpart) + strlen(nm) + 2);
        if (!w) continue;
        sprintf(w, "%s%s%s", dirpart, nm, isdir ? "/" : "");
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char **nl = realloc(list, (size_t)cap * sizeof *list);
            if (!nl) { free(w); break; }
            list = nl;
        }
        list[n++] = w;
    }
    closedir(d);
    if (n > 1) qsort(list, (size_t)n, sizeof *list, cmp);
    *out = list;
    return n;
}

size_t complete_lcp(char **list, int n)
{
    if (n <= 0) return 0;
    size_t l = strlen(list[0]);
    for (int i = 1; i < n; i++) {
        size_t k = 0;
        while (k < l && list[i][k] == list[0][k]) k++;
        l = k;
    }
    return l;
}

/* backslash-escape characters the shell would treat specially */
void complete_escape(const char *s, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *s && o + 2 < outsz; s++) {
        if (strchr(" \t'\"\\$&;|<>()*?[]#!`", *s)) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = 0;
}
