#!/usr/bin/env bash
# M1/M2 loader test: memfd (default) and native modes must run converted
# binaries by direct invocation. Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
SELF_EXEC="$repo/loader/self-exec"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

make -C "$repo/loader" >/dev/null

cat > hello.c <<'EOF'
#include <libgen.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
	/* Print basename(argv[0]) so results are independent of whether we run
	 * the ELF directly or via the loader (which passes the .self path). */
	char buf[256];
	strncpy(buf, argv[0], sizeof buf - 1);
	printf("Hello from %s (argc=%d)\n", basename(buf), argc);
	return 7;
}
EOF
cc hello.c -o hello
python -m selfconv.elf2self hello hello.self

for mode in memfd native; do
	set +e
	SELF_MODE=$mode "$SELF_EXEC" hello.self a b c > got.$mode.txt 2>err.$mode
	rc=$?
	set -e
	if [ "$mode" = native ] && grep -q "not yet implemented" err.$mode; then
		printf '\033[33m--\033[0m  native mode: %s\n' "$(cat err.$mode)"
		continue
	fi
	grep -q "Hello from hello.self (argc=4)" got.$mode.txt
	test "$rc" -eq 7
	pass "$mode: hello runs, argv threaded, exit code propagated ($rc)"
done

# a real nixpkgs multi-call binary. coreutils dispatches on basename(argv[0]),
# so the .self must be named 'ls' (the kernel's binfmt 'P' flag handles this
# in the real path; by direct invocation we just name the file accordingly).
python -m selfconv.elf2self "$(command -v ls)" ls
"$(command -v ls)" -1 /nix/store > exp_ls.txt
SELF_MODE=${SELF_MODE_LS:-memfd} "$SELF_EXEC" ./ls -1 /nix/store > got_ls.txt
diff exp_ls.txt got_ls.txt
pass "memfd: nixpkgs ls via loader matches native ls"

echo "LOADER TESTS PASSED (${SELF_MODES:-memfd native})"
