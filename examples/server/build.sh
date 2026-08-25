#!/usr/bin/env bash
# Build the single-file webserver:
#
#   1. compile server.c into an ordinary ELF
#   2. elf2self: rewrite that ELF as a SQLite database
#   3. add the website to the executable with INSERT
#
# Step 3 is the point. There is no bundler and no stapled archive; the pages
# are rows next to `segments` and `symbols`, put there by the sqlite3 CLI.
#
# Usage: bash examples/server/build.sh [outfile]      (default: ./server)
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
out="${1:-$PWD/server}"
strip_tables="${SELF_STRIP:-0}"

export PYTHONPATH="$repo/converter${PYTHONPATH:+:$PYTHONPATH}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ── 1. an ordinary ELF ────────────────────────────────────────────────
${CC:-cc} -O2 -Wall -Wextra -std=gnu11 "$here/server.c" -o "$work/server.elf" \
	$(pkg-config --cflags --libs sqlite3)
elf_bytes=$(stat -c%s "$work/server.elf")

# ── 2. the same program, as rows ──────────────────────────────────────
rm -f "$out"
python3 -m selfconv.elf2self "$work/server.elf" "$out"

# ── 3. the website, added to the executable with SQL ──────────────────
sqlite3 "$out" < "$here/site/schema.sql"

mime_for() {
	case "$1" in
	*.html) echo "text/html; charset=utf-8" ;;
	*.css) echo "text/css; charset=utf-8" ;;
	*.js) echo "application/javascript; charset=utf-8" ;;
	*.json) echo "application/json" ;;
	*.svg) echo "image/svg+xml" ;;
	*.png) echo "image/png" ;;
	*.ico) echo "image/x-icon" ;;
	*.txt | *.md) echo "text/plain; charset=utf-8" ;;
	*) echo "application/octet-stream" ;;
	esac
}

# readfile() puts the bytes straight into the BLOB, so a binary asset needs no
# encoding step; the whole site lands in one transaction.
{
	echo "BEGIN;"
	echo "DELETE FROM routes;"
	for asset in "$here"/site/*; do
		name="$(basename "$asset")"
		[ "$name" = "schema.sql" ] && continue
		printf "INSERT INTO routes (path, mime, body) VALUES ('/%s', '%s', readfile('%s'));\n" \
			"$name" "$(mime_for "$name")" "$asset"
	done
	echo "COMMIT;"
} | sqlite3 "$out"

# ── optional: strip(1), which here is DELETE + VACUUM ─────────────────
if [ "$strip_tables" = "1" ]; then
	sqlite3 "$out" 'DELETE FROM sections; DELETE FROM notes; VACUUM;'
fi

chmod +x "$out"

self_bytes=$(stat -c%s "$out")
routes=$(sqlite3 "$out" 'SELECT count(*) FROM routes')
printf '%s: %s routes, %s bytes (ELF was %s)\n' "$out" "$routes" "$self_bytes" "$elf_bytes"
sqlite3 -column -header "$out" 'SELECT path, mime, bytes FROM site'
