#!/usr/bin/env bash
# M3b: self-ld IS the dynamic linker. A freestanding (no-libc) app calls into
# a freestanding shared library; both live as SELF (SQLite) databases, the
# library is resolved via SQL, and self-ld applies the relocations itself.
# Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
SELF_EXEC="$repo/loader/self-exec"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

make -C "$repo/loader" >/dev/null

# ── freestanding library: add(a,b), no libc ──────────────────────────
cat > lib.c <<'EOF'
long add(long a, long b) { return a + b; }
EOF
cc -nostdlib -shared -fPIC -fno-plt -Wl,-soname,libadd.so.1 lib.c -o libadd.so.1
ln -sf libadd.so.1 libadd.so   # link name for -ladd

# ── freestanding app: _start calls add(40,2), exits with the result ──
# -fno-plt makes the external call go through a GOT slot (R_X86_64_GLOB_DAT)
# that self-ld resolves; the exit status carries the computed value.
cat > app.c <<'EOF'
long add(long, long);
static long sys_exit(long code) {
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(60), "D"(code) : "rcx","r11","memory");
	return ret;
}
void _start(void) { sys_exit(add(40, 2)); }
EOF
cc -nostdlib -fPIC -pie -fno-plt app.c -L. -ladd -Wl,-rpath,'$ORIGIN' -o app
rm -f libadd.so   # drop the dev symlink; keep only libadd.so.1

# sanity: the ELF versions produce exit code 42
set +e; ./app; rc=$?; set -e
test "$rc" -eq 42
pass "baseline: freestanding ELF app+lib exits 42"

# ── convert BOTH to SELF, remove the ELF library, index it ───────────
python -m selfconv.elf2self libadd.so.1 libadd.so.1.self
python -m selfconv.elf2self app app.self
rm -f libadd.so.1                       # library now only exists as SQLite
python -m selfconv.cli scan --db system.db libadd.so.1.self

echo "reloc types self-ld must satisfy in app.self:"
python -m selfconv.cli q app.self \
  "SELECT DISTINCT type FROM relocations"

# ── run under self-ld: it maps both, resolves add() via SQL, binds ──
set +e
SELF_MODE=selfld SELF_SYSTEM_DB="$PWD/system.db" "$SELF_EXEC" ./app.self 2>selfld.log
rc=$?
set -e
cat selfld.log
test "$rc" -eq 42
pass "M3b: self-ld mapped app+lib from SQLite, bound add() via SQL, exit=$rc"

echo "SELFLD (M3b) TEST PASSED"
