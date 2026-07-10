#!/usr/bin/env bash
# M3a: glibc's own ld.so loads a SELF (SQLite) shared library, resolved via
# SQL through the LD_AUDIT hook. Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
AUDIT="$repo/loader/libself-audit.so"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

make -C "$repo/loader" >/dev/null

# ── a shared library and an app that needs it ────────────────────────
cat > greet.c <<'EOF'
#include <stdio.h>
void greet(const char *who) { printf("Hello, %s, from a SQLite library!\n", who); }
EOF
cat > app.c <<'EOF'
extern void greet(const char *);
int main(void) { greet("world"); return 0; }
EOF

cc -shared -fPIC -Wl,-soname,libgreet.so.1 greet.c -o libgreet.so.1
ln -sf libgreet.so.1 libgreet.so             # link name -> real soname file
cc app.c -L. -lgreet -Wl,-rpath,'$ORIGIN' -o app  # link against the ELF .so
rm -f libgreet.so                            # drop the dev symlink
cp libgreet.so.1 libgreet.so.1.orig          # keep a copy of the ELF

# baseline: works against the ELF library
./app > expected.txt
grep -q "SQLite library" expected.txt
pass "baseline: app runs against the ELF libgreet"

# ── convert the library to SELF and REMOVE the ELF ───────────────────
python -m selfconv.elf2self libgreet.so.1.orig libgreet.so.1.self
echo "soname: $(python -m selfconv.cli q libgreet.so.1.self \
  "SELECT value FROM self_meta WHERE key='soname'")"
rm -f libgreet.so.1 libgreet.so.1.orig          # no ELF library on disk now

# prove the ELF is truly gone: app must fail to start normally
if ./app 2>/dev/null; then
	echo "FAIL: app ran without the audit hook -- ELF library still present?"
	exit 1
fi
pass "with the ELF removed, ./app fails to start (no libgreet on disk)"

# ── index the .self into a resolver DB and run under the audit hook ──
python -m selfconv.cli scan --db system.db .
sqlite3 system.db "SELECT soname, kind, path FROM objects WHERE soname LIKE 'libgreet%'"

SELF_AUDIT_DEBUG=1 SELF_SYSTEM_DB="$PWD/system.db" LD_AUDIT="$AUDIT" \
	./app > got.txt 2>audit.log || { echo "run failed"; cat audit.log; exit 1; }
cat audit.log
diff expected.txt got.txt
pass "M3a: stock ld.so loaded libgreet.so.1 FROM SQLite, resolved via SQL"

echo "AUDIT (M3a) TEST PASSED"
