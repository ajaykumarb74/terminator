/*
 * Terminator shell engine - one instance per tab.
 *
 * The engine never touches X11.  It talks to the GUI over two pipes (see terminator.h):
 *   ctl_fd  GUI -> engine : lines the user typed, Ctrl+C / Ctrl+Z / Ctrl+D, quit
 *   out_fd  engine -> GUI : terminal output, status (cwd, busy, continuation), exit requests
 * so the very same code runs as a thread (thread mode) or as a forked process (process mode).
 *
 * Contents: helpers - lexer - parser (AST) - word expansion - jobs - pipeline executor -
 *           builtins (cd, history, multiWatch, jobs/fg/bg, source, test, ...) - script
 *           interpreter - engine main loop.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "terminator.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXPIPE   32
#define MAXJOBS   32
#define MAXDEPTH  40
#define SCRIPT_MAX (4 * 1024 * 1024)

extern char **environ;

/* ======================================================================== */
/* small helpers                                                            */
/* ======================================================================== */
static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { static const char m[] = "terminator: out of memory\n"; if (write(2, m, sizeof m - 1) < 0) {} _exit(1); }
    return p;
}
static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { static const char m[] = "terminator: out of memory\n"; if (write(2, m, sizeof m - 1) < 0) {} _exit(1); }
    return p;
}
static char *xstrdup(const char *s) { size_t n = strlen(s); char *p = xmalloc(n + 1); memcpy(p, s, n + 1); return p; }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); if (n) memcpy(p, s, n); p[n] = 0; return p; }
static const char *errs(int e) { static __thread char b[128]; return strerror_r(e, b, sizeof b); }

typedef struct { char *p; size_t n, cap; } Buf;
static void b_add(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        while (nc < b->n + n + 1) nc *= 2;
        b->p = xrealloc(b->p, nc);
        b->cap = nc;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void b_addc(Buf *b, char c) { b_add(b, &c, 1); }
static void b_adds(Buf *b, const char *s) { b_add(b, s, strlen(s)); }
static void b_free(Buf *b) { free(b->p); memset(b, 0, sizeof *b); }

typedef struct { char **v; int n, cap; } Strv;
static void sv_push(Strv *s, char *p)
{
    if (s->n + 2 > s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = xrealloc(s->v, (size_t)s->cap * sizeof *s->v); }
    s->v[s->n++] = p;
    s->v[s->n] = NULL;
}
static void sv_free(Strv *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* ---------- shell variables ---------- */
typedef struct { char *name, *val; int exported; } Var;
typedef struct { Var *v; int n, cap; } Vars;

static Var *vars_find(Vars *vs, const char *name)
{
    for (int i = 0; i < vs->n; i++) if (!strcmp(vs->v[i].name, name)) return &vs->v[i];
    return NULL;
}
static void vars_set(Vars *vs, const char *name, const char *val, int exp)
{
    Var *v = vars_find(vs, name);
    if (!v) {
        if (vs->n == vs->cap) { vs->cap = vs->cap ? vs->cap * 2 : 64; vs->v = xrealloc(vs->v, (size_t)vs->cap * sizeof *vs->v); }
        v = &vs->v[vs->n++];
        v->name = xstrdup(name);
        v->val = NULL;
        v->exported = 0;
    }
    char *nv = xstrdup(val);
    free(v->val);
    v->val = nv;
    if (exp >= 0) v->exported = exp;
}
static void vars_unset(Vars *vs, const char *name)
{
    for (int i = 0; i < vs->n; i++)
        if (!strcmp(vs->v[i].name, name)) {
            free(vs->v[i].name); free(vs->v[i].val);
            vs->v[i] = vs->v[--vs->n];
            return;
        }
}
static void vars_free(Vars *vs)
{
    for (int i = 0; i < vs->n; i++) { free(vs->v[i].name); free(vs->v[i].val); }
    free(vs->v);
    memset(vs, 0, sizeof *vs);
}
static void vars_clone(Vars *dst, const Vars *src)
{
    memset(dst, 0, sizeof *dst);
    for (int i = 0; i < src->n; i++) vars_set(dst, src->v[i].name, src->v[i].val, src->v[i].exported);
}

/* ======================================================================== */
/* shell state                                                              */
/* ======================================================================== */
typedef struct Capture { Buf b; struct Capture *prev; } Capture;

enum { J_RUNNING, J_STOPPED, J_DONE };

typedef struct {
    int    used, id;                 /* id == 0: foreground job that has no number yet */
    pid_t  pgid;
    pid_t  pids[MAXPIPE];
    char   done[MAXPIPE], stopped[MAXPIPE];
    int    n, alive;
    int    bg, fg, status, sig;
    int    out_fd, err_fd, in_w;     /* read ends of output pipes, write end of its stdin pipe */
    char   cmd[256];
} Job;

typedef struct Shell {
    char      cwd[PATH_MAX];
    Vars      vars;
    char    **pos;                   /* pos[0] = $0, pos[1..] = $1.. */
    int       npos;
    int       last_status, subst_status;
    Job       jobs[MAXJOBS];
    int       ctl_fd, out_fd;
    IpcReader rd;
    Capture  *cap;                   /* command substitution capture */
    int       in_child;              /* running inside a forked subshell: write straight to fd 1/2 */
    int       fg_in_w;               /* stdin pipe of the foreground job (fed from the GUI) */
    int       intr, quit, exit_req, exit_status;
    int       busy, cont;
    Buf       cmdbuf;                /* multi-line command being assembled */
    int       last_ch;               /* last byte sent to the GUI */
    int       depth;
} Shell;

static void send_status(Shell *sh)
{
    if (sh->out_fd < 0 || sh->in_child) return;
    TabStatus s;
    memset(&s, 0, sizeof s);
    s.busy = sh->busy;
    s.cont = sh->cont;
    snprintf(s.cwd, sizeof s.cwd, "%s", sh->cwd);
    if (ipc_send(sh->out_fd, MSG_STATUS, &s, sizeof s)) { sh->quit = 1; sh->intr = 1; }
}

/* ---------- output routing ---------- */
static void gui_send(Shell *sh, const char *d, size_t n)
{
    if (sh->in_child || sh->out_fd < 0 || !n) return;
    size_t o = 0;
    while (o < n) {
        size_t c = n - o > 16384 ? 16384 : n - o;
        if (ipc_send(sh->out_fd, MSG_OUT, d + o, (uint32_t)c)) { sh->quit = 1; sh->intr = 1; return; }
        o += c;
    }
    sh->last_ch = (unsigned char)d[n - 1];
}
/* a line of shell notification: starts on a fresh line */
static void gui_note(Shell *sh, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (sh->last_ch != '\n' && sh->last_ch != 0) gui_send(sh, "\n", 1);
    gui_send(sh, buf, strlen(buf));
}
static void emit_out(Shell *sh, const char *d, size_t n)
{
    if (!n) return;
    if (sh->cap) b_add(&sh->cap->b, d, n);
    else if (sh->in_child) ipc_write_all(1, d, n);
    else gui_send(sh, d, n);
}
static void emit_err(Shell *sh, const char *d, size_t n)
{
    if (!n) return;
    if (sh->in_child) ipc_write_all(2, d, n);
    else gui_send(sh, d, n);
}
static void sayf(Shell *sh, const char *fmt, ...)      /* diagnostics (stderr) */
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit_err(sh, buf, strlen(buf));
}
static void outf(Shell *sh, const char *fmt, ...)      /* normal output (stdout) */
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    emit_out(sh, buf, strlen(buf));
}

static const char *var_get(Shell *sh, const char *name)
{
    Var *v = vars_find(&sh->vars, name);
    return v ? v->val : NULL;
}
static void resolve_path(Shell *sh, const char *p, char *out, size_t sz)
{
    if (p[0] == '/') snprintf(out, sz, "%s", p);
    else snprintf(out, sz, "%s/%s", sh->cwd, p);
}
static void set_pos(Shell *sh, const char *arg0, char **args, int nargs)
{
    for (int i = 0; i < sh->npos; i++) free(sh->pos[i]);
    free(sh->pos);
    sh->pos = xmalloc((size_t)(nargs + 2) * sizeof(char *));
    sh->pos[0] = xstrdup(arg0);
    for (int i = 0; i < nargs; i++) sh->pos[i + 1] = xstrdup(args[i]);
    sh->npos = nargs + 1;
}

/* ======================================================================== */
/* lexer                                                                    */
/* ======================================================================== */
enum { TK_WORD, TK_PIPE, TK_AND, TK_OR, TK_SEMI, TK_BG, TK_LT, TK_GT, TK_APP, TK_DUP, TK_EOF };
typedef struct { int type; char *text; int fd, dupto, line; } Tok;
typedef struct { Tok *t; int n, cap; } Toks;

static void tk_push(Toks *k, int type, const char *text, size_t tl, int fd, int dupto, int line)
{
    if (k->n == k->cap) { k->cap = k->cap ? k->cap * 2 : 32; k->t = xrealloc(k->t, (size_t)k->cap * sizeof *k->t); }
    Tok *t = &k->t[k->n++];
    t->type = type; t->text = xstrndup(text, tl); t->fd = fd; t->dupto = dupto; t->line = line;
}
static void toks_free(Toks *k)
{
    for (int i = 0; i < k->n; i++) free(k->t[i].text);
    free(k->t);
    memset(k, 0, sizeof *k);
}

/* scanners return the index just past the construct, or -1 if the input ends first */
static int scan_subst(const char *s, int i);
static int scan_squote(const char *s, int i)
{
    for (i++; s[i]; i++) if (s[i] == '\'') return i + 1;
    return -1;
}
static int scan_bquote(const char *s, int i)
{
    for (i++; s[i];) {
        if (s[i] == '\\' && s[i + 1]) { i += 2; continue; }
        if (s[i] == '`') return i + 1;
        i++;
    }
    return -1;
}
static int scan_dquote(const char *s, int i)
{
    for (i++; s[i];) {
        if (s[i] == '\\' && s[i + 1]) { i += 2; continue; }
        if (s[i] == '"') return i + 1;
        if (s[i] == '$' && s[i + 1] == '(') { int j = scan_subst(s, i); if (j < 0) return -1; i = j; continue; }
        if (s[i] == '`') { int j = scan_bquote(s, i); if (j < 0) return -1; i = j; continue; }
        i++;
    }
    return -1;
}
static int scan_subst(const char *s, int i)           /* s[i..i+1] == "$(" */
{
    for (int j = i + 2; s[j];) {
        char c = s[j];
        int k;
        if (c == '\\' && s[j + 1]) { j += 2; continue; }
        if (c == '\'') { if ((k = scan_squote(s, j)) < 0) return -1; j = k; continue; }
        if (c == '"')  { if ((k = scan_dquote(s, j)) < 0) return -1; j = k; continue; }
        if (c == '`')  { if ((k = scan_bquote(s, j)) < 0) return -1; j = k; continue; }
        if (c == '$' && s[j + 1] == '(') { if ((k = scan_subst(s, j)) < 0) return -1; j = k; continue; }
        if (c == ')') return j + 1;
        j++;
    }
    return -1;
}

enum { LEX_OK = 0, LEX_INCOMPLETE = 1, LEX_ERROR = 2 };

/* Words are kept RAW (quotes, escapes and $(...) intact); expansion happens at execution time. */
static int lex(const char *s, Toks *tk, char *err, size_t errsz)
{
    int i = 0, line = 1;
    while (s[i]) {
        char c = s[i];
        if (c == '\n') { tk_push(tk, TK_SEMI, ";", 1, 0, 0, line); line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '\\' && s[i + 1] == '\n') { i += 2; line++; continue; }
        if (c == '#') { while (s[i] && s[i] != '\n') i++; continue; }
        if (c == '|') {
            if (s[i + 1] == '|') { tk_push(tk, TK_OR, "||", 2, 0, 0, line); i += 2; }
            else { tk_push(tk, TK_PIPE, "|", 1, 0, 0, line); i++; }
            continue;
        }
        if (c == '&') {
            if (s[i + 1] == '&') { tk_push(tk, TK_AND, "&&", 2, 0, 0, line); i += 2; }
            else { tk_push(tk, TK_BG, "&", 1, 0, 0, line); i++; }
            continue;
        }
        if (c == ';') {
            if (s[i + 1] == ';') { snprintf(err, errsz, "line %d: syntax error near unexpected token `;;'", line); return LEX_ERROR; }
            tk_push(tk, TK_SEMI, ";", 1, 0, 0, line); i++;
            continue;
        }
        if (c == '(' || c == ')') {
            snprintf(err, errsz, "line %d: syntax error near unexpected token `%c' (subshell grouping is not supported)", line, c);
            return LEX_ERROR;
        }
        /* redirections: <  >  >>  2>  2>>  1>  2>&1 */
        int fd = -1, j = i;
        if ((c == '1' || c == '2') && s[i + 1] == '>') { fd = c - '0'; j = i + 1; }
        if (s[j] == '<') { tk_push(tk, TK_LT, "<", 1, 0, 0, line); i = j + 1; continue; }
        if (s[j] == '>') {
            if (fd < 0) fd = 1;
            if (s[j + 1] == '>') { tk_push(tk, TK_APP, ">>", 2, fd, 0, line); i = j + 2; }
            else if (s[j + 1] == '&' && isdigit((unsigned char)s[j + 2])) { tk_push(tk, TK_DUP, ">&", 2, fd, s[j + 2] - '0', line); i = j + 3; }
            else { tk_push(tk, TK_GT, ">", 1, fd, 0, line); i = j + 1; }
            continue;
        }
        /* a word */
        Buf w = {0};
        int wl = line;
        while (s[i]) {
            c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || strchr("|&;<>", c)) break;
            int e;
            if (c == '\\') {
                if (!s[i + 1]) { b_free(&w); return LEX_INCOMPLETE; }
                if (s[i + 1] == '\n') { i += 2; line++; continue; }      /* line continuation */
                b_addc(&w, c); b_addc(&w, s[i + 1]); i += 2;
                continue;
            }
            if (c == '\'' || c == '"' || c == '`' || (c == '$' && s[i + 1] == '(')) {
                e = c == '\'' ? scan_squote(s, i) : c == '"' ? scan_dquote(s, i) : c == '`' ? scan_bquote(s, i) : scan_subst(s, i);
                if (e < 0) { b_free(&w); return LEX_INCOMPLETE; }
                for (int k = i; k < e; k++) if (s[k] == '\n') line++;
                b_add(&w, s + i, (size_t)(e - i));
                i = e;
                continue;
            }
            if (c == '$' && s[i + 1] == '{') {
                const char *cl = strchr(s + i, '}');
                if (!cl) { b_free(&w); return LEX_INCOMPLETE; }
                e = (int)(cl - s) + 1;
                b_add(&w, s + i, (size_t)(e - i));
                i = e;
                continue;
            }
            b_addc(&w, c);
            i++;
        }
        tk_push(tk, TK_WORD, w.p ? w.p : "", w.n, 0, 0, wl);
        b_free(&w);
    }
    tk_push(tk, TK_EOF, "", 0, 0, 0, line);
    return LEX_OK;
}

/* ======================================================================== */
/* parser: tokens -> AST                                                    */
/* ======================================================================== */
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_LIST, N_IF };
enum { R_IN, R_OUT, R_APP, R_DUP };

typedef struct { int fd, kind, dupto; char *word; } Redir;

typedef struct Node Node;
struct Node {
    int    type;
    /* N_SIMPLE */
    char **words; int nwords, capw;
    Redir *redirs; int nredirs, capr;
    /* N_PIPE: kids are N_SIMPLE */
    Node **kids; int nkids, capk;
    int    negate;
    /* N_AND / N_OR */
    Node  *l, *r;
    /* N_LIST */
    Node **items; char **texts; char *bg; int nitems, capi;
    /* N_IF */
    Node **conds, **bodies; int nconds, capc;
    Node  *els;
    char  *text;                       /* source text (for job table) */
};

static Node *node_new(int type) { Node *n = xmalloc(sizeof *n); memset(n, 0, sizeof *n); n->type = type; return n; }
static void node_free(Node *n)
{
    if (!n) return;
    for (int i = 0; i < n->nwords; i++) free(n->words[i]);
    free(n->words);
    for (int i = 0; i < n->nredirs; i++) free(n->redirs[i].word);
    free(n->redirs);
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    node_free(n->l); node_free(n->r);
    for (int i = 0; i < n->nitems; i++) { node_free(n->items[i]); free(n->texts[i]); }
    free(n->items); free(n->texts); free(n->bg);
    for (int i = 0; i < n->nconds; i++) { node_free(n->conds[i]); node_free(n->bodies[i]); }
    free(n->conds); free(n->bodies);
    node_free(n->els);
    free(n->text);
    free(n);
}

typedef struct { Toks *tk; int p; int incomplete; char err[256]; } Parser;

static Tok *cur(Parser *P) { return &P->tk->t[P->p]; }
static void adv(Parser *P) { if (cur(P)->type != TK_EOF) P->p++; }
static void skip_semis(Parser *P) { while (cur(P)->type == TK_SEMI) adv(P); }
static int is_kw(Tok *t, const char *kw) { return t->type == TK_WORD && !strcmp(t->text, kw); }
static int is_stop_kw(Tok *t) { return is_kw(t, "then") || is_kw(t, "elif") || is_kw(t, "else") || is_kw(t, "fi"); }

static void perr(Parser *P, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P->err, sizeof P->err, fmt, ap);
    va_end(ap);
}
static void unexpected(Parser *P)
{
    Tok *t = cur(P);
    if (t->type == TK_EOF) { P->incomplete = 1; perr(P, "syntax error: unexpected end of input"); }
    else perr(P, "line %d: syntax error near unexpected token `%s'", t->line, t->type == TK_SEMI ? ";" : t->text);
}
static char *toks_text(Parser *P, int a, int b)
{
    Buf r = {0};
    for (int i = a; i < b; i++) {
        Tok *t = &P->tk->t[i];
        if (i > a) b_addc(&r, ' ');
        if (t->type == TK_GT || t->type == TK_APP || t->type == TK_DUP) {
            if (t->fd == 2) b_addc(&r, '2');
            b_adds(&r, t->text);
            if (t->type == TK_DUP) b_addc(&r, (char)('0' + t->dupto));
        } else b_adds(&r, t->text);
    }
    if (!r.p) return xstrdup("");
    return r.p;
}

static Node *parse_list(Parser *P, int in_block);

static int expect_kw(Parser *P, const char *kw)
{
    skip_semis(P);
    if (is_kw(cur(P), kw)) { adv(P); return 1; }
    if (cur(P)->type == TK_EOF) { P->incomplete = 1; perr(P, "syntax error: unexpected end of input (missing `%s')", kw); }
    else perr(P, "line %d: syntax error near unexpected token `%s' (expected `%s')", cur(P)->line, cur(P)->text, kw);
    return 0;
}

static Node *parse_if(Parser *P)
{
    Node *n = node_new(N_IF);
    int start = P->p;
    adv(P);                                           /* if */
    for (;;) {
        Node *cond = parse_list(P, 1);
        if (!cond) goto fail;
        if (!cond->nitems) { node_free(cond); unexpected(P); goto fail; }
        if (!expect_kw(P, "then")) { node_free(cond); goto fail; }
        Node *body = parse_list(P, 1);
        if (!body) { node_free(cond); goto fail; }
        if (!body->nitems) { node_free(cond); node_free(body); unexpected(P); goto fail; }
        if (n->nconds == n->capc) {
            n->capc = n->capc ? n->capc * 2 : 4;
            n->conds = xrealloc(n->conds, (size_t)n->capc * sizeof *n->conds);
            n->bodies = xrealloc(n->bodies, (size_t)n->capc * sizeof *n->bodies);
        }
        n->conds[n->nconds] = cond;
        n->bodies[n->nconds++] = body;
        skip_semis(P);
        if (is_kw(cur(P), "elif")) { adv(P); continue; }
        break;
    }
    if (is_kw(cur(P), "else")) {
        adv(P);
        n->els = parse_list(P, 1);
        if (!n->els) goto fail;
        if (!n->els->nitems) { unexpected(P); goto fail; }
    }
    if (!expect_kw(P, "fi")) goto fail;
    n->text = toks_text(P, start, P->p);
    return n;
fail:
    node_free(n);
    return NULL;
}

static Node *parse_simple(Parser *P)
{
    Node *n = node_new(N_SIMPLE);
    for (;;) {
        Tok *t = cur(P);
        if (t->type == TK_WORD) {
            if (n->nwords == 0 && is_stop_kw(t)) { node_free(n); unexpected(P); return NULL; }
            if (n->nwords == n->capw) { n->capw = n->capw ? n->capw * 2 : 8; n->words = xrealloc(n->words, (size_t)n->capw * sizeof *n->words); }
            n->words[n->nwords++] = xstrdup(t->text);
            adv(P);
        } else if (t->type == TK_LT || t->type == TK_GT || t->type == TK_APP || t->type == TK_DUP) {
            Redir r;
            memset(&r, 0, sizeof r);
            r.fd = t->type == TK_LT ? 0 : t->fd;
            r.kind = t->type == TK_LT ? R_IN : t->type == TK_GT ? R_OUT : t->type == TK_APP ? R_APP : R_DUP;
            r.dupto = t->dupto;
            adv(P);
            if (r.kind != R_DUP) {
                if (cur(P)->type != TK_WORD) { node_free(n); unexpected(P); return NULL; }
                r.word = xstrdup(cur(P)->text);
                adv(P);
            }
            if (n->nredirs == n->capr) { n->capr = n->capr ? n->capr * 2 : 4; n->redirs = xrealloc(n->redirs, (size_t)n->capr * sizeof *n->redirs); }
            n->redirs[n->nredirs++] = r;
        } else break;
    }
    if (!n->nwords && !n->nredirs) { node_free(n); unexpected(P); return NULL; }
    return n;
}

static Node *parse_pipeline(Parser *P)
{
    int start = P->p, neg = 0;
    while (is_kw(cur(P), "!")) { neg ^= 1; adv(P); }
    Node *first;
    if (is_kw(cur(P), "if")) {
        first = parse_if(P);
        if (!first) return NULL;
        if (cur(P)->type == TK_PIPE) { node_free(first); perr(P, "line %d: compound commands in pipelines are not supported", cur(P)->line); return NULL; }
        return first;
    }
    first = parse_simple(P);
    if (!first) return NULL;
    Node *pl = node_new(N_PIPE);
    pl->negate = neg;
    pl->kids = xmalloc(sizeof(Node *) * 4); pl->capk = 4;
    pl->kids[pl->nkids++] = first;
    while (cur(P)->type == TK_PIPE) {
        adv(P);
        skip_semis(P);
        Node *s = parse_simple(P);
        if (!s) { node_free(pl); return NULL; }
        if (pl->nkids == pl->capk) { pl->capk *= 2; pl->kids = xrealloc(pl->kids, (size_t)pl->capk * sizeof *pl->kids); }
        pl->kids[pl->nkids++] = s;
    }
    pl->text = toks_text(P, start, P->p);
    return pl;
}

static Node *parse_andor(Parser *P)
{
    Node *l = parse_pipeline(P);
    if (!l) return NULL;
    while (cur(P)->type == TK_AND || cur(P)->type == TK_OR) {
        int t = cur(P)->type == TK_AND ? N_AND : N_OR;
        adv(P);
        skip_semis(P);
        Node *r = parse_pipeline(P);
        if (!r) { node_free(l); return NULL; }
        Node *n = node_new(t);
        n->l = l; n->r = r;
        l = n;
    }
    return l;
}

static Node *parse_list(Parser *P, int in_block)
{
    Node *ls = node_new(N_LIST);
    for (;;) {
        skip_semis(P);
        Tok *t = cur(P);
        if (t->type == TK_EOF) break;
        if (in_block && is_stop_kw(t)) break;
        int start = P->p;
        Node *c = parse_andor(P);
        if (!c) { node_free(ls); return NULL; }
        char bg = 0;
        t = cur(P);
        if (t->type == TK_SEMI) adv(P);
        else if (t->type == TK_BG) { bg = 1; adv(P); }
        else if (t->type != TK_EOF && !(in_block && is_stop_kw(t))) {
            node_free(c); node_free(ls);
            unexpected(P);
            return NULL;
        }
        if (ls->nitems == ls->capi) {
            ls->capi = ls->capi ? ls->capi * 2 : 8;
            ls->items = xrealloc(ls->items, (size_t)ls->capi * sizeof *ls->items);
            ls->texts = xrealloc(ls->texts, (size_t)ls->capi * sizeof *ls->texts);
            ls->bg = xrealloc(ls->bg, (size_t)ls->capi);
        }
        ls->items[ls->nitems] = c;
        ls->texts[ls->nitems] = toks_text(P, start, bg ? P->p - 1 : P->p);
        ls->bg[ls->nitems++] = bg;
    }
    return ls;
}

/* 0 = ok, 1 = incomplete (needs more input), 2 = syntax error (message in err) */
static int parse_text(const char *text, Node **out, char *err, size_t errsz)
{
    Toks tk;
    memset(&tk, 0, sizeof tk);
    *out = NULL;
    int r = lex(text, &tk, err, errsz);
    if (r == LEX_INCOMPLETE) { snprintf(err, errsz, "syntax error: unexpected end of input (unterminated quote or substitution)"); toks_free(&tk); return 1; }
    if (r == LEX_ERROR) { toks_free(&tk); return 2; }
    Parser P;
    memset(&P, 0, sizeof P);
    P.tk = &tk;
    Node *n = parse_list(&P, 0);
    if (n && cur(&P)->type != TK_EOF) { node_free(n); n = NULL; unexpected(&P); }
    if (!n) { snprintf(err, errsz, "%s", P.err); r = P.incomplete ? 1 : 2; }
    else { *out = n; r = 0; }
    toks_free(&tk);
    return r;
}

/* ======================================================================== */
/* word expansion                                                           */
/* ======================================================================== */
static int run_text(Shell *sh, const char *text);
static char *expand_single_for_default(Shell *sh, const char *w);

typedef struct { char *s; char *q; size_t n, cap; int has; } Field;
static void fld_add(Field *f, char c, int quoted)
{
    if (f->n + 1 >= f->cap) {
        f->cap = f->cap ? f->cap * 2 : 64;
        f->s = xrealloc(f->s, f->cap);
        f->q = xrealloc(f->q, f->cap);
    }
    f->s[f->n] = c;
    f->q[f->n] = (char)quoted;
    f->n++;
}

/* glob in the tab's cwd without chdir()ing (cwd is per tab, the process cwd is shared) */
static int do_glob(Shell *sh, Field *f, Strv *out)
{
    Buf pat = {0};
    int rel = f->s[0] != '/';
    size_t skip = 0;
    if (rel) {
        for (const char *c = sh->cwd; *c; c++) {
            if (strchr("*?[]\\", *c)) b_addc(&pat, '\\');
            b_addc(&pat, *c);
        }
        if (strcmp(sh->cwd, "/")) b_addc(&pat, '/');
        skip = strcmp(sh->cwd, "/") ? strlen(sh->cwd) + 1 : 1;
    }
    for (size_t i = 0; i < f->n; i++) {
        char c = f->s[i];
        if (f->q[i] ? strchr("*?[]\\", c) != NULL : c == '\\') b_addc(&pat, '\\');
        b_addc(&pat, c);
    }
    glob_t g;
    memset(&g, 0, sizeof g);
    int rc = glob(pat.p ? pat.p : "", 0, NULL, &g);
    b_free(&pat);
    if (rc != 0 || g.gl_pathc == 0) { globfree(&g); return 0; }
    for (size_t i = 0; i < g.gl_pathc; i++) {
        const char *p = g.gl_pathv[i];
        sv_push(out, xstrdup(rel && strlen(p) >= skip ? p + skip : p));
    }
    globfree(&g);
    return 1;
}

static void fld_flush(Shell *sh, Field *f, Strv *out, int doglob)
{
    if (!f->n && !f->has) return;
    int g = 0;
    if (doglob)
        for (size_t i = 0; i < f->n; i++) if (!f->q[i] && strchr("*?[", f->s[i])) { g = 1; break; }
    if (!(g && do_glob(sh, f, out))) sv_push(out, xstrndup(f->s, f->n));
    f->n = 0;
    f->has = 0;
}

static char *cmd_subst(Shell *sh, const char *text)
{
    Capture c;
    memset(&c, 0, sizeof c);
    char cwd_save[PATH_MAX];
    snprintf(cwd_save, sizeof cwd_save, "%s", sh->cwd);
    int sx = sh->exit_req, ss = sh->exit_status, st = 0;
    if (sh->depth < MAXDEPTH) {
        c.prev = sh->cap;
        sh->cap = &c;
        sh->depth++;
        st = run_text(sh, text);
        sh->depth--;
        sh->cap = c.prev;
    }
    snprintf(sh->cwd, sizeof sh->cwd, "%s", cwd_save);   /* a substitution is a subshell: cd must not leak */
    sh->exit_req = sx; sh->exit_status = ss;
    sh->last_status = st;
    sh->subst_status = st;
    while (c.b.n && c.b.p[c.b.n - 1] == '\n') c.b.p[--c.b.n] = 0;
    return c.b.p ? c.b.p : xstrdup("");
}

static void add_value(Shell *sh, Field *f, const char *val, int quoted, Strv *out, int doglob)
{
    if (quoted) {
        for (; *val; val++) fld_add(f, *val, 1);
        f->has = 1;
    } else {
        for (; *val; val++) {
            if (isspace((unsigned char)*val)) fld_flush(sh, f, out, doglob);
            else fld_add(f, *val, 0);
        }
    }
}

/* w[0] == '$'. Returns number of characters consumed. */
static int expand_dollar(Shell *sh, const char *w, Field *f, int quoted, Strv *out, int doglob)
{
    char name[256];
    char numbuf[32];
    const char *val = NULL;
    char *alloc = NULL;
    int used = 1;

    if (w[1] == '(') {
        int e = scan_subst(w, 0);
        if (e < 0) { fld_add(f, '$', quoted); return 1; }
        char *inner = xstrndup(w + 2, (size_t)e - 3);
        alloc = cmd_subst(sh, inner);
        free(inner);
        val = alloc; used = e;
    } else if (w[1] == '{') {
        const char *end = strchr(w + 2, '}');
        if (!end) { fld_add(f, '$', quoted); return 1; }
        size_t ln = (size_t)(end - (w + 2));
        char *body = xstrndup(w + 2, ln);
        size_t k = 0;
        if (body[0] && strchr("?#", body[0])) k = 1;           /* ${?} ${#} */
        else while (body[k] && (isalnum((unsigned char)body[k]) || body[k] == '_')) k++;
        snprintf(name, sizeof name, "%.*s", (int)(k < sizeof name ? k : sizeof name - 1), body);
        const char *v = NULL;
        if (!strcmp(name, "?")) { snprintf(numbuf, sizeof numbuf, "%d", sh->last_status); v = numbuf; }
        else if (!strcmp(name, "#")) { snprintf(numbuf, sizeof numbuf, "%d", sh->npos - 1); v = numbuf; }
        else if (isdigit((unsigned char)name[0])) { int ix = atoi(name); v = ix < sh->npos ? sh->pos[ix] : NULL; }
        else v = var_get(sh, name);
        const char *rest = body + k;
        if (!strncmp(rest, ":-", 2) || *rest == '-') {
            int colon = *rest == ':';
            const char *dflt = rest + (colon ? 2 : 1);
            if (!v || (colon && !*v)) {
                alloc = expand_single_for_default(sh, dflt);
                val = alloc;
            } else val = v;
        } else val = v ? v : "";
        free(body);
        used = (int)(end - w) + 1;
    } else if (isalpha((unsigned char)w[1]) || w[1] == '_') {
        int k = 1;
        while (isalnum((unsigned char)w[k]) || w[k] == '_') k++;
        snprintf(name, sizeof name, "%.*s", k - 1 < (int)sizeof name - 1 ? k - 1 : (int)sizeof name - 1, w + 1);
        const char *v = var_get(sh, name);
        val = v ? v : "";
        used = k;
    } else if (w[1] && strchr("?#$@*!0123456789", w[1])) {
        char c = w[1];
        used = 2;
        if (c == '?') { snprintf(numbuf, sizeof numbuf, "%d", sh->last_status); val = numbuf; }
        else if (c == '#') { snprintf(numbuf, sizeof numbuf, "%d", sh->npos - 1); val = numbuf; }
        else if (c == '$') { snprintf(numbuf, sizeof numbuf, "%d", (int)getpid()); val = numbuf; }
        else if (c == '!') val = "";
        else if (isdigit((unsigned char)c)) { int ix = c - '0'; val = ix < sh->npos ? sh->pos[ix] : ""; }
        else {                                       /* $@ and $* */
            if (c == '@' && quoted) {
                for (int i = 1; i < sh->npos; i++) {
                    if (i > 1) fld_flush(sh, f, out, 0);
                    f->has = 1;
                    for (const char *p = sh->pos[i]; *p; p++) fld_add(f, *p, 1);
                }
                return used;
            }
            Buf j = {0};
            for (int i = 1; i < sh->npos; i++) { if (i > 1) b_addc(&j, ' '); b_adds(&j, sh->pos[i]); }
            alloc = j.p ? j.p : xstrdup("");
            val = alloc;
        }
    } else { fld_add(f, '$', quoted); return 1; }

    add_value(sh, f, val ? val : "", quoted, out, doglob);
    free(alloc);
    return used;
}

/* Expand one raw word.  single=1: no field splitting / globbing, yields exactly one string
 * (used for assignments and redirection targets). */
static void expand_word(Shell *sh, const char *w, Strv *out, int single)
{
    Field f = {0};
    int doglob = !single;
    int i = 0;
    if (w[0] == '~' && (w[1] == 0 || w[1] == '/')) {
        const char *h = var_get(sh, "HOME");
        if (h) { for (; *h; h++) fld_add(&f, *h, 1); f.has = 1; }
        i = 1;
    }
    while (w[i]) {
        char c = w[i];
        if (c == '\\') {
            if (w[i + 1]) { fld_add(&f, w[i + 1], 1); i += 2; } else i++;
            f.has = 1;
        } else if (c == '\'') {
            i++;
            f.has = 1;
            while (w[i] && w[i] != '\'') fld_add(&f, w[i++], 1);
            if (w[i]) i++;
        } else if (c == '"') {
            i++;
            f.has = 1;
            while (w[i] && w[i] != '"') {
                if (w[i] == '\\' && w[i + 1]) {
                    char n = w[i + 1];
                    if (n == '$' || n == '`' || n == '"' || n == '\\') { fld_add(&f, n, 1); i += 2; }
                    else if (n == '\n') { fld_add(&f, '\n', 1); i += 2; }   /* matches the assignment's sample output: the newline is kept */
                    else { fld_add(&f, '\\', 1); i++; }
                } else if (w[i] == '$') i += expand_dollar(sh, w + i, &f, 1, out, doglob);
                else if (w[i] == '`') {
                    int e = scan_bquote(w, i);
                    if (e < 0) { fld_add(&f, '`', 1); i++; continue; }
                    Buf t = {0};
                    for (int k = i + 1; k < e - 1; k++) {
                        if (w[k] == '\\' && (w[k + 1] == '`' || w[k + 1] == '\\' || w[k + 1] == '$')) k++;
                        b_addc(&t, w[k]);
                    }
                    char *r = cmd_subst(sh, t.p ? t.p : "");
                    b_free(&t);
                    add_value(sh, &f, r, 1, out, doglob);
                    free(r);
                    i = e;
                } else fld_add(&f, w[i++], 1);
            }
            if (w[i]) i++;
        } else if (c == '$') {
            i += expand_dollar(sh, w + i, &f, single, out, doglob);
        } else if (c == '`') {
            int e = scan_bquote(w, i);
            if (e < 0) { fld_add(&f, '`', 0); i++; continue; }
            Buf t = {0};
            for (int k = i + 1; k < e - 1; k++) {
                if (w[k] == '\\' && (w[k + 1] == '`' || w[k + 1] == '\\' || w[k + 1] == '$')) k++;
                b_addc(&t, w[k]);
            }
            char *r = cmd_subst(sh, t.p ? t.p : "");
            b_free(&t);
            add_value(sh, &f, r, single, out, doglob);
            free(r);
            i = e;
        } else { fld_add(&f, c, 0); i++; }
    }
    fld_flush(sh, &f, out, doglob);
    free(f.s); free(f.q);
}

/* helper used by ${VAR:-default} */
static char *expand_single_for_default(Shell *sh, const char *w)
{
    Strv t = {0};
    expand_word(sh, w, &t, 1);
    char *r = t.n ? xstrdup(t.v[0]) : xstrdup("");
    sv_free(&t);
    return r;
}

/* ======================================================================== */
/* jobs                                                                     */
/* ======================================================================== */
static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}
static void cl(int fd) { if (fd >= 0) close(fd); }

static Job *job_alloc(Shell *sh)
{
    for (int i = 0; i < MAXJOBS; i++)
        if (!sh->jobs[i].used) {
            Job *j = &sh->jobs[i];
            memset(j, 0, sizeof *j);
            j->used = 1;
            j->out_fd = j->err_fd = j->in_w = -1;
            return j;
        }
    return NULL;
}
static void job_free(Shell *sh, Job *j)
{
    if (j->in_w >= 0 && sh->fg_in_w == j->in_w) sh->fg_in_w = -1;
    cl(j->out_fd); cl(j->err_fd); cl(j->in_w);
    memset(j, 0, sizeof *j);
}
static void job_assign_id(Shell *sh, Job *j)
{
    if (j->id) return;
    for (int id = 1;; id++) {
        int taken = 0;
        for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].id == id) taken = 1;
        if (!taken) { j->id = id; return; }
    }
}
static char job_mark(Shell *sh, Job *j)
{
    int m1 = 0, m2 = 0;
    for (int i = 0; i < MAXJOBS; i++) {
        if (!sh->jobs[i].used || !sh->jobs[i].id) continue;
        int id = sh->jobs[i].id;
        if (id > m1) { m2 = m1; m1 = id; } else if (id > m2) m2 = id;
    }
    return j->id == m1 ? '+' : j->id == m2 ? '-' : ' ';
}
static void job_reap(Job *j)
{
    for (int i = 0; i < j->n; i++) {
        if (j->done[i]) continue;
        int st;
        pid_t r = waitpid(j->pids[i], &st, WNOHANG | WUNTRACED);
        if (r == j->pids[i]) {
            if (WIFSTOPPED(st)) j->stopped[i] = 1;
            else {
                j->done[i] = 1; j->alive--;
                if (i == j->n - 1) {
                    j->status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
                    j->sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
                }
            }
        } else if (r < 0 && errno == ECHILD) { j->done[i] = 1; j->alive--; }
    }
}
static int job_stopped(Job *j)
{
    if (j->alive <= 0) return 0;
    for (int i = 0; i < j->n; i++) if (!j->done[i] && !j->stopped[i]) return 0;
    return 1;
}
static void job_signal(Job *j, int sig) { if (j->pgid > 0) kill(-j->pgid, sig); }

static void job_pump(Shell *sh, Job *j)
{
    char buf[8192];
    for (int w = 0; w < 2; w++) {
        int *fdp = w ? &j->err_fd : &j->out_fd;
        for (int rounds = 0; *fdp >= 0 && rounds < 32; rounds++) {
            ssize_t r = read(*fdp, buf, sizeof buf);
            if (r > 0) {
                if (j->fg) { if (w) emit_err(sh, buf, (size_t)r); else emit_out(sh, buf, (size_t)r); }
                else gui_send(sh, buf, (size_t)r);
            } else if (r == 0) { close(*fdp); *fdp = -1; }
            else if (errno == EINTR) continue;
            else break;
        }
    }
}
static void jobs_pump_all(Shell *sh)
{
    for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used) job_pump(sh, &sh->jobs[i]);
}
static int build_pollset(Shell *sh, struct pollfd *p, int with_ctl)
{
    int n = 0;
    if (with_ctl && sh->ctl_fd >= 0) { p[n].fd = sh->ctl_fd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = &sh->jobs[i];
        if (!j->used) continue;
        if (j->out_fd >= 0) { p[n].fd = j->out_fd; p[n].events = POLLIN; p[n].revents = 0; n++; }
        if (j->err_fd >= 0) { p[n].fd = j->err_fd; p[n].events = POLLIN; p[n].revents = 0; n++; }
    }
    return n;
}

/* After a signal we sent ourselves, give the kernel a moment to report the state change. */
static void await_change(Job *j)
{
    for (int i = 0; i < 60; i++) {
        job_reap(j);
        if (j->alive == 0 || job_stopped(j)) return;
        usleep(5000);
    }
}

/* Control frames that arrive while a command runs (the GUI turns key presses into these). */
static void busy_dispatch(Shell *sh, Job *j)
{
    if (sh->ctl_fd < 0) return;
    char type, buf[INSZ + 8];
    uint32_t len;
    while (ipc_pop(&sh->rd, &type, buf, sizeof buf - 1, &len)) {
        buf[len] = 0;
        switch (type) {
        case MSG_LINE:                               /* text for the running program's stdin */
            gui_send(sh, buf, len);
            gui_send(sh, "\n", 1);
            if (sh->fg_in_w >= 0) {
                buf[len] = '\n';
                if (write(sh->fg_in_w, buf, len + 1) < 0) {}
            }
            break;
        case MSG_INT:                                /* Ctrl+C */
            sh->intr = 1;
            gui_send(sh, "^C\n", 3);
            if (j) job_signal(j, SIGINT);
            break;
        case MSG_TSTP:                               /* Ctrl+Z */
            if (j && j->fg) {
                gui_send(sh, "^Z\n", 3);
                job_signal(j, SIGTSTP);
                await_change(j);
            }
            break;
        case MSG_EOF:                                /* Ctrl+D */
            if (sh->fg_in_w >= 0) {
                for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].in_w == sh->fg_in_w) sh->jobs[i].in_w = -1;
                close(sh->fg_in_w);
                sh->fg_in_w = -1;
            }
            break;
        case MSG_QUIT:
            sh->quit = 1; sh->intr = 1;
            if (j) job_signal(j, SIGKILL);
            break;
        default: break;
        }
    }
}
static void busy_fill(Shell *sh, Job *j)
{
    if (ipc_fill(sh->ctl_fd, &sh->rd, 1) < 0) {       /* GUI went away */
        sh->quit = 1; sh->intr = 1;
        if (j) job_signal(j, SIGKILL);
        return;
    }
    busy_dispatch(sh, j);
}

static int wait_fg(Shell *sh, Job *j)
{
    int backoff = 1;
    j->fg = 1; j->bg = 0;
    sh->fg_in_w = j->in_w;
    for (;;) {
        busy_dispatch(sh, j);
        job_reap(j);
        if (j->alive == 0 || job_stopped(j)) break;
        struct pollfd p[1 + 2 * MAXJOBS];
        int n = build_pollset(sh, p, 1);
        int closed = j->out_fd < 0 && j->err_fd < 0;
        int tmo = closed ? backoff : 250;      /* pipes wake us on output/EOF; timeout is only a safety net */
        if (n) poll(p, (nfds_t)n, tmo); else usleep((useconds_t)tmo * 1000);
        if (n && sh->ctl_fd >= 0 && p[0].fd == sh->ctl_fd && (p[0].revents & (POLLIN | POLLHUP | POLLERR)))
            busy_fill(sh, j);
        jobs_pump_all(sh);
        backoff = closed ? (backoff * 2 > 50 ? 50 : backoff * 2) : 1;
    }
    jobs_pump_all(sh);
    j->fg = 0;
    if (j->alive > 0) {                              /* stopped (Ctrl+Z) */
        job_assign_id(sh, j);
        sh->fg_in_w = -1;
        gui_note(sh, "[%d]%c Stopped %s\n", j->id, job_mark(sh, j), j->cmd);
        sh->intr = 1;
        return 128 + SIGTSTP;
    }
    int st = j->status;
    job_free(sh, j);
    return st;
}

/* ======================================================================== */
/* pipeline execution                                                       */
/* ======================================================================== */
typedef struct { int fd, kind, dupto; char *path; } XRedir;
typedef struct {
    Strv   argv, assigns;
    char  *rawargs;                  /* raw text of the arguments (multiWatch) */
    XRedir *rd; int nrd;
    int    has_in;
} Stage;

static void stage_free(Stage *s)
{
    sv_free(&s->argv); sv_free(&s->assigns);
    free(s->rawargs);
    for (int i = 0; i < s->nrd; i++) free(s->rd[i].path);
    free(s->rd);
    memset(s, 0, sizeof *s);
}

static int is_assign(const char *w)
{
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_')) return 0;
    int i = 1;
    while (isalnum((unsigned char)w[i]) || w[i] == '_') i++;
    return w[i] == '=' ? i : 0;
}

static int expand_stage(Shell *sh, Node *n, Stage *st)
{
    int started = 0;
    sh->subst_status = -1;
    for (int i = 0; i < n->nwords; i++) {
        const char *w = n->words[i];
        if (!started) {
            int eq = is_assign(w);
            if (eq) {
                Strv t = {0};
                expand_word(sh, w + eq + 1, &t, 1);
                Buf b = {0};
                b_add(&b, w, (size_t)eq + 1);
                if (t.n) b_adds(&b, t.v[0]);
                sv_push(&st->assigns, b.p);
                sv_free(&t);
                continue;
            }
            started = 1;
            if (!strcmp(w, "multiWatch")) {
                Buf r = {0};
                for (int k = i + 1; k < n->nwords; k++) { if (k > i + 1) b_addc(&r, ' '); b_adds(&r, n->words[k]); }
                st->rawargs = r.p ? r.p : xstrdup("");
            }
        }
        expand_word(sh, w, &st->argv, 0);
        if (sh->intr || sh->quit) return -1;
    }
    for (int i = 0; i < n->nredirs; i++) {
        Redir *r = &n->redirs[i];
        XRedir x;
        memset(&x, 0, sizeof x);
        x.fd = r->fd; x.kind = r->kind; x.dupto = r->dupto;
        if (r->kind != R_DUP) {
            Strv t = {0};
            expand_word(sh, r->word, &t, 1);
            if (!t.n || !t.v[0][0]) { sayf(sh, "terminator: %s: ambiguous redirect\n", r->word); sv_free(&t); return -1; }
            x.path = xstrdup(t.v[0]);
            sv_free(&t);
            if (r->kind == R_IN) st->has_in = 1;
        }
        st->rd = xrealloc(st->rd, (size_t)(st->nrd + 1) * sizeof *st->rd);
        st->rd[st->nrd++] = x;
    }
    return 0;
}

static char **build_env(Shell *sh, Strv *assigns)
{
    Strv e = {0};
    for (int i = 0; i < sh->vars.n; i++) {
        Var *v = &sh->vars.v[i];
        if (!v->exported) continue;
        int over = 0;
        for (int k = 0; k < assigns->n; k++) {
            size_t nl = strlen(v->name);
            if (!strncmp(assigns->v[k], v->name, nl) && assigns->v[k][nl] == '=') over = 1;
        }
        if (over) continue;
        Buf b = {0};
        b_adds(&b, v->name); b_addc(&b, '='); b_adds(&b, v->val);
        sv_push(&e, b.p);
    }
    for (int k = 0; k < assigns->n; k++) sv_push(&e, xstrdup(assigns->v[k]));
    if (!e.v) { e.v = xmalloc(sizeof(char *)); e.v[0] = NULL; }
    return e.v;
}
static void free_env(char **env)
{
    for (char **p = env; *p; p++) free(*p);
    free(env);
}

static void child_reset_signals(void)
{
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE, SIGCHLD, SIGHUP, SIGTERM };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) signal(sigs[i], SIG_DFL);
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
}
static void close_from(int lo)
{
    long mx = sysconf(_SC_OPEN_MAX);
    if (mx < 0 || mx > 4096) mx = 4096;
    for (int fd = lo; fd < mx; fd++) close(fd);
}
/* in a forked subshell: drop every descriptor / job that belongs to the parent engine */
static void become_subshell(Shell *sh)
{
    sh->in_child = 1;
    sh->cap = NULL;
    for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used) { cl(sh->jobs[i].out_fd); cl(sh->jobs[i].err_fd); cl(sh->jobs[i].in_w); memset(&sh->jobs[i], 0, sizeof sh->jobs[i]); }
    cl(sh->ctl_fd); cl(sh->out_fd);
    sh->ctl_fd = sh->out_fd = sh->fg_in_w = -1;
    sh->intr = sh->quit = 0;
    ipc_free(&sh->rd);
}

typedef struct { const char *path, *text, *arg0; char **args; int nargs; int direct; } ScriptCall;
static int classify(Shell *sh, char **argv, ScriptCall *sc);
static int run_internal(Shell *sh, char **argv, int argc, const char *rawargs);
static int exec_node(Shell *sh, Node *n);

static int run_stages(Shell *sh, Stage *st, int ns, const char *text)
{
    /* a missing input file is reported and nothing runs */
    for (int s = 0; s < ns; s++)
        for (int k = 0; k < st[s].nrd; k++)
            if (st[s].rd[k].kind == R_IN) {
                char path[PATH_MAX * 2];
                resolve_path(sh, st[s].rd[k].path, path, sizeof path);
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0) { sayf(sh, "terminator: %s: %s\n", st[s].rd[k].path, errs(errno)); return 1; }
                close(fd);
            }

    Job *j = job_alloc(sh);
    if (!j) { sayf(sh, "terminator: too many jobs\n"); return 1; }
    snprintf(j->cmd, sizeof j->cmd, "%s", text ? text : st[0].argv.v[0]);

    int merge = !sh->cap && !sh->in_child;           /* top level: one pipe keeps stdout/stderr order */
    int op[2] = {-1, -1}, ep[2] = {-1, -1}, in_p[2] = {-1, -1};
    int use_in = sh->ctl_fd >= 0 && !st[0].has_in;
    if (pipe2(op, O_CLOEXEC) < 0 || (!merge && pipe2(ep, O_CLOEXEC) < 0) || (use_in && pipe2(in_p, O_CLOEXEC) < 0)) {
        sayf(sh, "terminator: pipe failed: %s\n", errs(errno));
        cl(op[0]); cl(op[1]); cl(ep[0]); cl(ep[1]); cl(in_p[0]); cl(in_p[1]);
        memset(j, 0, sizeof *j);
        return 1;
    }

    char **envs[MAXPIPE];
    ScriptCall dummy;
    for (int s = 0; s < ns; s++) envs[s] = build_env(sh, &st[s].assigns);

    int prev = -1, failed = 0;
    for (int s = 0; s < ns && !failed; s++) {
        int np[2] = {-1, -1};
        if (s < ns - 1 && pipe2(np, O_CLOEXEC) < 0) { sayf(sh, "terminator: pipe failed: %s\n", errs(errno)); failed = 1; break; }
        int cls = classify(sh, st[s].argv.v, &dummy);
        pid_t pid = fork();
        if (pid < 0) { sayf(sh, "terminator: fork failed: %s\n", errs(errno)); cl(np[0]); cl(np[1]); failed = 1; break; }
        if (pid == 0) {
            if (!sh->in_child) setpgid(0, s == 0 ? 0 : j->pgid);
            child_reset_signals();
            if (s == 0) { if (use_in) dup2(in_p[0], STDIN_FILENO); }
            else dup2(prev, STDIN_FILENO);
            dup2(s < ns - 1 ? np[1] : op[1], STDOUT_FILENO);
            dup2(merge ? op[1] : ep[1], STDERR_FILENO);
            close_from(3);
            char msg[PATH_MAX + 128];
            if (chdir(sh->cwd) == -1) {
                int n = snprintf(msg, sizeof msg, "terminator: cannot enter %s: %s\n", sh->cwd, errs(errno));
                if (write(2, msg, (size_t)n) < 0) {}
                _exit(1);
            }
            for (int k = 0; k < st[s].nrd; k++) {
                XRedir *r = &st[s].rd[k];
                if (r->kind == R_DUP) { dup2(r->dupto, r->fd); continue; }
                int flags = r->kind == R_IN ? O_RDONLY : r->kind == R_OUT ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND);
                int fd = open(r->path, flags, 0644);       /* each process opens its own descriptor */
                if (fd < 0) {
                    int n = snprintf(msg, sizeof msg, "terminator: %s: %s\n", r->path, errs(errno));
                    if (write(2, msg, (size_t)n) < 0) {}
                    _exit(1);
                }
                dup2(fd, r->fd);
                close(fd);
            }
            if (cls == 0) {
                const char *pv = var_get(sh, "PATH");
                if (pv) setenv("PATH", pv, 1);
                execvpe(st[s].argv.v[0], st[s].argv.v, envs[s]);
                int e = errno;
                int n = (e == ENOENT && !strchr(st[s].argv.v[0], '/'))
                    ? snprintf(msg, sizeof msg, "terminator: %s: command not found\n", st[s].argv.v[0])
                    : snprintf(msg, sizeof msg, "terminator: %s: %s\n", st[s].argv.v[0], errs(e));
                if (write(2, msg, (size_t)n) < 0) {}
                _exit(e == ENOENT ? 127 : 126);
            }
            become_subshell(sh);                           /* builtin / script inside a pipeline */
            int rc = run_internal(sh, st[s].argv.v, st[s].argv.n, st[s].rawargs);
            _exit(rc & 255);
        }
        if (s == 0) j->pgid = pid;
        if (!sh->in_child) setpgid(pid, j->pgid);
        j->pids[s] = pid;
        j->n = s + 1;
        j->alive = s + 1;
        cl(prev);
        if (s == 0) { cl(in_p[0]); in_p[0] = -1; }
        cl(np[1]);
        prev = np[0];
    }
    for (int s = 0; s < ns; s++) free_env(envs[s]);
    cl(prev); cl(op[1]); cl(ep[1]); cl(in_p[0]);

    if (failed) {
        for (int i = 0; i < j->n; i++) { kill(j->pids[i], SIGKILL); waitpid(j->pids[i], NULL, 0); }
        cl(op[0]); cl(ep[0]); cl(in_p[1]);
        memset(j, 0, sizeof *j);
        return 1;
    }
    j->out_fd = op[0]; j->err_fd = merge ? -1 : ep[0]; j->in_w = in_p[1];
    set_nonblock(j->out_fd);
    if (j->err_fd >= 0) set_nonblock(j->err_fd);
    if (j->in_w >= 0) set_nonblock(j->in_w);
    return wait_fg(sh, j);
}

/* builtin that stays in the shell; `> file` is honoured by capturing its output */
static int run_inshell(Shell *sh, Stage *st)
{
    int has_out = 0;
    for (int k = 0; k < st->nrd; k++) {
        XRedir *r = &st->rd[k];
        if (r->kind == R_IN) {
            char path[PATH_MAX * 2];
            resolve_path(sh, r->path, path, sizeof path);
            if (access(path, R_OK) != 0) { sayf(sh, "terminator: %s: %s\n", r->path, errs(errno)); return 1; }
        } else if (r->kind != R_DUP && r->fd == 1) has_out = 1;
    }
    if (!has_out) return run_internal(sh, st->argv.v, st->argv.n, st->rawargs);
    Capture c;
    memset(&c, 0, sizeof c);
    c.prev = sh->cap;
    sh->cap = &c;
    int rc = run_internal(sh, st->argv.v, st->argv.n, st->rawargs);
    sh->cap = c.prev;
    for (int k = 0; k < st->nrd; k++) {
        XRedir *r = &st->rd[k];
        if (r->kind == R_DUP || r->kind == R_IN || r->fd != 1) continue;
        char path[PATH_MAX * 2];
        resolve_path(sh, r->path, path, sizeof path);
        int fd = open(path, O_WRONLY | O_CREAT | (r->kind == R_APP ? O_APPEND : O_TRUNC), 0644);
        if (fd < 0) { sayf(sh, "terminator: %s: %s\n", r->path, errs(errno)); rc = 1; continue; }
        if (c.b.n) ipc_write_all(fd, c.b.p, c.b.n);
        close(fd);
    }
    b_free(&c.b);
    return rc;
}

static int exec_pipeline(Shell *sh, Node *n)
{
    int ns = n->nkids, status = 0;
    if (ns > MAXPIPE) { sayf(sh, "terminator: too many pipeline stages\n"); return 2; }
    Stage st[MAXPIPE];
    memset(st, 0, sizeof st);
    int ok = 1;
    for (int i = 0; i < ns; i++)
        if (expand_stage(sh, n->kids[i], &st[i]) < 0) { ok = 0; break; }
    if (!ok) status = sh->intr ? 130 : 1;
    else if (ns == 1) {
        Stage *s = &st[0];
        if (s->argv.n == 0) {
            for (int a = 0; a < s->assigns.n; a++) {
                char *eq = strchr(s->assigns.v[a], '=');
                char *name = xstrndup(s->assigns.v[a], (size_t)(eq - s->assigns.v[a]));
                vars_set(&sh->vars, name, eq + 1, -1);
                free(name);
            }
            for (int k = 0; k < s->nrd; k++) {                 /* `> file` alone creates the file */
                XRedir *r = &s->rd[k];
                if (r->kind == R_OUT || r->kind == R_APP) {
                    char path[PATH_MAX * 2];
                    resolve_path(sh, r->path, path, sizeof path);
                    int fd = open(path, O_WRONLY | O_CREAT | (r->kind == R_APP ? O_APPEND : O_TRUNC), 0644);
                    if (fd >= 0) close(fd); else { sayf(sh, "terminator: %s: %s\n", r->path, errs(errno)); status = 1; }
                }
            }
            if (!status) status = s->assigns.n && sh->subst_status >= 0 ? sh->subst_status : 0;
        } else {
            ScriptCall sc;
            if (classify(sh, s->argv.v, &sc)) status = run_inshell(sh, s);
            else status = run_stages(sh, st, 1, n->text);
        }
    } else status = run_stages(sh, st, ns, n->text);
    for (int i = 0; i < ns; i++) stage_free(&st[i]);
    if (n->negate) status = !status;
    return status;
}

static int run_background(Shell *sh, Node *n, const char *text)
{
    Job *j = job_alloc(sh);
    if (!j) { sayf(sh, "terminator: too many jobs\n"); return 1; }
    int op[2];
    if (pipe2(op, O_CLOEXEC) < 0) { sayf(sh, "terminator: pipe failed: %s\n", errs(errno)); memset(j, 0, sizeof *j); return 1; }
    pid_t pid = fork();
    if (pid < 0) { sayf(sh, "terminator: fork failed: %s\n", errs(errno)); cl(op[0]); cl(op[1]); memset(j, 0, sizeof *j); return 1; }
    if (pid == 0) {
        setpgid(0, 0);
        child_reset_signals();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); }
        dup2(op[1], STDOUT_FILENO);
        dup2(op[1], STDERR_FILENO);
        close_from(3);
        if (chdir(sh->cwd) == -1) _exit(1);
        become_subshell(sh);
        int rc = exec_node(sh, n);
        _exit(rc & 255);
    }
    setpgid(pid, pid);
    cl(op[1]);
    j->out_fd = op[0];
    set_nonblock(j->out_fd);
    j->pids[0] = pid; j->pgid = pid; j->n = 1; j->alive = 1; j->bg = 1;
    snprintf(j->cmd, sizeof j->cmd, "%s", text);
    job_assign_id(sh, j);
    gui_note(sh, "[%d] %d\n", j->id, (int)pid);
    return 0;
}

static int exec_node(Shell *sh, Node *n)
{
    int st = 0;
    switch (n->type) {
    case N_PIPE:
        return exec_pipeline(sh, n);
    case N_AND:
    case N_OR:
        st = exec_node(sh, n->l);
        sh->last_status = st;
        if (sh->intr || sh->exit_req || sh->quit) return st;
        if ((n->type == N_AND) == (st == 0)) st = exec_node(sh, n->r);
        return st;
    case N_LIST:
        st = sh->last_status;
        for (int i = 0; i < n->nitems; i++) {
            if (sh->intr || sh->exit_req || sh->quit) break;
            if (n->bg[i] && !sh->in_child && !sh->cap) st = run_background(sh, n->items[i], n->texts[i]);
            else st = exec_node(sh, n->items[i]);
            sh->last_status = st;
        }
        return st;
    case N_IF:
        for (int i = 0; i < n->nconds; i++) {
            st = exec_node(sh, n->conds[i]);
            sh->last_status = st;
            if (sh->intr || sh->exit_req || sh->quit) return st;
            if (st == 0) return exec_node(sh, n->bodies[i]);
        }
        if (n->els) return exec_node(sh, n->els);
        return 0;
    default:
        return 0;
    }
}

static int run_text(Shell *sh, const char *text)
{
    Node *n;
    char err[256];
    if (sh->depth > MAXDEPTH) { sayf(sh, "terminator: recursion too deep\n"); return 1; }
    int r = parse_text(text, &n, err, sizeof err);
    if (r) { sayf(sh, "terminator: %s\n", err); return 2; }
    sh->depth++;
    int st = exec_node(sh, n);
    sh->depth--;
    node_free(n);
    return st;
}

/* ======================================================================== */
/* builtins                                                                 */
/* ======================================================================== */
static const char *const BUILTINS[] = {
    "cd", "exit", "history", "jobs", "fg", "bg", "source", ".", "multiWatch", "export", "unset",
    "bind", "shift", "echo", "pwd", "true", "false", ":", "test", "[", "[[", "clear", "kill", NULL
};
static int is_builtin_name(const char *n)
{
    for (int i = 0; BUILTINS[i]; i++) if (!strcmp(n, BUILTINS[i])) return 1;
    return 0;
}

static int read_file(Shell *sh, const char *path, Buf *out)
{
    char full[PATH_MAX * 2];
    resolve_path(sh, path, full, sizeof full);
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat sb;
    if (fstat(fd, &sb) < 0) { int e = errno; close(fd); return e; }
    if (S_ISDIR(sb.st_mode)) { close(fd); return EISDIR; }
    char buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        if (out->n + (size_t)r > SCRIPT_MAX) { close(fd); return EFBIG; }
        b_add(out, buf, (size_t)r);
    }
    int e = r < 0 ? errno : 0;
    close(fd);
    if (!out->p) b_add(out, "", 0);
    return e;
}

static int looks_like_script(Shell *sh, const char *path)
{
    char full[PATH_MAX * 2], head[256];
    resolve_path(sh, path, full, sizeof full);
    struct stat sb;
    if (stat(full, &sb) < 0 || !S_ISREG(sb.st_mode)) return 0;
    int fd = open(full, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = read(fd, head, sizeof head - 1);
    close(fd);
    if (r >= 2 && head[0] == '#' && head[1] == '!') {
        head[r] = 0;
        char *nl = strchr(head, '\n');
        if (nl) *nl = 0;
        return strstr(head, "sh") != NULL;           /* bash, sh, dash, env bash ... */
    }
    size_t l = strlen(path);
    return l > 3 && !strcmp(path + l - 3, ".sh");
}

/* 0 = external program, 1 = builtin, 2 = shell script run by our own interpreter */
static int classify(Shell *sh, char **argv, ScriptCall *sc)
{
    memset(sc, 0, sizeof *sc);
    if (!argv[0]) return 0;
    const char *name = argv[0];
    if (is_builtin_name(name)) return 1;
    const char *slash = strrchr(name, '/');
    const char *base = slash ? slash + 1 : name;
    if (!strcmp(base, "bash") || !strcmp(base, "sh")) {
        int i = 1;
        for (; argv[i]; i++) {
            if (!strcmp(argv[i], "-c")) {
                if (!argv[i + 1]) return 0;
                sc->text = argv[i + 1];
                sc->arg0 = argv[i + 2] ? argv[i + 2] : "bash";
                sc->args = argv[i + 2] ? argv + i + 3 : argv + i + 2;
                while (sc->args[sc->nargs]) sc->nargs++;
                return 2;
            }
            if (argv[i][0] == '-' && argv[i][1]) continue;      /* -e, -x, ... ignored */
            break;
        }
        if (!argv[i]) return 0;                                   /* interactive bash: run the real one */
        sc->path = sc->arg0 = argv[i];
        sc->args = argv + i + 1;
        while (sc->args[sc->nargs]) sc->nargs++;
        return 2;
    }
    if (strchr(name, '/') && looks_like_script(sh, name)) {
        sc->path = sc->arg0 = name;
        sc->args = argv + 1;
        while (sc->args[sc->nargs]) sc->nargs++;
        sc->direct = 1;
        return 2;
    }
    return 0;
}

/* Run a script with its own variables / positional parameters / cwd (like a child bash would). */
static int run_script(Shell *sh, ScriptCall *sc)
{
    Buf text = {0};
    const char *label = sc->path ? sc->path : "bash -c";
    if (sc->path) {
        char full[PATH_MAX * 2];
        resolve_path(sh, sc->path, full, sizeof full);
        if (sc->direct && access(full, X_OK) != 0) { sayf(sh, "terminator: %s: %s\n", sc->path, errs(errno ? errno : EACCES)); return 126; }
        int e = read_file(sh, sc->path, &text);
        if (e) { sayf(sh, "terminator: %s: %s\n", sc->path, errs(e)); b_free(&text); return 127; }
    } else b_adds(&text, sc->text);

    Node *n;
    char err[256];
    int r = parse_text(text.p ? text.p : "", &n, err, sizeof err);   /* syntax errors are reported before anything runs */
    b_free(&text);
    if (r) { sayf(sh, "terminator: %s: %s\n", label, err); return 2; }
    if (sh->depth > MAXDEPTH) { node_free(n); sayf(sh, "terminator: %s: recursion too deep\n", label); return 1; }

    Vars saved;
    vars_clone(&saved, &sh->vars);
    char cwd_save[PATH_MAX];
    snprintf(cwd_save, sizeof cwd_save, "%s", sh->cwd);
    char **spos = sh->pos; int snpos = sh->npos; int slast = sh->last_status;
    sh->pos = NULL; sh->npos = 0;
    set_pos(sh, sc->arg0, sc->args, sc->nargs);

    sh->depth++;
    int st = exec_node(sh, n);
    sh->depth--;
    if (sh->exit_req) { st = sh->exit_status; sh->exit_req = 0; }

    for (int i = 0; i < sh->npos; i++) free(sh->pos[i]);
    free(sh->pos);
    sh->pos = spos; sh->npos = snpos;
    vars_free(&sh->vars);
    sh->vars = saved;
    snprintf(sh->cwd, sizeof sh->cwd, "%s", cwd_save);
    sh->last_status = slast;
    node_free(n);
    return st;
}

static int bi_source(Shell *sh, int argc, char **argv)
{
    if (argc < 2) { sayf(sh, "terminator: %s: filename argument required\n", argv[0]); return 2; }
    Buf text = {0};
    int e = read_file(sh, argv[1], &text);
    if (e == ENOENT && !strchr(argv[1], '/')) {                     /* bash also searches PATH */
        const char *path = var_get(sh, "PATH");
        char *pc = xstrdup(path ? path : "");
        for (char *sp = pc, *tok; (tok = strsep(&sp, ":")) && e == ENOENT;) {
            char cand[PATH_MAX * 2];
            snprintf(cand, sizeof cand, "%s/%s", *tok ? tok : ".", argv[1]);
            b_free(&text);
            e = read_file(sh, cand, &text);
        }
        free(pc);
    }
    if (e) { sayf(sh, "terminator: %s: %s: %s\n", argv[0], argv[1], errs(e)); b_free(&text); return 1; }
    Node *n;
    char err[256];
    int r = parse_text(text.p ? text.p : "", &n, err, sizeof err);
    b_free(&text);
    if (r) { sayf(sh, "terminator: %s: %s\n", argv[1], err); return 2; }
    if (sh->depth > MAXDEPTH) { node_free(n); sayf(sh, "terminator: source: recursion too deep\n"); return 1; }
    char **spos = NULL; int snpos = 0, replaced = 0;
    if (argc > 2) {
        spos = sh->pos; snpos = sh->npos; sh->pos = NULL; sh->npos = 0; replaced = 1;
        set_pos(sh, argv[1], argv + 2, argc - 2);
    }
    sh->depth++;
    int st = exec_node(sh, n);
    sh->depth--;
    if (replaced) {
        for (int i = 0; i < sh->npos; i++) free(sh->pos[i]);
        free(sh->pos);
        sh->pos = spos; sh->npos = snpos;
    }
    node_free(n);
    return st;                                       /* `exit` inside a sourced file leaves exit_req set */
}

static int bi_cd(Shell *sh, int argc, char **argv)
{
    const char *p = argc > 1 ? argv[1] : var_get(sh, "HOME");
    int print = 0;
    if (argc > 1 && !strcmp(argv[1], "-")) { p = var_get(sh, "OLDPWD"); print = 1; }
    if (!p || !*p) { sayf(sh, "terminator: cd: %s not set\n", argc > 1 ? "OLDPWD" : "HOME"); return 1; }
    char res[PATH_MAX * 2];
    resolve_path(sh, p, res, sizeof res);
    char *rp = realpath(res, NULL);
    if (!rp) { sayf(sh, "terminator: cd: %s: %s\n", p, errs(errno)); return 1; }
    struct stat sb;
    int rc = 0;
    if (stat(rp, &sb) < 0) { sayf(sh, "terminator: cd: %s: %s\n", p, errs(errno)); rc = 1; }
    else if (!S_ISDIR(sb.st_mode)) { sayf(sh, "terminator: cd: %s: Not a directory\n", p); rc = 1; }
    else if (access(rp, X_OK) != 0) { sayf(sh, "terminator: cd: %s: %s\n", p, errs(errno)); rc = 1; }
    else if (strlen(rp) >= PATH_MAX) { sayf(sh, "terminator: cd: path too long\n"); rc = 1; }
    if (!rc) {
        vars_set(&sh->vars, "OLDPWD", sh->cwd, 1);
        snprintf(sh->cwd, sizeof sh->cwd, "%s", rp);
        vars_set(&sh->vars, "PWD", sh->cwd, 1);
        if (print) outf(sh, "%s\n", sh->cwd);
    }
    free(rp);
    return rc;
}

static int bi_history(Shell *sh, int argc, char **argv)
{
    long show = HIST_SHOW;
    if (argc > 1) { show = atol(argv[1]); if (show < 1) show = 1; if (show > HIST_SHOW) show = HIST_SHOW; }
    long total = hist_total();
    long from = total - show + 1;
    if (from < 1) from = 1;
    char line[HIST_LEN];
    for (long s = from; s <= total; s++)
        if (hist_get_abs(s, line, sizeof line) == 0) outf(sh, "%5ld  %s\n", s, line);
    return 0;
}

static Job *find_job(Shell *sh, const char *arg)
{
    if (!arg) {
        Job *best = NULL;
        for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].id && (!best || sh->jobs[i].id > best->id)) best = &sh->jobs[i];
        return best;
    }
    if (*arg == '%') arg++;
    int id = atoi(arg);
    for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].id == id && id > 0) return &sh->jobs[i];
    return NULL;
}
static int bi_jobs(Shell *sh)
{
    for (int id = 1; id < 1000; id++) {
        Job *j = NULL;
        for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].id == id) j = &sh->jobs[i];
        if (!j) { int more = 0; for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].id > id) more = 1; if (!more) break; continue; }
        job_pump(sh, j);
        job_reap(j);
        const char *state = j->alive == 0 ? "Done" : job_stopped(j) ? "Stopped" : "Running";
        outf(sh, "[%d]%c %s %s%s\n", j->id, job_mark(sh, j), state, j->cmd, (j->alive && !job_stopped(j) && j->bg) ? " &" : "");
        if (j->alive == 0) job_free(sh, j);
    }
    return 0;
}
static int bi_fg(Shell *sh, int argc, char **argv)
{
    if (sh->in_child || sh->ctl_fd < 0) { sayf(sh, "terminator: fg: no job control in this context\n"); return 1; }
    Job *j = find_job(sh, argc > 1 ? argv[1] : NULL);
    if (!j) { sayf(sh, "terminator: fg: %s: no such job\n", argc > 1 ? argv[1] : "current"); return 1; }
    outf(sh, "%s\n", j->cmd);
    memset(j->stopped, 0, sizeof j->stopped);
    job_signal(j, SIGCONT);
    return wait_fg(sh, j);
}
static int bi_bg(Shell *sh, int argc, char **argv)
{
    if (sh->in_child || sh->ctl_fd < 0) { sayf(sh, "terminator: bg: no job control in this context\n"); return 1; }
    Job *j = find_job(sh, argc > 1 ? argv[1] : NULL);
    if (!j) { sayf(sh, "terminator: bg: %s: no such job\n", argc > 1 ? argv[1] : "current"); return 1; }
    memset(j->stopped, 0, sizeof j->stopped);
    j->bg = 1;
    job_signal(j, SIGCONT);
    outf(sh, "[%d]%c %s &\n", j->id, job_mark(sh, j), j->cmd);
    return 0;
}

static int bi_kill(Shell *sh, int argc, char **argv)
{
    static const struct { const char *n; int s; } sigs[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"TERM", SIGTERM},
        {"STOP", SIGSTOP}, {"CONT", SIGCONT}, {"TSTP", SIGTSTP}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {NULL, 0}
    };
    int sig = SIGTERM, i = 1, rc = 0;
    if (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *s = argv[i] + 1;
        if (!strncmp(s, "SIG", 3)) s += 3;
        if (isdigit((unsigned char)*s)) sig = atoi(s);
        else {
            int f = 0;
            for (int k = 0; sigs[k].n; k++) if (!strcmp(s, sigs[k].n)) { sig = sigs[k].s; f = 1; }
            if (!f) { sayf(sh, "terminator: kill: %s: invalid signal specification\n", argv[i]); return 1; }
        }
        i++;
    }
    if (i >= argc) { sayf(sh, "usage: kill [-SIGNAL] pid | %%job ...\n"); return 2; }
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            Job *j = find_job(sh, argv[i]);
            if (!j) { sayf(sh, "terminator: kill: %s: no such job\n", argv[i]); rc = 1; continue; }
            job_signal(j, sig);
            if (sig == SIGTERM || sig == SIGHUP) job_signal(j, SIGCONT);   /* let stopped jobs die */
        } else if (kill((pid_t)atoi(argv[i]), sig) < 0) { sayf(sh, "terminator: kill: (%s) - %s\n", argv[i], errs(errno)); rc = 1; }
    }
    return rc;
}

static int bi_echo(Shell *sh, int argc, char **argv)
{
    int i = 1, nl = 1, esc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *f = argv[i] + 1;
        int okflags = 1;
        for (; *f; f++) if (!strchr("neE", *f)) okflags = 0;
        if (!okflags) break;
        for (f = argv[i] + 1; *f; f++) { if (*f == 'n') nl = 0; else if (*f == 'e') esc = 1; else esc = 0; }
    }
    Buf b = {0};
    int first = i;
    for (; i < argc; i++) {
        if (i > first) b_addc(&b, ' ');
        const char *a = argv[i];
        if (!esc) { b_adds(&b, a); continue; }
        for (; *a; a++) {
            if (*a == '\\' && a[1]) {
                a++;
                switch (*a) {
                case 'n': b_addc(&b, '\n'); break;
                case 't': b_addc(&b, '\t'); break;
                case 'r': b_addc(&b, '\r'); break;
                case '\\': b_addc(&b, '\\'); break;
                case 'c': nl = 0; goto done;
                default: b_addc(&b, '\\'); b_addc(&b, *a);
                }
            } else b_addc(&b, *a);
        }
    }
done:
    if (nl) b_addc(&b, '\n');
    if (b.n) emit_out(sh, b.p, b.n);
    b_free(&b);
    return 0;
}

static int bi_export(Shell *sh, int argc, char **argv)
{
    if (argc == 1) {
        for (int i = 0; i < sh->vars.n; i++) if (sh->vars.v[i].exported) outf(sh, "export %s=\"%s\"\n", sh->vars.v[i].name, sh->vars.v[i].val);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) { char *nm = xstrndup(argv[i], (size_t)(eq - argv[i])); vars_set(&sh->vars, nm, eq + 1, 1); free(nm); }
        else { Var *v = vars_find(&sh->vars, argv[i]); if (v) v->exported = 1; else vars_set(&sh->vars, argv[i], "", 1); }
    }
    return 0;
}

/* ---- test / [ ---- */
typedef struct { Shell *sh; char **a; int n, i, err; } TP;
static int t_or(TP *t);
static int t_unary(TP *t, const char *op, const char *arg)
{
    struct stat sb;
    char full[PATH_MAX * 2];
    char c = op[1];
    if (c == 'z') return arg[0] == 0;
    if (c == 'n') return arg[0] != 0;
    resolve_path(t->sh, arg, full, sizeof full);
    int ok = (c == 'L' || c == 'h') ? lstat(full, &sb) == 0 : stat(full, &sb) == 0;
    if (!ok) return 0;
    switch (c) {
    case 'e': return 1;
    case 'f': return S_ISREG(sb.st_mode);
    case 'd': return S_ISDIR(sb.st_mode);
    case 'L': case 'h': return S_ISLNK(sb.st_mode);
    case 's': return sb.st_size > 0;
    case 'r': return access(full, R_OK) == 0;
    case 'w': return access(full, W_OK) == 0;
    case 'x': return access(full, X_OK) == 0;
    }
    return 0;
}
static int t_isint(const char *s, long *v)
{
    char *e;
    errno = 0;
    *v = strtol(s, &e, 10);
    return *s && !*e && errno == 0;
}
static int t_primary(TP *t)
{
    if (t->i >= t->n) { t->err = 1; return 0; }
    const char *a = t->a[t->i];
    if (!strcmp(a, "!")) { t->i++; return !t_primary(t); }
    if (!strcmp(a, "(")) {
        t->i++;
        int v = t_or(t);
        if (t->i < t->n && !strcmp(t->a[t->i], ")")) t->i++; else t->err = 1;
        return v;
    }
    if (t->i + 2 < t->n + 0 || t->i + 2 == t->n - 0) {}
    if (t->i + 2 < t->n || t->i + 2 == t->n) {
        /* binary operator? (needs a, op, b) */
        if (t->i + 2 <= t->n - 1) {
            const char *op = t->a[t->i + 1], *b = t->a[t->i + 2];
            if (!strcmp(op, "=") || !strcmp(op, "==")) { t->i += 3; return !strcmp(a, b); }
            if (!strcmp(op, "!=")) { t->i += 3; return strcmp(a, b) != 0; }
            if (!strcmp(op, "-eq") || !strcmp(op, "-ne") || !strcmp(op, "-lt") || !strcmp(op, "-le") || !strcmp(op, "-gt") || !strcmp(op, "-ge")) {
                long x, y;
                t->i += 3;
                if (!t_isint(a, &x) || !t_isint(b, &y)) { sayf(t->sh, "terminator: [: integer expression expected\n"); t->err = 2; return 0; }
                if (!strcmp(op, "-eq")) return x == y;
                if (!strcmp(op, "-ne")) return x != y;
                if (!strcmp(op, "-lt")) return x < y;
                if (!strcmp(op, "-le")) return x <= y;
                if (!strcmp(op, "-gt")) return x > y;
                return x >= y;
            }
        }
    }
    if (a[0] == '-' && a[1] && !a[2] && strchr("efdrwxsznLh", a[1]) && t->i + 1 < t->n) {
        const char *arg = t->a[t->i + 1];
        t->i += 2;
        return t_unary(t, a, arg);
    }
    t->i++;
    return a[0] != 0;
}
static int t_and(TP *t)
{
    int v = t_primary(t);
    while (t->i < t->n && !strcmp(t->a[t->i], "-a")) { t->i++; int r = t_primary(t); v = v && r; }
    return v;
}
static int t_or(TP *t)
{
    int v = t_and(t);
    while (t->i < t->n && !strcmp(t->a[t->i], "-o")) { t->i++; int r = t_and(t); v = v || r; }
    return v;
}
static int bi_test(Shell *sh, int argc, char **argv)
{
    int n = argc - 1;
    char **a = argv + 1;
    if (!strcmp(argv[0], "[") || !strcmp(argv[0], "[[")) {
        const char *close = argv[0][1] ? "]]" : "]";
        if (n < 1 || strcmp(a[n - 1], close)) { sayf(sh, "terminator: %s: missing `%s'\n", argv[0], close); return 2; }
        n--;
    }
    if (n == 0) return 1;
    TP t = { sh, a, n, 0, 0 };
    int v = t_or(&t);
    if (t.err == 1 || (!t.err && t.i < t.n)) { sayf(sh, "terminator: %s: syntax error in test expression\n", argv[0]); return 2; }
    if (t.err) return 2;
    return v ? 0 : 1;
}

/* ---- multiWatch ---- */
#define MAXW 16
typedef struct { char *cmd; pid_t pid; int fd; long next; int interval; Buf acc; } Watch;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* multiWatch [-n secs] ["cmd1", "cmd2", ...] */
static int parse_watch_list(const char *raw, Strv *cmds, int *interval)
{
    const char *p = raw;
    while (isspace((unsigned char)*p)) p++;
    while (!strncmp(p, "-n", 2)) {
        p += 2;
        while (isspace((unsigned char)*p)) p++;
        *interval = atoi(p);
        while (isdigit((unsigned char)*p)) p++;
        while (isspace((unsigned char)*p)) p++;
    }
    if (*p != '[') return -1;
    p++;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == ']') break;
        if (!*p) return -1;
        Buf item = {0};
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) {
                if (q == '"' && *p == '\\' && p[1]) p++;
                b_addc(&item, *p++);
            }
            if (!*p) { b_free(&item); return -1; }
            p++;
        } else {
            while (*p && *p != ',' && *p != ']') b_addc(&item, *p++);
            while (item.n && isspace((unsigned char)item.p[item.n - 1])) item.p[--item.n] = 0;
        }
        sv_push(cmds, item.p ? item.p : xstrdup(""));
        while (isspace((unsigned char)*p)) p++;
        if (*p == ',') p++;
        else if (*p != ']') return -1;
    }
    return 0;
}
static void watch_start(Shell *sh, Watch *w)
{
    int pp[2];
    if (pipe2(pp, O_CLOEXEC) < 0) { w->next = now_ms() + 1000; return; }
    pid_t pid = fork();
    if (pid < 0) { cl(pp[0]); cl(pp[1]); w->next = now_ms() + 1000; return; }
    if (pid == 0) {
        setpgid(0, 0);                                /* one group per command so kill(-pgid) reaches its whole pipeline */
        child_reset_signals();
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, STDIN_FILENO);
        dup2(pp[1], STDOUT_FILENO);
        dup2(pp[1], STDERR_FILENO);
        close_from(3);
        if (chdir(sh->cwd) == -1) _exit(1);
        become_subshell(sh);
        int st = run_text(sh, w->cmd);
        _exit(st & 255);
    }
    setpgid(pid, pid);
    cl(pp[1]);
    w->pid = pid; w->fd = pp[0];
    set_nonblock(w->fd);
    w->acc.n = 0;
    if (w->acc.p) w->acc.p[0] = 0;
}
static void watch_print(Shell *sh, Watch *w, const char *data, size_t n)
{
    static const char dash[] = "----------------------------------------------------";
    Buf b = {0};
    char hdr[64];
    b_addc(&b, '"'); b_adds(&b, w->cmd);
    snprintf(hdr, sizeof hdr, "\", %ld:\n", (long)time(NULL));
    b_adds(&b, hdr);
    b_adds(&b, dash); b_addc(&b, '\n');
    b_add(&b, data, n);
    if (n && data[n - 1] != '\n') b_addc(&b, '\n');
    b_adds(&b, dash); b_addc(&b, '\n');
    emit_out(sh, b.p, b.n);                            /* one write: blocks never interleave */
    b_free(&b);
}
static void watch_read(Shell *sh, Watch *w)
{
    char buf[4096];
    int eof = 0;
    for (int r = 0; r < 16; r++) {
        ssize_t k = read(w->fd, buf, sizeof buf);
        if (k > 0) b_add(&w->acc, buf, (size_t)k);
        else if (k == 0) { eof = 1; break; }
        else if (errno == EINTR) continue;
        else break;
    }
    size_t flush = 0;
    if (eof) flush = w->acc.n;
    else for (size_t i = w->acc.n; i > 0; i--) if (w->acc.p[i - 1] == '\n') { flush = i; break; }
    if (flush) {
        watch_print(sh, w, w->acc.p, flush);
        memmove(w->acc.p, w->acc.p + flush, w->acc.n - flush);
        w->acc.n -= flush;
        w->acc.p[w->acc.n] = 0;
    }
    if (eof) {
        cl(w->fd); w->fd = -1;
        if (w->pid > 0) waitpid(w->pid, NULL, 0);
        w->pid = -1;
        w->next = now_ms() + (long)w->interval * 1000;
    }
}
static int bi_multiwatch(Shell *sh, const char *raw)
{
    Strv cmds = {0};
    int interval = 2;
    if (parse_watch_list(raw ? raw : "", &cmds, &interval) < 0 || cmds.n == 0 || cmds.n > MAXW) {
        sayf(sh, "usage: multiWatch [-n secs] [\"cmd1\", \"cmd2\", ...]   (at most %d commands)\n", MAXW);
        sv_free(&cmds);
        return 1;
    }
    if (interval < 1) interval = 2;
    Watch w[MAXW];
    memset(w, 0, sizeof w);
    for (int i = 0; i < cmds.n; i++) { w[i].cmd = cmds.v[i]; w[i].pid = -1; w[i].fd = -1; w[i].interval = interval; }

    while (!sh->intr && !sh->quit) {
        busy_dispatch(sh, NULL);
        if (sh->intr || sh->quit) break;
        long now = now_ms(), tmo = -1;
        for (int i = 0; i < cmds.n; i++) if (w[i].pid < 0 && now >= w[i].next) watch_start(sh, &w[i]);
        struct pollfd p[MAXW + 1];
        int idx[MAXW + 1], np = 0, has_ctl = 0;
        if (sh->ctl_fd >= 0) { p[np].fd = sh->ctl_fd; p[np].events = POLLIN; p[np].revents = 0; idx[np++] = -1; has_ctl = 1; }
        for (int i = 0; i < cmds.n; i++) {
            if (w[i].pid >= 0 && w[i].fd >= 0) { p[np].fd = w[i].fd; p[np].events = POLLIN; p[np].revents = 0; idx[np++] = i; }
            else if (w[i].pid < 0) { long d = w[i].next - now; if (d < 0) d = 0; if (tmo < 0 || d < tmo) tmo = d; }
        }
        if (poll(p, (nfds_t)np, (int)tmo) < 0 && errno != EINTR) break;     /* select()/poll(): no busy waiting */
        for (int k = 0; k < np; k++) {
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (idx[k] < 0) busy_fill(sh, NULL);
            else watch_read(sh, &w[idx[k]]);
        }
        (void)has_ctl;
    }
    for (int i = 0; i < cmds.n; i++) if (w[i].pid > 0) kill(-w[i].pid, SIGTERM);   /* Ctrl+C: stop every child */
    for (int i = 0; i < cmds.n; i++) {
        if (w[i].pid > 0) {
            int tries = 0;
            while (waitpid(w[i].pid, NULL, WNOHANG) == 0 && tries++ < 100) usleep(10000);
            if (tries > 100) { kill(-w[i].pid, SIGKILL); waitpid(w[i].pid, NULL, 0); }
        }
        cl(w[i].fd);
        b_free(&w[i].acc);
    }
    sv_free(&cmds);
    return sh->intr ? 130 : 0;
}

static int run_internal(Shell *sh, char **argv, int argc, const char *rawargs)
{
    ScriptCall sc;
    if (classify(sh, argv, &sc) == 2) return run_script(sh, &sc);
    const char *c = argv[0];
    if (!strcmp(c, "cd")) return bi_cd(sh, argc, argv);
    if (!strcmp(c, "exit")) {
        if (argc > 1) sh->exit_status = atoi(argv[1]) & 255; else sh->exit_status = sh->last_status & 255;
        sh->exit_req = 1;
        return sh->exit_status;
    }
    if (!strcmp(c, "history")) return bi_history(sh, argc, argv);
    if (!strcmp(c, "jobs")) return bi_jobs(sh);
    if (!strcmp(c, "fg")) return bi_fg(sh, argc, argv);
    if (!strcmp(c, "bg")) return bi_bg(sh, argc, argv);
    if (!strcmp(c, "source") || !strcmp(c, ".")) return bi_source(sh, argc, argv);
    if (!strcmp(c, "multiWatch")) return bi_multiwatch(sh, rawargs);
    if (!strcmp(c, "export")) return bi_export(sh, argc, argv);
    if (!strcmp(c, "unset")) { for (int i = 1; i < argc; i++) vars_unset(&sh->vars, argv[i]); return 0; }
    if (!strcmp(c, "shift")) {
        int n = argc > 1 ? atoi(argv[1]) : 1;
        if (n < 0 || n > sh->npos - 1) return 1;
        for (int i = 0; i < n; i++) { free(sh->pos[1]); memmove(&sh->pos[1], &sh->pos[2], (size_t)(sh->npos - 2) * sizeof(char *)); sh->npos--; }
        return 0;
    }
    if (!strcmp(c, "echo")) return bi_echo(sh, argc, argv);
    if (!strcmp(c, "kill")) return bi_kill(sh, argc, argv);
    if (!strcmp(c, "pwd")) { outf(sh, "%s\n", sh->cwd); return 0; }
    if (!strcmp(c, "true") || !strcmp(c, ":")) return 0;
    if (!strcmp(c, "false")) return 1;
    if (!strcmp(c, "test") || !strcmp(c, "[") || !strcmp(c, "[[")) return bi_test(sh, argc, argv);
    if (!strcmp(c, "clear")) { if (!sh->in_child && sh->out_fd >= 0) ipc_send(sh->out_fd, MSG_CLEAR, NULL, 0); return 0; }
    if (!strcmp(c, "bind")) {
        if (sh->in_child || sh->out_fd < 0) { sayf(sh, "terminator: bind: only available at the prompt\n"); return 1; }
        Buf b = {0};
        for (int i = 1; i < argc; i++) { if (i > 1) b_addc(&b, ' '); b_adds(&b, argv[i]); }
        ipc_send(sh->out_fd, MSG_BIND, b.p ? b.p : "", (uint32_t)b.n);
        b_free(&b);
        return 0;
    }
    sayf(sh, "terminator: %s: not a builtin\n", c);
    return 1;
}

/* ======================================================================== */
/* engine main loop                                                         */
/* ======================================================================== */
static void shell_init(Shell *sh, int ctl, int out, const char *cwd)
{
    memset(sh, 0, sizeof *sh);
    sh->ctl_fd = ctl; sh->out_fd = out; sh->fg_in_w = -1; sh->last_ch = '\n'; sh->subst_status = -1;
    snprintf(sh->cwd, sizeof sh->cwd, "%s", cwd && *cwd ? cwd : "/");
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        char *nm = xstrndup(*e, (size_t)(eq - *e));
        vars_set(&sh->vars, nm, eq + 1, 1);
        free(nm);
    }
    vars_set(&sh->vars, "PWD", sh->cwd, 1);
    if (!var_get(sh, "HOME")) vars_set(&sh->vars, "HOME", "/", 1);
    if (!var_get(sh, "PATH")) vars_set(&sh->vars, "PATH", "/usr/local/bin:/usr/bin:/bin", 1);
    sh->pos = xmalloc(2 * sizeof(char *));
    sh->pos[0] = xstrdup("terminator");
    sh->npos = 1;
    for (int i = 0; i < MAXJOBS; i++) sh->jobs[i].out_fd = sh->jobs[i].err_fd = sh->jobs[i].in_w = -1;
}

static void hist_record(const char *cmd)
{
    Buf b = {0};
    for (const char *p = cmd; *p; p++) {
        if (*p == '\\' && p[1] == '\n') { p++; continue; }
        if (*p == '\n') {                           /* keep the command valid when recalled */
            while (b.n && b.p[b.n - 1] == ' ') b.p[--b.n] = 0;
            int glue = !b.n || strchr(";|&", b.p[b.n - 1]) ||
                       (b.n >= 4 && !strcmp(b.p + b.n - 4, "then")) || (b.n >= 4 && !strcmp(b.p + b.n - 4, "else"));
            b_adds(&b, glue ? " " : "; ");
        } else b_addc(&b, *p);
    }
    if (b.p) hist_add(b.p);
    b_free(&b);
}

static void engine_line(Shell *sh, const char *line)
{
    if (sh->last_ch != '\n') gui_send(sh, "\n", 1);
    gui_send(sh, sh->cont ? CPROMPT : PROMPT, strlen(sh->cont ? CPROMPT : PROMPT));   /* echo what the user entered */
    gui_send(sh, line, strlen(line));
    gui_send(sh, "\n", 1);

    if (sh->cmdbuf.n) b_addc(&sh->cmdbuf, '\n');
    b_adds(&sh->cmdbuf, line);

    Node *n = NULL;
    char err[256];
    int r = parse_text(sh->cmdbuf.p, &n, err, sizeof err);
    if (r == 1) {                                    /* incomplete: show ">" and keep reading */
        sh->cont = 1;
        send_status(sh);
        return;
    }
    char *cmd = xstrdup(sh->cmdbuf.p);
    b_free(&sh->cmdbuf);
    sh->cont = 0;
    if (r == 2) {
        hist_record(cmd);
        gui_note(sh, "terminator: %s\n", err);
        sh->last_status = 2;
    } else {
        hist_record(cmd);
        sh->busy = 1;
        send_status(sh);
        sh->intr = 0;
        sh->last_status = exec_node(sh, n);
        sh->intr = 0;
        sh->busy = 0;
        node_free(n);
    }
    free(cmd);
    if (sh->last_ch != '\n') gui_send(sh, "\n", 1);
    send_status(sh);
}

static void engine_interrupt(Shell *sh, const char *partial)
{
    if (sh->last_ch != '\n') gui_send(sh, "\n", 1);
    const char *pr = sh->cont ? CPROMPT : PROMPT;
    gui_send(sh, pr, strlen(pr));
    gui_send(sh, partial, strlen(partial));
    gui_send(sh, "^C\n", 3);
    b_free(&sh->cmdbuf);
    sh->cont = 0;
    send_status(sh);
}

static void idle_dispatch(Shell *sh)
{
    char type, buf[INSZ + 8];
    uint32_t len;
    while (!sh->quit && ipc_pop(&sh->rd, &type, buf, sizeof buf - 1, &len)) {
        buf[len] = 0;
        if (type == MSG_LINE) engine_line(sh, buf);
        else if (type == MSG_INT) engine_interrupt(sh, buf);
        else if (type == MSG_QUIT) sh->quit = 1;
        if (sh->exit_req) {
            int st = sh->exit_status;
            ipc_send(sh->out_fd, MSG_EXIT, &st, sizeof st);
            sh->quit = 1;
        }
    }
}

static void jobs_service(Shell *sh)
{
    jobs_pump_all(sh);
    for (int i = 0; i < MAXJOBS; i++) {
        Job *j = &sh->jobs[i];
        if (!j->used || !j->id || j->fg) continue;
        job_reap(j);
        if (j->alive > 0) continue;
        job_pump(sh, j);
        const char *what = j->sig ? "Terminated" : "Done";
        char ex[32] = "";
        if (!j->sig && j->status) { snprintf(ex, sizeof ex, "Exit %d", j->status); what = ex; }
        gui_note(sh, "[%d]%c %s %s\n", j->id, job_mark(sh, j), what, j->cmd);
        job_free(sh, j);
    }
}

void engine_run(int ctl_fd, int out_fd, const char *cwd)
{
    Shell *sh = xmalloc(sizeof *sh);
    shell_init(sh, ctl_fd, out_fd, cwd);
    send_status(sh);
    while (!sh->quit) {
        idle_dispatch(sh);
        if (sh->quit) break;
        struct pollfd p[1 + 2 * MAXJOBS];
        int n = build_pollset(sh, p, 1), live = 0;
        for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].alive > 0 && !job_stopped(&sh->jobs[i])) live = 1;
        int r = poll(p, (nfds_t)n, live ? 500 : -1);      /* pipes wake us; the timeout only covers exotic cases */
        if (r < 0 && errno != EINTR) break;
        if (r > 0 && p[0].fd == sh->ctl_fd && (p[0].revents & (POLLIN | POLLHUP | POLLERR)))
            if (ipc_fill(sh->ctl_fd, &sh->rd, 1) < 0) break;
        jobs_service(sh);
    }
    /* closing the tab: take its jobs down with it */
    for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used && sh->jobs[i].alive > 0) { job_signal(&sh->jobs[i], SIGHUP); job_signal(&sh->jobs[i], SIGCONT); }
    for (int t = 0; t < 30; t++) {
        int left = 0;
        for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used) { job_reap(&sh->jobs[i]); if (sh->jobs[i].alive > 0) left = 1; }
        if (!left) break;
        usleep(10000);
    }
    for (int i = 0; i < MAXJOBS; i++) if (sh->jobs[i].used) { if (sh->jobs[i].alive > 0) { job_signal(&sh->jobs[i], SIGKILL); for (int k = 0; k < sh->jobs[i].n; k++) waitpid(sh->jobs[i].pids[k], NULL, 0); } job_free(sh, &sh->jobs[i]); }
    cl(sh->ctl_fd); cl(sh->out_fd);
    for (int i = 0; i < sh->npos; i++) free(sh->pos[i]);
    free(sh->pos);
    vars_free(&sh->vars);
    b_free(&sh->cmdbuf);
    ipc_free(&sh->rd);
    free(sh);
}
