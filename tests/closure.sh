#!/usr/bin/env bash
# One binary + its whole dependency closure in ONE SQLite database, and why
# that -- not a global soname->path table -- is the right "system database"
# on a store-based system. Run in `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT; cd "$work"
pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

subj="$(readlink -f "$(type -P ls)")"

# ── the global-resolver trap: one soname, many providers ─────────────
providers=$(ls /nix/store/*/lib/libc.so.6 2>/dev/null | wc -l)
echo "distinct /nix/store providers of soname 'libc.so.6': $providers"
test "$providers" -gt 1
pass "a global soname->path table is ambiguous here ($providers libc.so.6)"

# ── pack the binary + closure into one DB ────────────────────────────
python -m selfconv.cli closure "$subj" all.db
n=$(sqlite3 all.db "SELECT count(*) FROM objects")
test "$n" -ge 2
pass "merged $n objects (exe + libs) into one SQLite file"

# ── every NEEDED edge resolves to a concrete member: a foreign key ──
unresolved=$(sqlite3 all.db "SELECT count(*) FROM needs WHERE resolved_path IS NULL AND soname NOT LIKE 'ld-%'")
echo "NEEDED edges whose provider is NOT a member of this closure: $unresolved"
test "$unresolved" -eq 0
pass "resolution is a FK JOIN, not a soname guess (0 dangling edges)"

# ── ldd(1), as a query ───────────────────────────────────────────────
echo "ldd, as a JOIN:"
sqlite3 -column all.db "SELECT n.soname, substr(n.resolved_path,12,20)
  FROM needs n JOIN objects o ON o.id=n.object_id WHERE o.is_root=1"

# ── 'who provides symbol X in this closure?' -- one JOIN ─────────────
sym=$(sqlite3 all.db "SELECT name FROM imports LIMIT 1")
echo "who exports the root's import '$sym'?"
sqlite3 -column all.db "SELECT DISTINCT o.soname FROM exports e
  JOIN objects o ON o.id=e.object_id WHERE e.name='$sym'"

echo "CLOSURE TEST PASSED"
