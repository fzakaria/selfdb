#!/usr/bin/env bash
# LD_PRELOAD, as a database transaction. An app calls add(6,7). The real
# library returns 13. A "malicious" library returns a*b = 42. Interposition
# is not an environment variable -- it is a row in the `preload` table, and
# toggling it is a transaction you can COMMIT or ROLLBACK. Run in `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
SELF_EXEC="$repo/loader/self-exec"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT; cd "$work"
make -C "$repo/loader" >/dev/null
pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

# real library: add = a + b
cat > add.c <<'EOF'
long add(long a, long b){ return a + b; }
EOF
cc -nostdlib -shared -fPIC -fno-plt -Wl,-soname,libadd.so.1 add.c -o libadd.so.1
ln -sf libadd.so.1 libadd.so

# interposer: same symbol, different behavior (a * b)
cat > mul.c <<'EOF'
long add(long a, long b){ return a * b; }
EOF
cc -nostdlib -shared -fPIC -fno-plt -Wl,-soname,libadd.so.1 mul.c -o libmul.so.1

# app: exit(add(6,7))
cat > app.c <<'EOF'
long add(long, long);
static long sys_exit(long c){ long r; __asm__ volatile("syscall":"=a"(r):"a"(60),"D"(c):"rcx","r11","memory"); return r; }
void _start(void){ sys_exit(add(6, 7)); }
EOF
cc -nostdlib -fPIC -pie -fno-plt app.c -L. -ladd -Wl,-rpath,'$ORIGIN' -o app
rm -f libadd.so

python -m selfconv.elf2self libadd.so.1 libadd.so.1.self >/dev/null 2>&1
python -m selfconv.elf2self libmul.so.1 libmul.so.1.self >/dev/null 2>&1
python -m selfconv.elf2self app app.self >/dev/null 2>&1
python -m selfconv.cli scan --db system.db libadd.so.1.self >/dev/null 2>&1

run() { set +e; SELF_MODE=selfld SELF_SYSTEM_DB="$PWD/system.db" "$SELF_EXEC" ./app.self 2>/dev/null; echo $?; set -e; }

# baseline: no preload row -> real add() -> 6 + 7 = 13
rc=$(run); test "$rc" -eq 13
pass "no preload: add(6,7) = $rc"

# enable interposition: ONE transaction
sqlite3 system.db "BEGIN;
  CREATE TABLE IF NOT EXISTS preload(ord INTEGER PRIMARY KEY, path TEXT);
  INSERT INTO preload(ord, path) VALUES (0, '$PWD/libmul.so.1.self');
COMMIT;"
rc=$(run); test "$rc" -eq 42
pass "after INSERT into preload (one COMMIT): add(6,7) = $rc  (interposed: 6*7)"

# roll it back: interposition gone, no relink, no env var touched
sqlite3 system.db "DELETE FROM preload;"
rc=$(run); test "$rc" -eq 13
pass "after DELETE: add(6,7) = $rc again  (interposition removed)"

echo "PRELOAD-AS-TRANSACTION TEST PASSED"
