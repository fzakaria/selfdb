#!/usr/bin/env bash
# M0 round-trip: ELF -> SELF -> ELF must still run, and the SELF file must
# answer the showcase queries. Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

# ── subject 1: freshly compiled dynamic PIE ──────────────────────────
cat > hello.c <<'EOF'
#include <stdio.h>
int main(int argc, char **argv) { printf("Hello, world!\n"); return 0; }
EOF
cc hello.c -o hello
./hello > expected.txt

python -m selfconv.elf2self hello hello.self
python -m selfconv.self2elf hello.self hello.rt
./hello.rt > got.txt
diff expected.txt got.txt
pass "hello: ELF -> SELF -> ELF runs identically"

file_out="$(file hello.self)"
grep -q "SQLite 3.x database" <<<"$file_out"
grep -qi "application id.*SELF\|application id.*0x53454c46\|1397049158" <<<"$file_out" \
  || echo "note: file(1) does not decode application_id here: $file_out"
pass "hello.self is a SQLite database"

# ── showcase queries ─────────────────────────────────────────────────
test "$(python -m selfconv.cli q hello.self 'SELECT count(*) FROM ldd')" -ge 1
python -m selfconv.cli q hello.self \
  "SELECT name, version FROM imports WHERE name = '__libc_start_main'" \
  | grep -q GLIBC
python -m selfconv.cli q hello.self \
  "SELECT count(*) FROM segments WHERE type='load'" | grep -qv '^0$'
pass "showcase queries (ldd / imports / segments)"

# ── strip(1) as DELETE + VACUUM ─────────────────────────────────────
before=$(stat -c%s hello.self)
sqlite3 hello.self 'DELETE FROM sections; DELETE FROM notes; VACUUM;'
after=$(stat -c%s hello.self)
test "$after" -lt "$before"
python -m selfconv.self2elf hello.self hello.stripped
./hello.stripped > got2.txt
diff expected.txt got2.txt
pass "strip via DELETE+VACUUM ($before -> $after bytes), still runs"

# ── subject 2: a real nixpkgs binary (coreutils ls) ─────────────────
ls_bin="$(command -v ls)"
python -m selfconv.elf2self "$ls_bin" ls.self
mkdir -p rt && python -m selfconv.self2elf ls.self rt/ls  # coreutils is a
"$ls_bin" -la /nix > expected_ls.txt                      # multi-call binary:
./rt/ls   -la /nix > got_ls.txt                           # argv[0] must be 'ls'
diff expected_ls.txt got_ls.txt
pass "nixpkgs ls: round-trip runs identically"

echo "ALL M0 TESTS PASSED"
