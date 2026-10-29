# Terminator - a custom terminal and shell

Terminator is a terminal emulator **and** shell written in C for Linux. It draws its own X11 window
(Xlib only), has multiple tabs, and interprets commands itself: pipes, redirection, quoting and
command substitution, job control, `multiWatch`, configurable hotkeys, shared searchable history,
file-name completion and a small bash-script interpreter. It was written for the *Computing Lab
(CS69201)* term project.

Only the facilities named in the assignment are used: Xlib, `fork/execvp/wait`, `pipe/dup2`,
`open/read/write`, `poll`, `signal/kill`, POSIX threads, POSIX semaphores and shared memory.

## Dependencies

| Package (Debian/Ubuntu) | Why |
|---|---|
| `build-essential` | gcc, make |
| `libx11-dev` | Xlib |
| `pkg-config` | optional, finds the X11 flags (the Makefile falls back to `-lX11`) |

```
sudo apt install build-essential libx11-dev pkg-config
```
Fedora: `sudo dnf install gcc make libX11-devel`.

## Build and run

```
make                         # builds ./terminator  (-lX11 -lpthread -lrt)
./terminator                 # thread mode (default): one process, each tab is a thread
./terminator useProcess=True # process mode: each tab is its own forked process
make test                    # headless engine tests (no X server needed)
```
Both modes behave the same; commands typed in a tab always run as child processes
(`fork()` + `execvp()`). Needs a running X server (`DISPLAY` set).

## Using it

| Action | Default key |
|---|---|
| New tab / close tab | `Ctrl+T` / `Ctrl+Shift+W` (or `Ctrl+D` on an empty prompt) |
| Switch tab | `F1`-`F8`, or click the tab bar (`[ + ]` opens a tab) |
| Move in line / start / end | `Left` `Right` / `Ctrl+A` / `Ctrl+E` (also `Home`/`End`) |
| History | `Up` / `Down`, `Ctrl+R` search |
| Delete to start / end / previous word | `Ctrl+U` / `Ctrl+K` / `Ctrl+W` |
| Clear screen | `Ctrl+L` (or `clear`) |
| Stop / suspend running command | `Ctrl+C` / `Ctrl+Z` |
| EOF to a running command / close tab | `Ctrl+D` |
| Copy / paste (X11 selections) | `Ctrl+Shift+C` / `Ctrl+Shift+V`; mouse-drag selects, middle-click pastes |
| Scroll | mouse wheel, `PageUp`/`PageDown`, `Shift+Home`/`Shift+End` |
| Complete a file name | `Tab` |

### Customising hotkeys
`bind` lists the bindings, `bind <action> <key>` changes one, `bind reset` restores the defaults.
Changes are written to `~/.terminatorrc` (override with `$TERMINATOR_RC`) and loaded at start-up, so
they survive restarts and need no recompiling. The file can also be edited by hand
(`sample/terminatorrc`):
```
bind line_start = Ctrl+B
bind clear_screen = Ctrl+Shift+L
```
Actions: `left right history_up history_down line_start line_end kill_to_start kill_to_end
delete_word clear_screen exit_shell copy paste interrupt suspend history_search new_tab close_tab`.
An invalid key, an unknown modifier, a plain printable key (which would stop it typing), or two
actions on the same key are **reported** (in the tab / at start-up) and the affected action falls
back to its default.

## Implemented features

* **Task 1** X11 window, per-tab transcript and prompt, scrolling, wrapped lines, tabs that keep
  running in the background, no busy-waiting (the event loop `poll()`s X and every tab pipe).
* **Task 2** `cd` (`cd -`, `~`), `exit`, `history`, `multiWatch`, `jobs/fg/bg`, `source`/`.`, plus
  `export unset shift echo pwd test [ kill clear bind`. `$?` is kept. Unknown commands print
  `terminator: x: command not found`.
* **Task 3** multi-line input with the `>` prompt for open quotes, trailing `\`, unterminated
  `$(`/backticks (nested and multi-line), unfinished `if`, trailing `|`/`&&`/`||`. Single quotes are
  literal; double quotes expand `$VAR`, `$?`, `$(...)`, backticks.
* **Task 4** `<`, `>`, `>>`, `2>`, `2>&1`, combined, via `dup2`. A missing input file is reported and
  the command is not run. Each process opens its own descriptor, so background jobs reading the
  same file never share an offset.
* **Task 5** pipelines of any length (N-1 pipes, N children, up to 32 stages), combined with redirection.
* **Task 6** `multiWatch ["cmd1", "cmd2"]` (optional `-n secs`): one process and one pipe per command,
  one `poll()` over all of them, output printed in the required framed format, commands re-run every
  2 s, `Ctrl+C` terminates every child and the shell keeps running.
* **Task 7** the hotkeys above, configurable and persistent (see above).
* **Task 8** `Ctrl+C`/`Ctrl+Z` are X11 key events turned into `kill()` on the job's process group;
  `cmd &`, `jobs`, `fg`, `bg`, `kill %N`; "Done"/"Stopped" notices. Terminator ignores
  SIGINT/SIGTSTP/SIGPIPE itself and never stops.
* **Task 9** history shared by all tabs, last 10,000 commands in `~/.terminator_history`; `history`
  shows the last 1,000; `Ctrl+R` implements the exact-match / longest-common-substring (> 2 chars) /
  no-match rules.
* **Task 10** `Tab` completion of the word before the cursor from the tab's own directory (also
  `dir/pre` and `~/pre`): one match completes, several extend to the longest common prefix and show a
  numbered list (press the number, `Enter` confirms multi-digit choices, anything else cancels), no
  match does nothing. Special characters are backslash-escaped.
* **Task 11** `./script.sh`, `bash script.sh`, `bash -c '...'` and `source script.sh` are interpreted
  by Terminator itself: comments, pipes, redirection, substitution, variables, `$1 $# $?`,
  `if/elif/else/fi` (nested) with `[ ... ]` tests (`-eq -ne -lt -le -gt -ge = != -f -d -e -r -w -x -s
  -z -n ! -a -o`), `&&`/`||`/`;`, `exit [n]`. The whole script is parsed first, so a syntax error such
  as a missing `fi` is reported and nothing runs.

## Known limitations / design choices

* Loops (`while`, `for`), functions, `case`, arithmetic expansion, heredocs and `( )` subshells are not implemented.
* Text is ASCII only (non-ASCII output bytes are drawn as `?`); the core X11 font is used (no Xft).
  The wallpaper (Xpm/Xrender) feature of the first prototype was dropped because the assignment
  allows plain Xlib only.
* Inside double quotes a backslash-newline keeps the newline - this reproduces the sample in the
  assignment (real bash would join the two lines).
* `Ctrl+R` prints "No match for search term in history" exactly as in the assignment's example.
* Lines typed while a command runs go to that command's stdin (like a terminal) - they are not queued as commands.
* `Ctrl+Z` on a compound command (`if ...; fi`) stops the current pipeline and abandons the rest of that list.
* `exit` inside a *sourced* file closes the tab (as it exits the shell in bash); inside `./script.sh` it only ends the script.
* Thread mode cannot survive a crash of Terminator itself; process mode survives a tab crash (the tab
  shows a message and restarts on `Enter`).
* Background jobs of a tab are killed when that tab is closed.

## Repository layout

```
Makefile  README.md  DESIGN.md
src/main.c      start-up, signals, mode selection
src/gui.c       all Xlib code: window, tabs, editor, hotkeys, selections, completion UI, Ctrl+R UI
src/shell.c     per-tab shell engine: lexer, parser, expansion, jobs, builtins, scripts, multiWatch
src/history.c   shared-memory history + semaphore + search
src/complete.c  file-name completion helpers
src/ipc.c       framed message protocol on pipes
tests/          run_tests.sh, engine_cli.c (headless driver), xdrive.c (injects X11 key events)
sample/         terminatorrc, check.sh, demo_commands.txt
```
