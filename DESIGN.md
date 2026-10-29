# Terminator - design document

## 1. Overview
```
            +---------------------------- GUI (gui.c, the only X11 code) ----------------------------+
 X server <-|  event loop: poll(X connection, tab pipes...)   per-tab transcript, line editor, hotkeys |
            +-----^-------------------------^---------------------------------^------------------------+
                  | pipes (framed msgs)     | pipes                           | pipes
            +-----+------+           +------+-----+                     +-----+------+
            | engine tab1|           | engine tab2|        ...          | engine tabN |  thread OR process
            +-----+------+           +------+-----+                     +-----+------+
                  | fork+execvp, pipe, dup2, kill        shared: history in shm + semaphore
```
Each tab = GUI-side state + one **engine** (`engine_run`, shell.c). The engine never touches X11; the
GUI never runs commands. The same `engine_run` is either a `pthread` (thread mode) or the body of a
forked process (process mode), and both talk to the GUI over the *same* pipe protocol - which is why
the two modes behave identically.

## 2. X11 GUI and event loop (gui.c)
* One window created with `XCreateSimpleWindow`; drawing goes to a back-buffer `Pixmap` using core
  font `XDrawString`, then `XCopyArea` (no flicker). A tab bar `[ 1: ~/dir ] [ 2: /tmp ] [ + ]` is drawn
  and hit-tested for mouse clicks.
* Per tab: a transcript buffer (sanitised: ANSI sequences dropped, tabs expanded, bounded to ~400 KB), the
  editable input line + cursor, history position, mode (normal / Ctrl+R / completion pick), scroll offset,
  selection. The screen is rebuilt from `transcript + prompt + input`, wrapped to the window width.
* `KeyPress` -> `XLookupKeysym` (+ `XLookupString` for text) -> action table -> edit buffer -> redraw.
* **No busy waiting:** the loop drains `XPending`, redraws if dirty, then `poll()`s the X connection fd
  and every tab's engine pipe with an infinite timeout. A flood (`yes`) is bounded to 64 KB per tab per
  iteration so key events (Ctrl+C) are still served.
* Clipboard: `XSetSelectionOwner` for CLIPBOARD and PRIMARY, `SelectionRequest` answered with TARGETS /
  UTF8_STRING / STRING; paste uses `XConvertSelection` + `SelectionNotify`.

## 3. Thread mode vs process mode
| | thread mode (default) | process mode (`useProcess=True`) |
|---|---|---|
| tab engine | `pthread_create(engine_thread)` | `fork()`; child runs `engine_run`, closes the X socket and other tabs' pipes |
| GUI <-> engine | two `pipe2(O_CLOEXEC)` pipes | the same |
| tab close | `MSG_QUIT` + `pthread_join` | `MSG_QUIT`, `waitpid` (SIGKILL after 2 s) |
| tab crash | takes the process down | pipe EOF -> tab marked dead, other tabs untouched, `Enter` restarts |
| shared state | process memory | shared memory |

**Synchronisation of shared state.** The only state shared between tabs is the history. It lives in a
POSIX shared-memory segment (`shm_open` + `ftruncate` + `mmap(MAP_SHARED)`, unlinked immediately) holding a
10,000-slot ring buffer, guarded by a **process-shared POSIX semaphore** (`sem_init(pshared=1)`). The same
segment and semaphore are used in both modes, so the code path is identical and correct for threads and
processes. Everything else (cwd, variables, jobs) is private to a tab's engine - `cd` in one tab cannot
affect another.

## 4. Process and job management (shell.c)
* **Pipeline**: N stages -> N-1 `pipe2` pipes + N `fork()`s. Child: `setpgid`, reset signals, `dup2` the pipe
  ends onto 0/1/2, close every other fd, `chdir(tab cwd)` (the process cwd is shared by threads, so each
  child enters the tab's directory), apply redirections by opening its own descriptors, `execvpe`
  (env built from the tab's exported variables). Wildcards are expanded in the parent with `glob()` against
  the tab's cwd *before* forking.
* **Output**: stdout and stderr of the whole pipeline go to one pipe the engine reads with `poll()` and
  forwards to the GUI in order. Inside `$(...)` stdout is captured and stderr still goes to the terminal.
* **Job table** per engine: process group (`pgid` = first stage), pids, state (running/stopped/done), command
  text, output pipe, stdin pipe. Foreground wait = `poll()` on {control pipe, job output pipes} plus
  `waitpid(WNOHANG|WUNTRACED)`; pipes wake it immediately, a 250 ms timeout is only a safety net.
* `cmd &` forks a **subshell** that runs the node (`&&`, `||`, pipelines, ...) in its own process group with
  stdin from `/dev/null`; the engine keeps polling its output pipe and prints `[n]+ Done ...` notices.
  Builtins/scripts inside a pipeline also run in such a forked subshell (they then write to fd 1/2).
* **Foreground stdin**: the first stage reads from a pipe fed by the engine, so text typed while a command
  runs (and `Ctrl+D` = close that pipe) reaches it like on a tty.
* **Builtins** run in the engine (so `cd`, `export`, `exit`, `source` change the tab's shell); with
  `> file` their output is captured and written by the engine.

## 5. IPC (ipc.c)
Frames `[type:1][len:4][payload]`. GUI->engine: `L` line, `c` Ctrl+C (+ unsent text, for echo), `z`
Ctrl+Z, `D` EOF, `Q` quit. Engine->GUI: `O` output, `C` clear, `S` status {busy, cont, cwd}, `X` exit,
`K` bind request. The receiver keeps a growing `IpcReader` buffer, so frames of any size and partial reads
are handled (no fixed 8 KB limit). The engine echoes the prompt + line itself, so the transcript is always
produced by one component. Redirection and pipes use `pipe2/dup2/open` as described above.

## 6. Signals
Keys arrive as X events, so the shell sends the signals itself: **Ctrl+C** -> `kill(-pgid, SIGINT)`,
**Ctrl+Z** -> `kill(-pgid, SIGTSTP)` then waits for the stop and prints `[1]+ Stopped cmd`; `fg`/`bg` send
`SIGCONT`. Because each job has its own process group, a signal never reaches another job or tab.
Terminator ignores SIGINT, SIGTSTP, SIGTTIN, SIGTTOU, SIGQUIT and SIGPIPE; children get default dispositions
back before `exec`. `multiWatch` kills each command's process group (`SIGTERM`, then `SIGKILL` after 1 s).

## 7. Data structures
* **History**: fixed ring `char items[10000][1024]` + monotonically increasing `total`; entry *k* lives in slot
  `k % 10000`. Each new command is also appended (`O_APPEND`, under the semaphore) to `~/.terminator_history`;
  the file is compacted to the last 10,000 lines on exit and loaded on start. Ctrl+R: pass 1 exact match
  newest-first; pass 2 longest common substring via a rolling-row DP (O(|term|·|cmd|) per entry), keeps the
  maximal ones when > 2 chars, newest first, de-duplicated.
* **Completion**: `opendir/readdir` -> sorted dynamic array of candidates (directories get `/`); the longest
  common prefix is computed by a column scan; the numbered list is held in the tab until a choice is made.
* **Parser/AST**: tokens keep *raw* words (quotes intact); a recursive-descent parser builds
  `LIST -> AND/OR -> PIPE -> SIMPLE` and `IF` nodes. Expansion (quotes, `$VAR`, `$?`, `$(...)`, backticks,
  field splitting, globbing) happens at execution time, so substitutions run innermost-first and scripts
  can set variables used by later lines. The same parse result decides "needs more input" (`>` prompt).
* **Scripts**: parsed completely before any command runs; run with a copy of the variables, positional
  parameters and cwd (restored afterwards), so a script cannot leak state like a child bash.
* **Hotkeys**: a table of actions -> `(KeySym, modifier mask)`; defaults, config parser, conflict
  resolver (reverts custom bindings that collide) and the writer for `~/.terminatorrc`.

## 8. Other decisions
* Only plain Xlib: the prototype's Xft/Xpm/Xrender wallpaper code was removed.
* Typed text lines are limited to 1023 ASCII characters; history entries to 1023 characters.
* Testing: `make test` (41 headless checks of parsing, pipes, redirection, scripts, job control,
  multiWatch), plus `tests/xdrive.c` which injects X11 key events into a real window under Xvfb. The
  engine was also run under AddressSanitizer/UBSan.
