#!/usr/bin/env bash
# examples/server: a webserver whose content, code and visitor log are one
# file. The test asserts the three claims the example makes:
#
#   1. the executable serves pages out of its own `routes` table
#   2. a request writes a row back into the executable while it runs
#   3. editing the live site is SQL -- UPDATE lands with no restart, and
#      ROLLBACK un-lands it
#   4. --journal picks the trade: `delete` is one file even while serving,
#      `wal` is faster and keeps sidecars until the last worker closes
#
# Run inside `nix develop`.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
SELF_EXEC="$repo/loader/self-exec"
PORT="${SELF_HTTPD_TEST_PORT:-8731}"
work="$(mktemp -d)"
server_pid=""
cleanup() {
	[ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
cd "$work"

pass() { printf '\033[32mok\033[0m  %s\n' "$1"; }

make -C "$repo/loader" >/dev/null
bash "$repo/examples/server/build.sh" "$work/server" >/dev/null

# The artifact is a SQLite database that `file` recognizes as such.
file server | grep -q "SQLite 3.x database"
pass "the webserver is a SQLite database (application id 0x53454c46)"

# The website went in with INSERT, so it is queryable before it ever runs.
test "$(sqlite3 server 'SELECT count(*) FROM routes')" -ge 3
sqlite3 server "SELECT 1 FROM routes WHERE path='/index.html'" | grep -q 1
pass "the site is rows: routes queryable in the unstarted executable"

"$SELF_EXEC" ./server "$PORT" >server.log 2>&1 &
server_pid=$!
for _ in $(seq 100); do
	curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" && break
	sleep 0.1
done

curl -fsS "http://127.0.0.1:$PORT/" | grep -q "row in the executable that served it"
curl -fsS "http://127.0.0.1:$PORT/style.css" | grep -q "SELECT body WHERE"
pass "GET / and GET /style.css come out of the routes table"

# Template values are queries against the file, so the page cannot show a
# count the database does not have.
visits_in_page="$(curl -fsS "http://127.0.0.1:$PORT/api/stats" |
	sed 's/.*"visits":\([0-9]*\).*/\1/')"
visits_in_db="$(sqlite3 server 'SELECT count(*) FROM visits')"
test "$visits_in_page" -le "$visits_in_db"
test "$visits_in_db" -ge 3
pass "every request appended a row to visits inside the executable ($visits_in_db)"

before="$(sqlite3 server 'SELECT count(*) FROM presses')"
curl -fsS -X POST -d press "http://127.0.0.1:$PORT/api/press" | grep -q '"presses"'
after="$(sqlite3 server 'SELECT count(*) FROM presses')"
test "$after" -eq "$((before + 1))"
pass "POST /api/press INSERTs into the running program's own file"

# The headline: deploying a content change is a transaction, not a restart.
echo '<!doctype html><h1>edited in place</h1>' >new.html
sqlite3 server "UPDATE routes SET body = readfile('new.html') WHERE path='/index.html'"
curl -fsS "http://127.0.0.1:$PORT/" | grep -q "edited in place"
pass "UPDATE routes changed the live site with no restart"

sqlite3 server "BEGIN; DELETE FROM routes WHERE path='/style.css'; ROLLBACK;"
curl -fsS -o /dev/null "http://127.0.0.1:$PORT/style.css"
pass "ROLLBACK un-deploys; /style.css is still served"

# One file while serving: the rollback journal only exists mid-transaction.
test "$(sqlite3 server 'PRAGMA journal_mode')" = "delete"
test "$(ls server* | grep -vc '^server.log$')" -eq 1
pass "journal=delete: no sidecars, the running server is a single file"

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""
test "$(ls server* | grep -vc '^server.log$')" -eq 1
pass "clean shutdown leaves the same single file"

# 4. --journal wal is the other half of the trade: faster, but the sidecars
#    exist for as long as a worker holds a connection, and go away when the
#    last one closes cleanly.
"$SELF_EXEC" ./server --journal wal "$PORT" >>server.log 2>&1 &
server_pid=$!
for _ in $(seq 100); do
	curl -fsS -o /dev/null "http://127.0.0.1:$PORT/" && break
	sleep 0.1
done
test "$(sqlite3 server 'PRAGMA journal_mode')" = "wal"
test -f server-wal && test -f server-shm
pass "journal=wal: -wal and -shm exist while a connection is open"

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""
test ! -e server-wal && test ! -e server-shm
pass "every worker closed cleanly: the WAL was checkpointed away, one file again"

echo "SERVER EXAMPLE TESTS PASSED"
