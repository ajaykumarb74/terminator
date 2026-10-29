/*
 * Terminator GUI - the only file that touches X11 (Xlib only).
 *
 * One window, a tab bar, a transcript per tab, an editable input line per tab.  Every tab owns a
 * shell engine (shell.c) that runs as a thread (thread mode) or as a forked process (process
 * mode) and is reached through two pipes with the framed protocol of ipc.c.  The event loop
 * poll()s the X connection and all tab pipes together, so it never busy-waits.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "terminator.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/keysymdef.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */
enum { M_NORMAL, M_SEARCH, M_PICK };

typedef struct {
    int       cmd_fd, out_fd;           /* GUI -> engine, engine -> GUI */
    pid_t     pid;                      /* process mode */
    pthread_t th;                       /* thread mode  */
    int       started, dead;
    IpcReader rd;
    /* transcript */
    char     *out; size_t olen, ocap;
    int       col, esc;
    /* state reported by the engine */
    int       busy, cont;
    char      cwd[PATH_MAX];
    /* line editor */
    char      in[INSZ]; int ilen, cur;
    int       hback; char draft[INSZ];
    /* modes */
    int       mode;
    char      sterm[128]; int slen;
    char    **cands; int ncands; int pick_ws, pick_end, pick_val;
    /* view */
    int       scroll;
    long      sel_a, sel_b; int sel_on;
} Tab;

typedef struct { int ctl, out; char cwd[PATH_MAX]; } ThreadArg;

static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font;
static Pixmap back;
static int W = 900, H = 600, cw = 9, lh = 18, asc = 14, hdr_h = 26, scr;
static unsigned long c_bg, c_fg, c_prompt, c_input, c_head, c_tabsel, c_selbg, c_cursor, c_dim;
static Atom a_clip, a_prim, a_utf8, a_targets, a_prop, a_wmdel, a_string;

static Tab tabs[MAX_TABS];
static int ntabs, active, running = 1, proc_mode, dirty = 1;
static char *clip_text;
static int tab_x0[MAX_TABS + 1], tab_x1[MAX_TABS + 1];     /* header hit boxes, [ntabs] = "[ + ]" */
static int dragging;

/* last frame layout, used by the mouse */
static char *g_disp; static size_t g_dcap, g_dlen, g_olen_vis;
typedef struct { size_t s, e; } Row;
static Row *g_rows; static size_t g_rcap; static int g_nrows, g_first, g_cols = 80, g_maxrows = 20;

/* ------------------------------------------------------------------ */
/* hotkeys (Task 7): defaults, config file, `bind` builtin             */
/* ------------------------------------------------------------------ */
enum { A_LEFT, A_RIGHT, A_UP, A_DOWN, A_START, A_END, A_KILLS, A_KILLE, A_DELW, A_CLEAR, A_EXIT,
       A_COPY, A_PASTE, A_INT, A_TSTP, A_SEARCH, A_NEWTAB, A_CLOSETAB, A_N };
static const struct { const char *name, *def, *desc; } ACT[A_N] = {
    {"left",           "Left",         "move cursor left"},
    {"right",          "Right",        "move cursor right"},
    {"history_up",     "Up",           "previous command"},
    {"history_down",   "Down",         "next command"},
    {"line_start",     "Ctrl+A",       "cursor to start of line"},
    {"line_end",       "Ctrl+E",       "cursor to end of line"},
    {"kill_to_start",  "Ctrl+U",       "delete to start of line"},
    {"kill_to_end",    "Ctrl+K",       "delete to end of line"},
    {"delete_word",    "Ctrl+W",       "delete previous word"},
    {"clear_screen",   "Ctrl+L",       "clear the screen"},
    {"exit_shell",     "Ctrl+D",       "close tab / send EOF"},
    {"copy",           "Ctrl+Shift+C", "copy selection (X11 selection)"},
    {"paste",          "Ctrl+Shift+V", "paste (X11 selection)"},
    {"interrupt",      "Ctrl+C",       "stop the running command"},
    {"suspend",        "Ctrl+Z",       "suspend the running command"},
    {"history_search", "Ctrl+R",       "search history"},
    {"new_tab",        "Ctrl+T",       "open a tab"},
    {"close_tab",      "Ctrl+Shift+W", "close the tab"},
};
typedef struct { KeySym ks; unsigned mods; } KB;
static KB kb[A_N], kbdef[A_N];
#define MODMASK (ShiftMask | ControlMask | Mod1Mask)

static const struct { const char *a, *k; } KEYALIAS[] = {
    {"pageup", "Prior"}, {"pagedown", "Next"}, {"enter", "Return"}, {"backspace", "BackSpace"},
    {"esc", "Escape"}, {"del", "Delete"}, {"plus", "plus"}, {"space", "space"}, {NULL, NULL}
};

static int parse_spec(const char *spec, KB *out, char *err, size_t errsz)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", spec);
    unsigned mods = 0;
    char *parts[8];
    int np = 0;
    for (char *p = buf, *tok; np < 8 && (tok = strsep(&p, "+"));) {
        while (isspace((unsigned char)*tok)) tok++;
        size_t l = strlen(tok);
        while (l && isspace((unsigned char)tok[l - 1])) tok[--l] = 0;
        parts[np++] = tok;
    }
    if (np == 0 || !*parts[np - 1]) { snprintf(err, errsz, "missing key in '%s'", spec); return -1; }
    for (int i = 0; i < np - 1; i++) {
        if (!strcasecmp(parts[i], "ctrl") || !strcasecmp(parts[i], "control")) mods |= ControlMask;
        else if (!strcasecmp(parts[i], "shift")) mods |= ShiftMask;
        else if (!strcasecmp(parts[i], "alt") || !strcasecmp(parts[i], "meta")) mods |= Mod1Mask;
        else { snprintf(err, errsz, "unknown modifier '%s'", parts[i]); return -1; }
    }
    const char *kn = parts[np - 1];
    for (int i = 0; KEYALIAS[i].a; i++) if (!strcasecmp(kn, KEYALIAS[i].a)) kn = KEYALIAS[i].k;
    KeySym ks = NoSymbol;
    if (!kn[1] && isalpha((unsigned char)kn[0])) ks = (KeySym)tolower((unsigned char)kn[0]);
    else ks = XStringToKeysym(kn);
    if (ks == NoSymbol && isalpha((unsigned char)kn[0])) {       /* "left" -> "Left", "f5" -> "F5" */
        char t[64];
        snprintf(t, sizeof t, "%s", kn);
        t[0] = (char)toupper((unsigned char)t[0]);
        ks = XStringToKeysym(t);
    }
    if (ks == NoSymbol) { snprintf(err, errsz, "unknown key '%s'", kn); return -1; }
    if (ks >= 'A' && ks <= 'Z') ks = ks - 'A' + 'a';
    if (!(mods & (ControlMask | Mod1Mask)) && ks >= 0x20 && ks < 0x7f) {
        snprintf(err, errsz, "'%s' would stop that key from typing; use Ctrl or Alt", spec);
        return -1;
    }
    if (!mods && (ks == XK_Return || ks == XK_BackSpace || ks == XK_Tab || ks == XK_Escape || ks == XK_KP_Enter)) {
        snprintf(err, errsz, "'%s' is reserved", spec);
        return -1;
    }
    out->ks = ks;
    out->mods = mods;
    return 0;
}
static void key_name(KB k, char *out, size_t sz)
{
    out[0] = 0;
    if (k.mods & ControlMask) strncat(out, "Ctrl+", sz - strlen(out) - 1);
    if (k.mods & Mod1Mask) strncat(out, "Alt+", sz - strlen(out) - 1);
    if (k.mods & ShiftMask) strncat(out, "Shift+", sz - strlen(out) - 1);
    const char *n = XKeysymToString(k.ks);
    char t[64];
    snprintf(t, sizeof t, "%s", n ? n : "?");
    if (!t[1]) t[0] = (char)toupper((unsigned char)t[0]);
    strncat(out, t, sz - strlen(out) - 1);
}
static int kb_eq(KB a, KB b) { return a.ks == b.ks && a.mods == b.mods; }

/* report and revert custom bindings that collide, until no collision is left */
static void resolve_conflicts(char *msg, size_t msgsz)
{
    for (int guard = 0; guard < A_N * 2; guard++) {
        int fixed = 0;
        for (int a = 0; a < A_N && !fixed; a++)
            for (int b = a + 1; b < A_N && !fixed; b++) {
                if (!kb_eq(kb[a], kb[b])) continue;
                char kn[64];
                key_name(kb[a], kn, sizeof kn);
                int ca = !kb_eq(kb[a], kbdef[a]), cb = !kb_eq(kb[b], kbdef[b]);
                size_t l = strlen(msg);
                snprintf(msg + l, msgsz - l, "bind: %s is used by both '%s' and '%s' - reverting to defaults\n", kn, ACT[a].name, ACT[b].name);
                if (ca) kb[a] = kbdef[a];
                if (cb) kb[b] = kbdef[b];
                fixed = 1;
            }
        if (!fixed) return;
    }
}
static const char *rc_path(void)
{
    static char p[PATH_MAX];
    const char *e = getenv("TERMINATOR_RC"), *h = getenv("HOME");
    if (e && *e) snprintf(p, sizeof p, "%s", e);
    else snprintf(p, sizeof p, "%s/.terminatorrc", h && *h ? h : "/tmp");
    return p;
}
static void save_config(void)
{
    FILE *f = fopen(rc_path(), "w");
    if (!f) return;
    fprintf(f, "# Terminator key bindings (rewritten by the `bind` command)\n# bind <action> = <Ctrl+Shift+Alt+Key>\n");
    for (int a = 0; a < A_N; a++)
        if (!kb_eq(kb[a], kbdef[a])) { char kn[64]; key_name(kb[a], kn, sizeof kn); fprintf(f, "bind %s = %s\n", ACT[a].name, kn); }
    fclose(f);
}
static int find_action(const char *n)
{
    for (int a = 0; a < A_N; a++) if (!strcmp(ACT[a].name, n)) return a;
    return -1;
}
static void bindings_defaults(void)
{
    for (int a = 0; a < A_N; a++) {
        char err[128];
        parse_spec(ACT[a].def, &kbdef[a], err, sizeof err);
        kb[a] = kbdef[a];
    }
}
static void load_config(char *msg, size_t msgsz)
{
    FILE *f = fopen(rc_path(), "r");
    if (!f) return;
    char line[256];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        size_t l = strlen(p);
        while (l && isspace((unsigned char)p[l - 1])) p[--l] = 0;
        if (!*p || *p == '#') continue;
        size_t m = strlen(msg);
        if (strncmp(p, "bind", 4) || !isspace((unsigned char)p[4])) { snprintf(msg + m, msgsz - m, "%s:%d: unknown directive, ignored\n", rc_path(), ln); continue; }
        p += 4;
        while (isspace((unsigned char)*p)) p++;
        char *sp = p;
        while (*sp && !isspace((unsigned char)*sp) && *sp != '=') sp++;
        char *rest = sp;
        while (isspace((unsigned char)*rest) || *rest == '=') rest++;
        *sp = 0;
        int a = find_action(p);
        if (a < 0) { snprintf(msg + m, msgsz - m, "%s:%d: unknown action '%s', ignored\n", rc_path(), ln, p); continue; }
        KB k;
        char err[160];
        if (parse_spec(rest, &k, err, sizeof err) < 0) {
            snprintf(msg + m, msgsz - m, "%s:%d: invalid binding for '%s' (%s); using default %s\n", rc_path(), ln, p, err, ACT[a].def);
            continue;
        }
        kb[a] = k;
    }
    fclose(f);
    resolve_conflicts(msg, msgsz);
}
/* `bind`, `bind reset`, `bind <action> <key>` typed in a tab */
static void bind_command(const char *args, char *reply, size_t rsz)
{
    reply[0] = 0;
    char a[256];
    snprintf(a, sizeof a, "%.255s", args);
    char *p = a;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) {
        for (int i = 0; i < A_N; i++) {
            char kn[64];
            key_name(kb[i], kn, sizeof kn);
            size_t l = strlen(reply);
            snprintf(reply + l, rsz - l, "%-15s %-14s %s%s\n", ACT[i].name, kn, ACT[i].desc, kb_eq(kb[i], kbdef[i]) ? "" : "  (custom)");
        }
        return;
    }
    if (!strcmp(p, "reset")) {
        for (int i = 0; i < A_N; i++) kb[i] = kbdef[i];
        save_config();
        snprintf(reply, rsz, "bind: all key bindings reset to defaults\n");
        return;
    }
    char *sp = p;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    char *rest = sp;
    if (*sp) *sp++ = 0;
    rest = sp;
    while (isspace((unsigned char)*rest) || *rest == '=') rest++;
    int act = find_action(p);
    if (act < 0) { snprintf(reply, rsz, "bind: unknown action '%s' (run `bind` to list them)\n", p); return; }
    if (!strcmp(rest, "default") || !strcmp(rest, "reset")) { kb[act] = kbdef[act]; save_config(); snprintf(reply, rsz, "bind: %s reset to %s\n", p, ACT[act].def); return; }
    KB k;
    char err[160];
    if (parse_spec(rest, &k, err, sizeof err) < 0) {
        kb[act] = kbdef[act];
        snprintf(reply, rsz, "bind: invalid binding (%s); %s falls back to default %s\n", err, p, ACT[act].def);
        save_config();
        return;
    }
    kb[act] = k;
    char msg[512] = "";
    resolve_conflicts(msg, sizeof msg);
    save_config();
    char kn[64];
    key_name(kb[act], kn, sizeof kn);
    snprintf(reply, rsz, "%s%s", msg, kb_eq(kb[act], k) ? "" : "");
    size_t l = strlen(reply);
    snprintf(reply + l, rsz - l, "bind: %s = %s\n", p, kn);
}

/* ------------------------------------------------------------------ */
/* transcript                                                         */
/* ------------------------------------------------------------------ */
static void tab_append(Tab *t, const char *d, size_t n)
{
    if (t->olen + n + 8 > t->ocap) {
        size_t nc = t->ocap ? t->ocap * 2 : 65536;
        while (nc < t->olen + n + 8) nc *= 2;
        t->out = realloc(t->out, nc);
        t->ocap = nc;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (t->esc == 1) { t->esc = c == '[' ? 2 : 0; continue; }
        if (t->esc == 2) { if (c >= 0x40 && c <= 0x7e) t->esc = 0; continue; }
        if (c == 27) { t->esc = 1; continue; }
        if (c == '\r') continue;
        if (c == '\t') { do { t->out[t->olen++] = ' '; t->col++; } while (t->col % 8); continue; }
        if (c == '\n') { t->out[t->olen++] = '\n'; t->col = 0; continue; }
        if (c == 8) { if (t->col > 0 && t->olen && t->out[t->olen - 1] != '\n') { t->olen--; t->col--; } continue; }
        if (c < 32 || c == 127) continue;
        t->out[t->olen++] = c < 127 ? (char)c : '?';
        t->col++;
    }
    if (t->olen > 400000) {                         /* bounded scrollback: drop the oldest lines */
        size_t cut = 100000;
        while (cut < t->olen && t->out[cut - 1] != '\n') cut++;
        memmove(t->out, t->out + cut, t->olen - cut);
        t->olen -= cut;
        t->sel_on = 0;
    }
    dirty = 1;
}
static void tab_puts(Tab *t, const char *s) { tab_append(t, s, strlen(s)); }

/* ------------------------------------------------------------------ */
/* engines: one per tab, thread or process                            */
/* ------------------------------------------------------------------ */
static void *engine_thread(void *p)
{
    ThreadArg a = *(ThreadArg *)p;
    free(p);
    engine_run(a.ctl, a.out, a.cwd);
    return NULL;
}
static int engine_start(Tab *t, const char *cwd)
{
    int c2e[2], e2c[2];
    if (pipe2(c2e, O_CLOEXEC) < 0) return -1;
    if (pipe2(e2c, O_CLOEXEC) < 0) { close(c2e[0]); close(c2e[1]); return -1; }
    t->pid = -1;
    if (proc_mode) {
        pid_t pid = fork();
        if (pid < 0) { close(c2e[0]); close(c2e[1]); close(e2c[0]); close(e2c[1]); return -1; }
        if (pid == 0) {                                  /* the tab's own process */
            close(c2e[1]); close(e2c[0]);
            for (int i = 0; i < ntabs; i++) if (&tabs[i] != t) { if (tabs[i].cmd_fd >= 0) close(tabs[i].cmd_fd); if (tabs[i].out_fd >= 0) close(tabs[i].out_fd); }
            close(ConnectionNumber(dpy));                /* only the GUI talks to X11 */
            engine_run(c2e[0], e2c[1], cwd);
            _exit(0);
        }
        close(c2e[0]); close(e2c[1]);
        t->pid = pid;
    } else {
        ThreadArg *a = malloc(sizeof *a);
        a->ctl = c2e[0]; a->out = e2c[1];
        snprintf(a->cwd, sizeof a->cwd, "%s", cwd);
        if (pthread_create(&t->th, NULL, engine_thread, a) != 0) { free(a); close(c2e[0]); close(c2e[1]); close(e2c[0]); close(e2c[1]); return -1; }
    }
    t->cmd_fd = c2e[1];
    t->out_fd = e2c[0];
    int fl = fcntl(t->out_fd, F_GETFL, 0);
    fcntl(t->out_fd, F_SETFL, fl | O_NONBLOCK);
    t->started = 1; t->dead = 0; t->busy = 0; t->cont = 0;
    t->rd.len = t->rd.off = 0;
    return 0;
}
static void engine_stop(Tab *t)
{
    if (!t->started) return;
    if (t->cmd_fd >= 0) { ipc_send(t->cmd_fd, MSG_QUIT, "", 0); close(t->cmd_fd); t->cmd_fd = -1; }
    if (proc_mode) {
        if (t->pid > 0) {
            int tries = 0;
            while (waitpid(t->pid, NULL, WNOHANG) == 0) {
                if (++tries > 200) { kill(t->pid, SIGKILL); waitpid(t->pid, NULL, 0); break; }
                usleep(10000);
            }
        }
    } else if (!t->dead) pthread_join(t->th, NULL);
    if (t->out_fd >= 0) { close(t->out_fd); t->out_fd = -1; }
    t->started = 0;
    t->pid = -1;
}
static void send_msg(Tab *t, char type, const void *d, size_t n)
{
    if (t->cmd_fd < 0 || t->dead) return;
    if (ipc_send(t->cmd_fd, type, d, (uint32_t)n)) t->dead = 1;
}

/* ------------------------------------------------------------------ */
/* tabs                                                               */
/* ------------------------------------------------------------------ */
static void tab_init(Tab *t, const char *cwd)
{
    memset(t, 0, sizeof *t);
    t->cmd_fd = t->out_fd = -1;
    t->pid = -1;
    snprintf(t->cwd, sizeof t->cwd, "%s", cwd);
}
static void tab_free(Tab *t)
{
    free(t->out);
    ipc_free(&t->rd);
    if (t->cands) strv_free_list(t->cands, t->ncands);
    memset(t, 0, sizeof *t);
}
static void new_tab(void)
{
    if (ntabs >= MAX_TABS) return;
    char cwd[PATH_MAX];
    snprintf(cwd, sizeof cwd, "%s", tabs[active].cwd);
    Tab *t = &tabs[ntabs];
    tab_init(t, cwd);
    ntabs++;
    if (engine_start(t, cwd) < 0) { tab_puts(t, "terminator: could not start the shell for this tab\n"); t->dead = 1; }
    else tab_puts(t, "New tab opened.\n\n");
    active = ntabs - 1;
    dirty = 1;
}
static void close_tab(int i)
{
    if (ntabs <= 1) { running = 0; return; }
    engine_stop(&tabs[i]);
    tab_free(&tabs[i]);
    for (int k = i; k < ntabs - 1; k++) tabs[k] = tabs[k + 1];
    memset(&tabs[ntabs - 1], 0, sizeof(Tab));
    ntabs--;
    if (active >= ntabs) active = ntabs - 1;
    else if (active > i) active--;
    dirty = 1;
}

/* messages from an engine; returns 1 if the tab asked to be closed */
static int tab_read(Tab *t)
{
    int closeit = 0;
    int k = ipc_fill(t->out_fd, &t->rd, 65536);       /* bounded so a flood never starves the event loop */
    if (k < 0 && !t->dead) {
        t->dead = 1; t->busy = 0; t->cont = 0;
        if (proc_mode && t->pid > 0) {
            int st = 0;
            if (waitpid(t->pid, &st, 0) > 0) { char m[128]; snprintf(m, sizeof m, "\n[terminator: tab shell %s %d - press Enter to restart it]\n", WIFSIGNALED(st) ? "killed by signal" : "exited with status", WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st)); tab_puts(t, m); }
            t->pid = -1;
        } else tab_puts(t, "\n[terminator: tab shell ended - press Enter to restart it]\n");
        if (!proc_mode) pthread_join(t->th, NULL);
        close(t->out_fd); t->out_fd = -1;
        if (t->cmd_fd >= 0) { close(t->cmd_fd); t->cmd_fd = -1; }
        t->started = 0;
    }
    static char buf[65536 + 8];
    char type;
    uint32_t len;
    while (ipc_pop(&t->rd, &type, buf, sizeof buf - 1, &len)) {
        buf[len] = 0;
        switch (type) {
        case MSG_OUT: tab_append(t, buf, len); break;
        case MSG_CLEAR: t->olen = 0; t->col = 0; t->scroll = 0; t->sel_on = 0; dirty = 1; break;
        case MSG_STATUS:
            if (len == sizeof(TabStatus)) { TabStatus s; memcpy(&s, buf, sizeof s); t->busy = s.busy; t->cont = s.cont; snprintf(t->cwd, sizeof t->cwd, "%s", s.cwd); dirty = 1; }
            break;
        case MSG_EXIT: closeit = 1; break;
        case MSG_BIND: { char reply[2048]; bind_command(buf, reply, sizeof reply); tab_puts(t, reply); break; }
        }
    }
    return closeit;
}

/* ------------------------------------------------------------------ */
/* line editor                                                        */
/* ------------------------------------------------------------------ */
static void ins_char(Tab *t, char c)
{
    if (t->ilen >= INSZ - 1) return;
    memmove(t->in + t->cur + 1, t->in + t->cur, (size_t)(t->ilen - t->cur + 1));
    t->in[t->cur++] = c;
    t->ilen++;
}
static void set_input(Tab *t, const char *s)
{
    snprintf(t->in, sizeof t->in, "%s", s);
    t->ilen = (int)strlen(t->in);
    t->cur = t->ilen;
}
static void hist_nav(Tab *t, int dir)
{
    char buf[HIST_LEN];
    if (dir < 0) {                                    /* up */
        if (t->hback == 0) snprintf(t->draft, sizeof t->draft, "%s", t->in);
        if (hist_get_back(t->hback + 1, buf, sizeof buf) == 0) { t->hback++; set_input(t, buf); }
    } else if (t->hback > 0) {
        t->hback--;
        if (t->hback == 0) set_input(t, t->draft);
        else if (hist_get_back(t->hback, buf, sizeof buf) == 0) set_input(t, buf);
    }
}
static const char *prompt_of(Tab *t)
{
    if (t->mode == M_SEARCH) return "Enter search term: ";
    if (t->dead || t->busy) return "";
    return t->cont ? CPROMPT : PROMPT;
}
static void submit(Tab *t)
{
    t->scroll = 0;
    if (t->dead) {                                    /* restart a crashed tab */
        engine_stop(t);
        t->dead = 0;
        if (engine_start(t, t->cwd) < 0) { t->dead = 1; tab_puts(t, "terminator: restart failed\n"); }
        else tab_puts(t, "[tab shell restarted]\n");
        t->ilen = t->cur = 0; t->in[0] = 0;
        return;
    }
    send_msg(t, MSG_LINE, t->in, (size_t)t->ilen);
    t->ilen = t->cur = 0;
    t->in[0] = 0;
    t->hback = 0;
}

/* Tab key (Task 10) */
static int word_start(Tab *t)
{
    int i = t->cur;
    while (i > 0 && !(isspace((unsigned char)t->in[i - 1]) && !(i >= 2 && t->in[i - 2] == '\\'))) i--;
    return i;
}
static void replace_word(Tab *t, int ws, int we, const char *text)
{
    char esc[PATH_MAX * 2];
    complete_escape(text, esc, sizeof esc);
    char tail[INSZ];
    snprintf(tail, sizeof tail, "%s", t->in + we);
    char head[INSZ];
    snprintf(head, sizeof head, "%.*s", ws, t->in);
    if ((int)(strlen(head) + strlen(esc) + strlen(tail)) >= INSZ - 1) return;
    snprintf(t->in, sizeof t->in, "%s%s%s", head, esc, tail);
    t->ilen = (int)strlen(t->in);
    t->cur = (int)(strlen(head) + strlen(esc));
}
static void do_complete(Tab *t)
{
    int ws = word_start(t);
    char word[INSZ];
    size_t w = 0;
    for (int i = ws; i < t->cur; i++) {
        if (t->in[i] == '\\' && i + 1 < t->cur) i++;
        word[w++] = t->in[i];
    }
    word[w] = 0;
    char **list = NULL;
    int n = complete_candidates(t->cwd, word, &list);
    if (n == 0) return;                               /* no match: do nothing */
    if (n == 1) { replace_word(t, ws, t->cur, list[0]); strv_free_list(list, 1); return; }
    size_t lcp = complete_lcp(list, n);
    if (lcp > strlen(word)) {
        char pre[PATH_MAX];
        snprintf(pre, sizeof pre, "%.*s", (int)lcp, list[0]);
        replace_word(t, ws, t->cur, pre);
    }
    /* still ambiguous: numbered list, user picks */
    char line[INSZ + 64];
    snprintf(line, sizeof line, "%s%s\n", prompt_of(t), t->in);
    tab_puts(t, line);
    for (int i = 0; i < n; i++) { char it[PATH_MAX + 16]; snprintf(it, sizeof it, "%d. %s%s", i + 1, list[i], i + 1 < n ? "  " : "\n"); tab_puts(t, it); }
    if (t->cands) strv_free_list(t->cands, t->ncands);
    t->cands = list; t->ncands = n;
    t->pick_ws = ws; t->pick_end = t->cur; t->pick_val = 0;
    t->mode = M_PICK;
}
static void pick_done(Tab *t, int val)
{
    if (val >= 1 && val <= t->ncands) replace_word(t, t->pick_ws, t->pick_end, t->cands[val - 1]);
    strv_free_list(t->cands, t->ncands);
    t->cands = NULL; t->ncands = 0; t->mode = M_NORMAL;
}

/* Ctrl+R (Task 9) */
static void do_search(Tab *t)
{
    char **res = NULL;
    char line[256];
    snprintf(line, sizeof line, "Enter search term: %s\n", t->sterm);
    tab_puts(t, line);
    int n = t->slen ? hist_search(t->sterm, &res) : 0;
    if (n == 0) tab_puts(t, "No match for search term in history\n");
    for (int i = 0; i < n; i++) { tab_puts(t, res[i]); tab_puts(t, "\n"); }
    if (res) strv_free_list(res, n);
    t->mode = M_NORMAL; t->slen = 0; t->sterm[0] = 0;
}

/* ------------------------------------------------------------------ */
/* clipboard (X11 selections)                                         */
/* ------------------------------------------------------------------ */
static void build_disp(Tab *t, size_t *cur_off);
static char *selection_text(Tab *t)
{
    size_t cur;
    build_disp(t, &cur);
    if (t->sel_on && t->sel_a != t->sel_b) {
        size_t a = (size_t)(t->sel_a < t->sel_b ? t->sel_a : t->sel_b), b = (size_t)(t->sel_a < t->sel_b ? t->sel_b : t->sel_a);
        if (b > g_dlen) b = g_dlen;
        if (a < b) { char *s = malloc(b - a + 1); memcpy(s, g_disp + a, b - a); s[b - a] = 0; return s; }
    }
    return NULL;
}
static void own_selection(char *text)
{
    free(clip_text);
    clip_text = text;
    XSetSelectionOwner(dpy, a_clip, win, CurrentTime);
    XSetSelectionOwner(dpy, a_prim, win, CurrentTime);
}
static void do_copy(Tab *t)
{
    char *s = selection_text(t);
    if (!s && t->ilen) s = strdup(t->in);             /* nothing selected: copy the input line */
    if (s) own_selection(s);
}
static void request_paste(Atom sel) { XConvertSelection(dpy, sel, a_utf8, a_prop, win, CurrentTime); }
static void paste_text(Tab *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (t->mode == M_SEARCH) { if (c >= 32 && c < 127 && t->slen < (int)sizeof t->sterm - 1) { t->sterm[t->slen++] = (char)c; t->sterm[t->slen] = 0; } continue; }
        if (c == '\n') { if (i + 1 < n) submit(t); continue; }      /* a final newline is dropped, like bracketed paste */
        if (c == '\t') c = ' ';
        if (c >= 32 && c < 127) ins_char(t, (char)c);
    }
    dirty = 1;
}
static void handle_selection_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify; ev.requestor = rq->requestor; ev.selection = rq->selection;
    ev.target = rq->target; ev.time = rq->time; ev.property = None;
    Atom prop = rq->property ? rq->property : rq->target;
    if (clip_text) {
        if (rq->target == a_targets) {
            Atom list[3] = { a_targets, a_utf8, a_string };
            XChangeProperty(dpy, rq->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
            ev.property = prop;
        } else if (rq->target == a_utf8 || rq->target == a_string) {
            XChangeProperty(dpy, rq->requestor, prop, rq->target, 8, PropModeReplace, (unsigned char *)clip_text, (int)strlen(clip_text));
            ev.property = prop;
        }
    }
    XSendEvent(dpy, rq->requestor, False, 0, (XEvent *)&ev);
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */
static unsigned long color(const char *name)
{
    XColor c, e;
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, scr), name, &c, &e)) return c.pixel;
    return BlackPixel(dpy, scr);
}
static void edit_state(Tab *t, const char **txt, int *len, int *cur)
{
    if (t->mode == M_SEARCH) { *txt = t->sterm; *len = t->slen; *cur = t->slen; }
    else { *txt = t->in; *len = t->ilen; *cur = t->cur; }
}
static void build_disp(Tab *t, size_t *cur_off)
{
    const char *pr = prompt_of(t), *tx;
    int el, ec;
    edit_state(t, &tx, &el, &ec);
    size_t need = t->olen + strlen(pr) + (size_t)el + 4;
    if (need > g_dcap) { g_dcap = need * 2; g_disp = realloc(g_disp, g_dcap); }
    size_t o = 0;
    if (t->olen) { memcpy(g_disp, t->out, t->olen); o = t->olen; }
    g_olen_vis = o;
    memcpy(g_disp + o, pr, strlen(pr)); o += strlen(pr);
    *cur_off = o + (size_t)ec;
    memcpy(g_disp + o, tx, (size_t)el); o += (size_t)el;
    g_dlen = o;
}
static void short_path(const char *cwd, char *out, size_t sz)
{
    const char *h = getenv("HOME");
    char t[PATH_MAX];
    if (h && *h && !strncmp(cwd, h, strlen(h)) && (cwd[strlen(h)] == '/' || !cwd[strlen(h)])) snprintf(t, sizeof t, "~%s", cwd + strlen(h));
    else snprintf(t, sizeof t, "%s", cwd);
    size_t l = strlen(t);
    if (l > 22) snprintf(out, sz, "...%s", t + l - 19); else snprintf(out, sz, "%s", t);
}
static void fill(int x, int y, int w, int h, unsigned long c)
{
    XSetForeground(dpy, gc, c);
    XFillRectangle(dpy, back, gc, x, y, (unsigned)w, (unsigned)h);
}
static void text_at(int x, int y, unsigned long c, const char *s, int n)
{
    if (n <= 0) return;
    XSetForeground(dpy, gc, c);
    XDrawString(dpy, back, gc, x, y, s, n);
}

static void draw(void)
{
    Tab *t = &tabs[active];
    fill(0, 0, W, H, c_bg);

    /* tab bar:  [ 1: ~/Downloads ] [ 2: /tmp ] [ + ] */
    int x = 8;
    for (int i = 0; i < ntabs; i++) {
        char sp[64], lab[128];
        short_path(tabs[i].cwd, sp, sizeof sp);
        snprintf(lab, sizeof lab, "[ %d: %s%s ]", i + 1, sp, tabs[i].busy ? " *" : "");
        int wpx = (int)strlen(lab) * cw;
        if (i == active) fill(x - 3, 2, wpx + 6, hdr_h - 4, c_tabsel);
        text_at(x, asc + 4, i == active ? c_fg : c_head, lab, (int)strlen(lab));
        tab_x0[i] = x - 3; tab_x1[i] = x + wpx + 3;
        x += wpx + cw;
    }
    if (ntabs < MAX_TABS) { text_at(x, asc + 4, c_dim, "[ + ]", 5); tab_x0[ntabs] = x - 3; tab_x1[ntabs] = x + 5 * cw + 3; }
    else tab_x0[ntabs] = tab_x1[ntabs] = -1;
    XSetForeground(dpy, gc, c_dim);
    XDrawLine(dpy, back, gc, 0, hdr_h - 1, W, hdr_h - 1);

    /* layout */
    g_cols = (W - 16) / cw; if (g_cols < 10) g_cols = 10;
    g_maxrows = (H - hdr_h - 6) / lh; if (g_maxrows < 1) g_maxrows = 1;
    size_t curoff;
    build_disp(t, &curoff);
    size_t n = g_dlen;
    if (g_rcap < n + 4) { g_rcap = (n + 4) * 2; g_rows = realloc(g_rows, g_rcap * sizeof *g_rows); }
    int nr = 1, col = 0, cur_row = 0, cur_col = 0;
    g_rows[0].s = 0;
    for (size_t i = 0;; i++) {
        int at_nl = i < n && g_disp[i] == '\n';
        if (!at_nl && col == g_cols && (i < n || i == curoff)) { g_rows[nr - 1].e = i; g_rows[nr].s = i; nr++; col = 0; }
        if (i == curoff) { cur_row = nr - 1; cur_col = col; }
        if (i == n) break;
        if (at_nl) { g_rows[nr - 1].e = i; g_rows[nr].s = i + 1; nr++; col = 0; }
        else col++;
    }
    g_rows[nr - 1].e = n;
    g_nrows = nr;
    if (t->scroll > nr - g_maxrows) t->scroll = nr - g_maxrows;
    if (t->scroll < 0) t->scroll = 0;
    int last = nr - t->scroll;                        /* one past the last visible row */
    int first = last - g_maxrows; if (first < 0) first = 0;
    g_first = first;

    size_t sa = 0, sb = 0;
    if (t->sel_on) { sa = (size_t)(t->sel_a < t->sel_b ? t->sel_a : t->sel_b); sb = (size_t)(t->sel_a < t->sel_b ? t->sel_b : t->sel_a); }
    size_t pstart = g_olen_vis, pend = g_olen_vis + strlen(prompt_of(t));
    for (int r = first; r < last; r++) {
        int top = hdr_h + 3 + (r - first) * lh, base = top + asc;
        size_t s = g_rows[r].s, e = g_rows[r].e;
        if (t->sel_on && sb > s && sa < e + 1) {
            size_t a = sa > s ? sa : s, b = sb < e ? sb : e;
            if (sb > e && g_disp[e < n ? e : n - 1] == '\n') b = e + 1 > s ? e : b;
            int x0 = 8 + (int)(a - s) * cw, wpx = (int)((b > a ? b - a : 1)) * cw;
            fill(x0, top, wpx, lh, c_selbg);
        }
        size_t bounds[4] = { s, pstart < s ? s : (pstart > e ? e : pstart), pend < s ? s : (pend > e ? e : pend), e };
        unsigned long cols[3] = { c_fg, c_prompt, c_input };
        for (int sgi = 0; sgi < 3; sgi++) {
            size_t a = bounds[sgi], b = bounds[sgi + 1];
            if (b > a) text_at(8 + (int)(a - s) * cw, base, cols[sgi], g_disp + a, (int)(b - a));
        }
    }
    if (!t->scroll && cur_row >= first && cur_row < last && !t->dead) {
        int top = hdr_h + 3 + (cur_row - first) * lh, x0 = 8 + cur_col * cw;
        fill(x0, top, cw, lh, c_cursor);
        if (curoff < n && g_disp[curoff] != '\n') text_at(x0, top + asc, c_bg, g_disp + curoff, 1);
    }
    if (t->scroll) {
        char m[64];
        snprintf(m, sizeof m, "[ scrolled up %d rows ]", t->scroll);
        text_at(W - (int)strlen(m) * cw - 8, asc + 4, c_dim, m, (int)strlen(m));
    }
    XCopyArea(dpy, back, win, gc, 0, 0, (unsigned)W, (unsigned)H, 0, 0);
    XFlush(dpy);
}

static void make_backbuffer(void)
{
    if (back) XFreePixmap(dpy, back);
    back = XCreatePixmap(dpy, win, (unsigned)W, (unsigned)H, (unsigned)DefaultDepth(dpy, scr));
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */
static size_t pos_to_off(int px, int py)
{
    int r = g_first + (py - hdr_h - 3) / lh;
    if (r < 0) r = 0;
    if (r >= g_nrows) r = g_nrows - 1;
    int c = (px - 8) / cw;
    if (c < 0) c = 0;
    size_t len = g_rows[r].e - g_rows[r].s;
    return g_rows[r].s + ((size_t)c < len ? (size_t)c : len);
}

static void do_action(Tab *t, int a)
{
    switch (a) {
    case A_LEFT:  if (t->cur > 0) t->cur--; break;
    case A_RIGHT: if (t->cur < t->ilen) t->cur++; break;
    case A_START: t->cur = 0; break;
    case A_END:   t->cur = t->ilen; break;
    case A_UP:    hist_nav(t, -1); break;
    case A_DOWN:  hist_nav(t, +1); break;
    case A_KILLS: memmove(t->in, t->in + t->cur, (size_t)(t->ilen - t->cur + 1)); t->ilen -= t->cur; t->cur = 0; break;
    case A_KILLE: t->in[t->cur] = 0; t->ilen = t->cur; break;
    case A_DELW: {
        int i = t->cur;
        while (i > 0 && isspace((unsigned char)t->in[i - 1])) i--;
        while (i > 0 && !isspace((unsigned char)t->in[i - 1])) i--;
        memmove(t->in + i, t->in + t->cur, (size_t)(t->ilen - t->cur + 1));
        t->ilen -= t->cur - i; t->cur = i;
        break;
    }
    case A_CLEAR: t->olen = 0; t->col = 0; t->scroll = 0; t->sel_on = 0; break;
    case A_EXIT:
        if (t->dead) close_tab(active);
        else if (t->busy) send_msg(t, MSG_EOF, "", 0);                    /* EOF for the running command */
        else if (t->cont) send_msg(t, MSG_INT, "", 0);
        else if (t->ilen == 0) close_tab(active);                          /* exit the shell */
        else if (t->cur < t->ilen) { memmove(t->in + t->cur, t->in + t->cur + 1, (size_t)(t->ilen - t->cur)); t->ilen--; }
        break;
    case A_COPY:  do_copy(t); break;
    case A_PASTE: request_paste(a_clip); break;
    case A_INT:
        send_msg(t, MSG_INT, t->in, (size_t)t->ilen);                      /* the engine echoes "<prompt><text>^C" */
        t->ilen = t->cur = 0; t->in[0] = 0; t->hback = 0;
        break;
    case A_TSTP:  if (t->busy) send_msg(t, MSG_TSTP, "", 0); break;
    case A_SEARCH: t->mode = M_SEARCH; t->slen = 0; t->sterm[0] = 0; break;
    case A_NEWTAB: new_tab(); break;
    case A_CLOSETAB: close_tab(active); break;
    }
}

static void handle_key(XKeyEvent *ke)
{
    Tab *t = &tabs[active];
    KeySym ks = XLookupKeysym(ke, 0);
    if (ks >= 'A' && ks <= 'Z') ks = ks - 'A' + 'a';
    unsigned mods = ke->state & MODMASK;
    KB pressed = { ks, mods };
    int act = -1;
    for (int a = 0; a < A_N; a++) if (kb_eq(kb[a], pressed)) { act = a; break; }
    char txt[32];
    KeySym k2;
    int tl = XLookupString(ke, txt, sizeof txt, &k2, NULL);

    t->scroll = (ks == XK_Prior || ks == XK_Next || ks == XK_Home || ks == XK_End) ? t->scroll : (act == A_COPY ? t->scroll : 0);

    if (t->mode == M_SEARCH) {                        /* Ctrl+R prompt */
        if (ks == XK_Return || ks == XK_KP_Enter) do_search(t);
        else if (ks == XK_Escape || act == A_INT) { t->mode = M_NORMAL; t->slen = 0; }
        else if (ks == XK_BackSpace) { if (t->slen) t->sterm[--t->slen] = 0; }
        else if (act == A_PASTE) request_paste(a_clip);
        else if (!(mods & (ControlMask | Mod1Mask)) && tl == 1 && txt[0] >= 32 && txt[0] < 127 && t->slen < (int)sizeof t->sterm - 1) { t->sterm[t->slen++] = txt[0]; t->sterm[t->slen] = 0; }
        return;
    }
    if (t->mode == M_PICK) {                          /* numbered completion list */
        if (!(mods & (ControlMask | Mod1Mask)) && tl == 1 && isdigit((unsigned char)txt[0])) {
            t->pick_val = t->pick_val * 10 + (txt[0] - '0');
            if (t->pick_val * 10 > t->ncands || t->pick_val == 0) { int v = t->pick_val; if (v == 0) { t->mode = M_NORMAL; strv_free_list(t->cands, t->ncands); t->cands = NULL; t->ncands = 0; } else pick_done(t, v); }
        } else if (ks == XK_Return || ks == XK_KP_Enter) pick_done(t, t->pick_val);
        else { strv_free_list(t->cands, t->ncands); t->cands = NULL; t->ncands = 0; t->mode = M_NORMAL; }
        return;
    }

    if (act >= 0) { do_action(t, act); return; }
    if (ks >= XK_F1 && ks <= XK_F8) { int i = (int)(ks - XK_F1); if (i < ntabs) active = i; return; }
    switch (ks) {
    case XK_Return: case XK_KP_Enter: submit(t); return;
    case XK_BackSpace: if (t->cur > 0) { memmove(t->in + t->cur - 1, t->in + t->cur, (size_t)(t->ilen - t->cur + 1)); t->cur--; t->ilen--; } return;
    case XK_Delete: if (t->cur < t->ilen) { memmove(t->in + t->cur, t->in + t->cur + 1, (size_t)(t->ilen - t->cur)); t->ilen--; } return;
    case XK_Left: do_action(t, A_LEFT); return;
    case XK_Right: do_action(t, A_RIGHT); return;
    case XK_Home: if (mods & ShiftMask) t->scroll = 1 << 28; else t->cur = 0; return;
    case XK_End:  if (mods & ShiftMask) t->scroll = 0; else t->cur = t->ilen; return;
    case XK_Prior: t->scroll += g_maxrows - 1; return;
    case XK_Next:  t->scroll -= g_maxrows - 1; return;
    case XK_Tab: do_complete(t); return;
    case XK_Escape: t->ilen = t->cur = 0; t->in[0] = 0; return;
    }
    if (!(mods & (ControlMask | Mod1Mask)) && tl == 1 && txt[0] >= 32 && txt[0] < 127) ins_char(t, txt[0]);
}

static void handle_button(XButtonEvent *be, int press)
{
    Tab *t = &tabs[active];
    if (press && be->button == Button4) { t->scroll += 3; return; }
    if (press && be->button == Button5) { t->scroll -= 3; return; }
    if (be->button == Button1) {
        if (press && be->y < hdr_h) {
            for (int i = 0; i < ntabs; i++) if (be->x >= tab_x0[i] && be->x < tab_x1[i]) { active = i; return; }
            if (ntabs < MAX_TABS && be->x >= tab_x0[ntabs] && be->x < tab_x1[ntabs]) new_tab();
            return;
        }
        if (press) { size_t o = pos_to_off(be->x, be->y); t->sel_a = t->sel_b = (long)o; t->sel_on = 1; dragging = 1; }
        else if (dragging) {
            dragging = 0;
            size_t o = pos_to_off(be->x, be->y);
            t->sel_b = (long)o;
            if (t->sel_a == t->sel_b) { t->sel_on = 0; return; }
            if (t->sel_b > t->sel_a) t->sel_b++; else t->sel_a++;
            char *s = selection_text(t);
            if (s) own_selection(s);                  /* PRIMARY: middle-click pastes it */
        }
    } else if (press && be->button == Button2) request_paste(a_prim);
}

static void handle_event(XEvent *ev)
{
    Tab *t = &tabs[active];
    switch (ev->type) {
    case Expose: dirty = 1; break;
    case ConfigureNotify:
        if (ev->xconfigure.width != W || ev->xconfigure.height != H) { W = ev->xconfigure.width; H = ev->xconfigure.height; make_backbuffer(); }
        dirty = 1;
        break;
    case KeyPress: handle_key(&ev->xkey); dirty = 1; break;
    case ButtonPress: handle_button(&ev->xbutton, 1); dirty = 1; break;
    case ButtonRelease: handle_button(&ev->xbutton, 0); dirty = 1; break;
    case MotionNotify:
        if (dragging) { t->sel_b = (long)pos_to_off(ev->xmotion.x, ev->xmotion.y); dirty = 1; }
        break;
    case SelectionRequest: handle_selection_request(&ev->xselectionrequest); break;
    case SelectionClear: break;
    case SelectionNotify:
        if (ev->xselection.property != None) {
            Atom type; int fmt; unsigned long nit, after; unsigned char *data = NULL;
            if (XGetWindowProperty(dpy, win, a_prop, 0, 16384, True, AnyPropertyType, &type, &fmt, &nit, &after, &data) == Success && data) {
                paste_text(t, (const char *)data, nit);
                XFree(data);
            }
        }
        break;
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == a_wmdel) running = 0;
        break;
    case DestroyNotify: running = 0; break;
    }
}

/* ------------------------------------------------------------------ */
/* main loop                                                          */
/* ------------------------------------------------------------------ */
static int open_font(void)
{
    static const char *names[] = {
        "-misc-fixed-medium-r-normal--15-*-*-*-c-90-iso8859-1", "9x15", "10x20", "8x13", "fixed", NULL
    };
    for (int i = 0; names[i]; i++) {
        font = XLoadQueryFont(dpy, names[i]);
        if (font) break;
    }
    if (!font) return -1;
    cw = font->max_bounds.width > 0 ? font->max_bounds.width : 8;
    asc = font->ascent;
    lh = font->ascent + font->descent + 3;
    hdr_h = lh + 10;
    return 0;
}

int gui_main(int use_process)
{
    proc_mode = use_process;
    bindings_defaults();
    if (!(dpy = XOpenDisplay(NULL))) { fprintf(stderr, "Terminator: cannot open the X11 display (is DISPLAY set?)\n"); return 1; }
    scr = DefaultScreen(dpy);
    if (open_font() < 0) { fprintf(stderr, "Terminator: cannot load a fixed-width X11 font\n"); return 1; }
    c_bg = color("#1b1d26"); c_fg = color("#e6e9f2"); c_prompt = color("#7cff6b"); c_input = color("#63e6ff");
    c_head = color("#ff7a90"); c_tabsel = color("#3b4261"); c_selbg = color("#44527a"); c_cursor = color("#f5c542"); c_dim = color("#7a8099");

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), 100, 100, (unsigned)W, (unsigned)H, 0, c_fg, c_bg);
    XStoreName(dpy, win, "Terminator - Custom Terminal");
    XSetWindowBackgroundPixmap(dpy, win, None);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | Button1MotionMask | StructureNotifyMask);
    a_wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &a_wmdel, 1);
    a_clip = XInternAtom(dpy, "CLIPBOARD", False); a_prim = XA_PRIMARY; a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False); a_prop = XInternAtom(dpy, "TERMINATOR_PASTE", False); a_string = XA_STRING;
    gc = XCreateGC(dpy, win, 0, NULL);
    XSetFont(dpy, gc, font->fid);
    make_backbuffer();

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    tab_init(&tabs[0], cwd);
    ntabs = 1;
    if (engine_start(&tabs[0], cwd) < 0) { fprintf(stderr, "Terminator: cannot start the shell\n"); return 1; }
    tab_puts(&tabs[0], "Terminator: A Custom Terminal\n");
    tab_puts(&tabs[0], proc_mode ? "Process mode: every tab is its own process.\n" : "Thread mode: every tab is a thread.\n");
    tab_puts(&tabs[0], "Ctrl+T new tab | Ctrl+Shift+W close | F1-F8 or click to switch | Tab completes | Ctrl+R searches history\n");
    tab_puts(&tabs[0], "`bind` lists and changes the hotkeys (saved in ~/.terminatorrc)\n\n");
    char cfg[4096] = "";
    load_config(cfg, sizeof cfg);
    if (*cfg) tab_puts(&tabs[0], cfg);

    XMapWindow(dpy, win);
    XFlush(dpy);

    while (running) {
        XEvent ev;
        while (XPending(dpy)) { XNextEvent(dpy, &ev); handle_event(&ev); if (!running) break; }
        if (!running) break;
        if (dirty) { dirty = 0; draw(); }
        struct pollfd p[MAX_TABS + 1];
        int idx[MAX_TABS + 1], np = 0;
        p[np].fd = ConnectionNumber(dpy); p[np].events = POLLIN; idx[np++] = -1;
        for (int i = 0; i < ntabs; i++) if (tabs[i].out_fd >= 0) { p[np].fd = tabs[i].out_fd; p[np].events = POLLIN; idx[np++] = i; }
        if (poll(p, (nfds_t)np, -1) < 0 && errno != EINTR) break;      /* blocks: no busy wait */
        int closereq = -1;
        for (int k = 1; k < np; k++)
            if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) { if (tab_read(&tabs[idx[k]])) closereq = idx[k]; }
        if (closereq >= 0) close_tab(closereq);
        dirty = 1;
    }
    for (int i = 0; i < ntabs; i++) engine_stop(&tabs[i]);
    hist_shutdown();
    XCloseDisplay(dpy);
    return 0;
}
