#!/bin/sh
# Headless regression tests for the shell engine (no X11 needed): `make test`
# Each case feeds lines to tests/engine_cli and checks the transcript for a fixed string.
cd "$(dirname "$0")/.." || exit 1
CLI=./tests/engine_cli
[ -x "$CLI" ] || { echo "build $CLI first (make test)"; exit 1; }
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
export HOME="$WORK"; cd "$WORK" || exit 1
printf 'abc\n' > abc.txt; printf 'def\n' > def.txt; printf 'z\nb\na\n' > unsorted.txt; printf 'int main(){return 0;}\n' > myprog.c
cat > check.sh <<'SH'
#!/bin/bash
# comment line
if [ -f "$1" ]; then
echo "File exists, compiling..."
else
echo "Source file missing!"
fi
SH
cat > nest.sh <<'SH'
N=5
if [ $N -gt 3 ]; then
  if [ $N -eq 5 ]; then echo five; elif [ $N = 4 ]; then echo four; fi
else
  echo small
fi
echo "args=$# first=$1"
exit 3
echo notreached
SH
printf 'if [ 1 -eq 1 ]; then\necho hi\n' > broken.sh
chmod +x check.sh nest.sh
CLIABS="$OLDPWD/$CLI"; pass=0; fail=0
check() {  # name, input, expected-substring
    out=$(printf '%b' "$2" | timeout 60 "$CLIABS" 2>&1)
    if printf '%s' "$out" | grep -qF -- "$3"; then pass=$((pass+1)); printf 'ok   %s\n' "$1"
    else fail=$((fail+1)); printf 'FAIL %s\n     expected: %s\n     got: %s\n' "$1" "$3" "$out"; fi
}
check "external command"        'echo hi\n' 'hi'
check "unknown command"         'notacommand\necho $?\n' 'terminator: notacommand: command not found'
check "exit status 127"         'notacommand\necho $?\n' '127'
check "single quotes literal"   "echo 'literal: \$HOME'\n" 'literal: $HOME'
check "double quotes expand"    'echo "expanded: $HOME"\n' "expanded: $WORK"
check "multiline dq + backslash" 'echo "Isn'"'"'t exciting?\\\nFork, exec, repeat!"\n' 'Fork, exec, repeat!'
check "continuation prompt"     'echo "a\nb"\n' '> b"'
check "nested substitution"     'echo "x $(basename $(pwd | tr a-z A-Z))"\n' 'x '
check "backticks"               'echo `echo ticks`\n' 'ticks'
check "multi-line substitution" 'echo "n=$(ls | \\\nwc -l)"\n' 'n=7'
check "input redirect"          'sort < unsorted.txt\n' 'a
b
z'
check "output redirect"         'echo data > o.txt\ncat o.txt\n' 'data'
check "both redirects"          'sort < unsorted.txt > sorted.txt\ncat sorted.txt\n' 'z'
check "append redirect"         'echo 1 > ap.txt\necho 2 >> ap.txt\ncat ap.txt\n' '2'
check "missing input file"      'sort < nothere.txt\n' 'terminator: nothere.txt: No such file or directory'
check "pipe + wildcard"         'ls ?bc.txt def.txt | wc -l\n' '2'
check "3-stage pipe + wildcard" 'cat abc.txt def.txt | sort | head -1\n' 'abc'
check "pipe + redirect"         'cat unsorted.txt | sort > p.txt\ncat p.txt\n' 'z'
check "cd per tab state"        'cd /tmp\npwd\n' '/tmp'
check "cd error"                'cd /nonexistent\n' 'No such file or directory'
check "history builtin"         'echo one\nhistory\n' 'echo one'
check "script with if/else"     './check.sh myprog.c\n' 'File exists, compiling...'
check "script else branch"      'bash check.sh nothere.c\n' 'Source file missing!'
check "nested if + args + exit" './nest.sh a b\necho st=$?\n' 'st=3'
check "script args"             './nest.sh a b\n' 'args=2 first=a'
check "script exit stops it"    './nest.sh a\n' 'five'
check "syntax error reported"   'bash broken.sh\n' "missing \`fi'"
check "source builtin"          'source check.sh myprog.c\n' 'File exists'
check "variables"               'X=5\necho $X ${Y:-dflt}\n' '5 dflt'
check "&& and ||"               'false || echo or-ok\ntrue && echo and-ok\n' 'and-ok'
check "test -d / -eq"           '[ -d /tmp ] && [ 3 -gt 2 ] && echo tests-ok\n' 'tests-ok'
check "Ctrl+Z stops job"        '~sleep 100\n@sleep 300\n@z\njobs\n' '[1]+ Stopped sleep 100'
check "bg then jobs"            '~sleep 100\n@sleep 300\n@z\nbg %1\njobs\n' '[1]+ Running sleep 100 &'
check "fg then Ctrl+C"          '~sleep 100\n@sleep 300\n@z\n~fg %1\n@sleep 300\n@c\necho st=$?\n' 'st=130'
check "background job"          'sleep 30 &\njobs\nkill %1\n' '[1]+ Running sleep 30 &'
check "Ctrl+C keeps shell"      '~sleep 100\n@sleep 300\n@c\necho alive\n' 'alive'
check "stdin reaches job"       '~cat\n~hello\n@sleep 200\n@d\n@wait\n' 'hello'
check "multiWatch format"       '~multiWatch ["date +%T", "ls | wc -l"]\n@sleep 700\n@c\n' '"ls | wc -l", '
check "multiWatch dashes"       '~multiWatch ["echo x"]\n@sleep 700\n@c\n' '----------------------------------------------------'
check "multiWatch Ctrl+C"       '~multiWatch ["echo x"]\n@sleep 500\n@c\necho after\n' 'after'
check "bind forwarded to GUI"   'bind line_start Ctrl+B\n' '[BIND:line_start Ctrl+B]'
printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
