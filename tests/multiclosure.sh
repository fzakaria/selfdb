#!/usr/bin/env bash
# Many roots, one database. Tests that `self closure` can pack several
# independent closures into a single SQLite file, and -- the part that
# actually matters -- that each NEEDED edge still names the provider its own
# object was linked against.
#
# The fixture is the ambiguity itself: two different libraries built with the
# SAME soname, in different directories, one app linked against each. A
# resolver keyed by soname has to pick one of them and is wrong half the time.
# A per-edge foreign key is right both times. Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT; cd "$work"
pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

# ── two libraries, one soname, different answers ─────────────────────
mkdir -p a b
echo 'int ambig(void){ return 11; }' > a/ambig.c
echo 'int ambig(void){ return 22; }' > b/ambig.c
for d in a b; do
	cc -shared -fPIC -Wl,-soname,libambig.so.1 "$d/ambig.c" -o "$d/libambig.so.1"
	ln -sf libambig.so.1 "$d/libambig.so"
done

# ── one app per library; both also pull in libc, which is shared ─────
echo 'int ambig(void); int main(void){ return ambig(); }' > app.c
for d in a b; do
	cc app.c -L"$d" -lambig -Wl,-rpath,"\$ORIGIN/$d" -o "app$d"
done
rm -f a/libambig.so b/libambig.so

# ── pack BOTH roots into ONE database ────────────────────────────────
python -m selfconv.cli closure --root appa --root appb -o both.db

q() { sqlite3 both.db "$1"; }

test "$(q 'SELECT count(*) FROM objects WHERE is_root=1')" -eq 2
pass "two roots in one database"

# both providers are present: the file really does hold the ambiguity
test "$(q "SELECT count(*) FROM objects WHERE soname='libambig.so.1'")" -eq 2
pass "both libambig.so.1 providers stored, not deduplicated by soname"

# libc is needed by both roots and must be stored exactly once
test "$(q "SELECT count(*) FROM objects WHERE soname LIKE 'libc.so%'")" -eq 1
pass "libc shared across both closures, stored once (path UNIQUE)"

# THE point: each edge resolves to the provider that object was linked against
for r in a b; do
	got=$(q "SELECT n.resolved_path FROM needs n JOIN objects o ON o.id=n.object_id
	         WHERE o.path LIKE '%/app$r' AND n.soname='libambig.so.1'")
	case "$got" in
	*"/$r/libambig.so.1") ;;
	*) echo "FAIL: app$r resolved libambig.so.1 to '$got', wanted the copy in $r/"; exit 1 ;;
	esac
done
pass "each root resolved libambig.so.1 to its own provider (per-edge FK)"

unresolved=$(q "SELECT count(*) FROM needs
                WHERE resolved_path IS NULL AND soname NOT LIKE 'ld-%'")
test "$unresolved" -eq 0
pass "no dangling edges across either closure"

echo "MULTI-CLOSURE TEST PASSED"
