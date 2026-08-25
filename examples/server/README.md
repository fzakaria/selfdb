# self-httpd: a webserver that is a SQLite database

A single file. `file(1)` calls it a SQLite database, the kernel executes it,
and it serves a website out of its own tables.

```console
$ file server
server: SQLite 3.x database, application id 1397050438, user version 1, ...

$ xxd -s 64 -l 8 server          # 1397050438 == 0x53454c46, the binfmt magic
00000040: 0000 0000 5345 4c46                      ....SELF

$ ./server --journal wal 8080
self-httpd: serving 3 routes out of /srv/self/server
self-httpd: listening on http://0.0.0.0:8080 with 4 workers (journal=wal)

$ sqlite3 server 'SELECT path, mime, bytes FROM site'
/favicon.svg|image/svg+xml|476
/index.html|text/html; charset=utf-8|6258
/style.css|text/css; charset=utf-8|2605
```

[redbean](https://redbean.dev) reaches the same single-file place by stapling
a ZIP onto an Actually Portable Executable and reading it back through a
bespoke `zipos` layer. In SELF the container is already a database, so there
is nothing to staple and nothing to parse:
`SELECT body FROM routes WHERE path = ?` is the whole asset pipeline.

The trade is honest and worth stating plainly: redbean runs on six operating
systems and two architectures. This runs on Linux, and only where
`binfmt_misc` has been taught the SELF magic. What it buys instead is that
the artifact is queryable and that changing the site is a transaction.

## How the program finds itself

`binfmt_misc` hands the interpreter `[self-exec, <path>, args…]`, and
`self-exec` passes `argv + 1` through, so the program's **`argv[0]` is the
`.self` path**. `/proc/self/exe` is not: it points at the interpreter in
`native` mode and at an anonymous memfd in `memfd` mode.

Nothing holds the file open by then — the kernel never `execve`d the database
itself — so `sqlite3_open(argv[0])` succeeds, with no `ETXTBSY` and no lock
contention.

## Build

```console
$ nix develop
$ bash examples/server/build.sh ./server     # compile -> elf2self -> INSERT
$ ./server 8080                              # needs binfmt_misc registered
$ loader/self-exec ./server 8080             # or call the interpreter directly

$ ./server --help
usage: ./server [--journal delete|wal] [port]
  delete  one file on disk while serving; ~1.4 ms/request (default)
  wal     ~3x faster, plus -wal and -shm while the server runs
environment: $PORT, $SELF_HTTPD_JOURNAL
```

`build.sh` is the demo in three steps:

1. `cc server.c -o server.elf` — an ordinary ELF.
2. `elf2self` — the same program, as rows.
3. `sqlite3 server < site/schema.sql` and one `INSERT` per file in `site/` —
   **the website is added to the executable with SQL**, after the compiler is
   done and after the linker is done.

## Editing a running site

```console
$ sqlite3 server "UPDATE routes SET body = readfile('new.html')
                  WHERE path = '/index.html'"
$ curl -s localhost:8080 | head -1        # already changed; no restart

$ sqlite3 server 'SELECT count(*), ua FROM visits GROUP BY ua ORDER BY 1 DESC'
```

Deployment is `scp` of one file. A rollback is `ROLLBACK`.

`sqldiff --summary` proves a deploy touched the website and not the program:

```console
$ sqldiff --summary yesterday.server server
routes:      1 changes, 0 inserts, 0 deletes, 2 unchanged
segments:    0 changes, 0 inserts, 0 deletes, 13 unchanged
symbols:     0 changes, 0 inserts, 0 deletes, 174 unchanged
```

And a webserver can index its own pages, inside itself, and still run:

```console
$ sqlite3 server "CREATE VIRTUAL TABLE search USING fts5(path, body);
                  INSERT INTO search SELECT path, body FROM routes
                    WHERE mime LIKE 'text/%'"
$ sqlite3 server "SELECT path, snippet(search, 1, '[', ']', '...', 6)
                  FROM search WHERE search MATCH 'transaction'"
/index.html|...Editing is a [transaction].</h2>
```

## Tables

`routes` is the content; `visits` and `presses` are what the site writes back
into itself while it runs (`site/schema.sql`). They sit alongside `segments`,
`symbols` and `relocations` — the tables the loader reads to run the program
in the first place.

| endpoint | what it does |
|---|---|
| `GET /` | `routes['/index.html']`, with `{{name}}` substituted from queries |
| `GET /<path>` | the matching `routes` row |
| `GET /api/stats` | every template variable as JSON |
| `GET /api/tables` | `sqlite_schema` with live row counts — `readelf -S` |
| `POST /api/press` | `INSERT INTO presses`, returns the new count |

Every request also appends to `visits`. The executable therefore grows as the
site is used: a few thousand requests took the example from 155,648 to
200,704 bytes.

## Two design choices worth knowing

**A pre-forked pool, not a connection per request.** Opening a SQLite
database costs far more than querying one. With a fresh `sqlite3_open` per
request the mean response was 3.6 ms; with four workers that each open the
executable once and keep it, it is 1.4 ms. Serving stays isolated — one slow
client occupies one worker, and a worker that dies is respawned.

**The journal mode is a flag, because the two answers are both defensible.**
`--journal delete` (the default) keeps a rollback journal: `server-journal`
exists only for the microseconds of a write transaction, so `ls` beside a
running server shows exactly one file. `--journal wal` is **about 3× faster**
end to end (0.54 ms against 1.48 ms mean) because a rollback journal creates,
syncs and deletes a file on every commit — but `server-wal` and `server-shm`
sit next to the executable for as long as a worker holds a connection.

The single-file property survives WAL *at rest*: every worker calls
`sqlite3_close` on `SIGTERM`, the last one checkpoints the log away, and the
artifact is one file again. `tests/server.sh` asserts both halves. The
deployment at <https://selfdb.exe.xyz> runs `--journal wal`, so:

```console
$ systemctl stop self-httpd && ls /srv/self
server
$ systemctl start self-httpd && ls /srv/self
server  server-shm  server-wal
```

The mode is set once at startup, by the parent, before any worker exists —
leaving WAL needs an exclusive lock, which four workers would contend for. A
mode change is therefore refused (loudly) if another instance is running.

If you run WAL: after a crash the sidecars survive and the next clean open
recovers and unlinks them, so do not `scp` the executable out from under an
un-recovered WAL.

## Measured

`examples/server/bench.py`, 300 requests per path, one fresh connection each,
on a NixOS host with the `memfd` loader:

| what | mean | p50 |
|---|---|---|
| the same accept/respond loop with the body compiled in | 0.40 ms | 0.38 ms |
| `GET /` served from the b-tree, no visit logged | 0.40 ms | 0.37 ms |
| `GET /` as deployed — `--journal wal`, one committed `INSERT` | 0.54 ms | 0.49 ms |
| `GET /` with `--journal delete`, one committed `INSERT` | 1.48 ms | 1.42 ms |

Two things fall out. **Reading is free**: a page plus the fifteen counters on
it, sixteen queries in all, does not clear the noise floor of the process
model itself. And **the cost is entirely the durable write** — 0.14 ms of it
under WAL, 1.08 ms under a rollback journal. The 8× difference between those
two is the price of keeping one file on disk while the server runs.

```console
$ python3 examples/server/bench.py http://127.0.0.1:8080/ / /style.css /api/stats
```

Reproduce the A/B by running two copies of the same artifact side by side:

```console
$ cp server bench_wal && cp server bench_del
$ ./bench_wal --journal wal 8098 &
$ ./bench_del --journal delete 8099 &
```

## Live

<https://selfdb.exe.xyz> — the page there is a row in the executable serving
it. Write-up:
[Your executable is a SQLite database](https://fzakaria.com/2026/08/23/your-executable-is-a-sqlite-database).
