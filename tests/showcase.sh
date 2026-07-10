#!/usr/bin/env bash
# The showcase (DESIGN.md §10): a program is a database, and a class of
# binary tooling collapses into SQL -- while the program still runs.
# Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONPATH="$repo/converter"
SELF_EXEC="$repo/loader/self-exec"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

make -C "$repo/loader" >/dev/null
hr() { printf '\033[1m$ %s\033[0m\n' "$*"; }

cat > hello.c <<'EOF'
#include <stdio.h>
int main(void){ puts("Hello, world!"); return 0; }
EOF
cc -O2 hello.c -o hello.elf
python -m selfconv.elf2self hello.elf hello >/dev/null 2>&1
chmod +x hello

hr "file hello"
file hello

hr "./hello        # runs via self-exec (memfd) / binfmt on a SELF system"
"$SELF_EXEC" ./hello

hr "sqlite3 hello '.schema segments'"
sqlite3 hello '.schema segments' | sed 's/^/    /'

hr "sqlite3 hello 'SELECT soname FROM ldd'            # ldd(1)"
sqlite3 hello 'SELECT soname FROM ldd'

hr "sqlite3 hello 'SELECT name,version FROM imports LIMIT 5'   # nm -D --undefined"
sqlite3 hello 'SELECT name,version FROM imports LIMIT 5'

hr "sqlite3 hello 'SELECT type,vaddr,memsz,r,w,x FROM segments'   # readelf -l"
sqlite3 hello 'SELECT type,vaddr,memsz,r,w,x FROM segments'

before=$(stat -c%s hello)
hr "sqlite3 hello 'DELETE FROM sections; DELETE FROM notes; VACUUM;'   # strip(1)"
sqlite3 hello 'DELETE FROM sections; DELETE FROM notes; VACUUM;'
after=$(stat -c%s hello)
echo "    $before -> $after bytes"

hr "./hello        # still runs -- the optional tables were optional"
"$SELF_EXEC" ./hello

echo
echo "Every line above replaced a bespoke ELF tool with a SQL query, and the"
echo "database is still an executable."
