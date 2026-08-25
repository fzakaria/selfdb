/*
 * self-httpd: a webserver whose content, code and data live in one file --
 * the file the kernel just executed.
 *
 * A SELF binary is a SQLite database (DESIGN.md). binfmt_misc hands the
 * database to `self-exec`, which maps the `segments` rows and jumps to the
 * entry point; by the time main() runs, nothing holds the file open. So the
 * program can turn around and open its own file as a database:
 *
 *   argv[0]  ->  sqlite3_open()  ->  SELECT body FROM routes WHERE path = ?
 *
 * `/proc/self/exe` is NOT the handle: binfmt gives the interpreter
 * [self-exec, <path>, args...] and self-exec passes argv+1 on, so argv[0] is
 * the .self path while /proc/self/exe points at the interpreter (native
 * mode) or at an anonymous memfd (memfd mode).
 *
 * Website content is inserted with SQL after conversion (see build.sh), and
 * the data the site collects is written back into the same file while it
 * serves. Editing the live site is an UPDATE; rolling it back is a ROLLBACK.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <sqlite3.h>

#define DEFAULT_PORT 8080
#define WORKER_COUNT 4
#define LISTEN_BACKLOG 128
#define REQUEST_TIMEOUT_SECONDS 15
#define BUSY_TIMEOUT_MS 5000
#define RESPAWN_DELAY_SECONDS 1
#define MAX_HEADER_BYTES 16384
#define MAX_BODY_BYTES 4096
#define MAX_PATH_BYTES 512
#define MAX_UA_BYTES 256
#define MAX_BUTTON_BYTES 32
#define DEFAULT_BUTTON "press"
#define DEFAULT_JOURNAL_MODE JOURNAL_DELETE
#define INDEX_ROUTE "/index.html"
#define HTML_MIME "text/html; charset=utf-8"
#define JSON_MIME "application/json"

/* How the executable journals writes to itself. The two modes trade the
 * single-file property against speed by a factor of about three, and which
 * one is right depends on whether the artifact is a demonstration or a
 * server, so it is a flag rather than a hardcoded answer. */
enum journal_mode {
	/* Rollback journal: `server-journal` exists only for the microseconds of
	 * a write transaction, so `ls` beside a running server shows exactly one
	 * file. Slower, because each commit creates, syncs and deletes it. */
	JOURNAL_DELETE,
	/* Write-ahead log: `server-wal` and `server-shm` sit beside the
	 * executable for as long as any connection is open, and a worker holds
	 * one for its whole life. SQLite checkpoints and unlinks both on the last
	 * clean close, so the artifact is a single file again once it stops. */
	JOURNAL_WAL,
};

static const struct journal_spec {
	const char *name;
	const char *pragma;
} JOURNAL_MODES[] = {
	[JOURNAL_DELETE] = { "delete", "PRAGMA journal_mode=DELETE" },
	[JOURNAL_WAL] = { "wal", "PRAGMA journal_mode=WAL" },
};
#define JOURNAL_MODE_COUNT (sizeof JOURNAL_MODES / sizeof JOURNAL_MODES[0])

/* The .self file this process was loaded from -- see the header comment. */
static char g_db_path[PATH_MAX];
static time_t g_started_at;
static volatile sig_atomic_t g_stopping;

/* ── a growable byte buffer, for responses we assemble ────────────── */

struct buffer {
	char *data;
	size_t len;
	size_t cap;
};

static void buf_append(struct buffer *b, const void *bytes, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t want = (b->cap ? b->cap : 1024);
		while (want < b->len + n + 1) {
			want *= 2;
		}
		char *grown = realloc(b->data, want);
		if (!grown) {
			return; /* out of memory: the response is truncated, not fatal */
		}
		b->data = grown;
		b->cap = want;
	}
	memcpy(b->data + b->len, bytes, n);
	b->len += n;
	b->data[b->len] = '\0';
}

static void buf_printf(struct buffer *b, const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (n > 0) {
		buf_append(b, line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	}
}

static void buf_free(struct buffer *b)
{
	free(b->data);
	b->data = NULL;
	b->len = b->cap = 0;
}

/* ── the database that is also this program ──────────────────────── */

static sqlite3 *db_open(void)
{
	sqlite3 *db = NULL;
	if (sqlite3_open(g_db_path, &db) != SQLITE_OK) {
		fprintf(stderr, "self-httpd: cannot open %s: %s\n", g_db_path,
		        db ? sqlite3_errmsg(db) : "?");
		sqlite3_close(db);
		return NULL;
	}

	/* The journal mode is not set here: it lives in the database header and
	 * main() has already chosen it once, before any worker existed. A worker
	 * that tried to switch modes would be fighting the other three for the
	 * exclusive lock that leaving WAL requires.
	 *
	 * synchronous=NORMAL is per-connection, though, and it matters: every
	 * request INSERTs a visit, and at FULL each one costs an fsync that
	 * dominates the response. A visitor log is worth losing the last few
	 * commits of after a power cut. */
	sqlite3_busy_timeout(db, BUSY_TIMEOUT_MS);
	sqlite3_exec(db, "PRAGMA synchronous=NORMAL", NULL, NULL, NULL);
	return db;
}

/* First column of the first row, as malloc'd text. NULL if the query yields
 * no row or fails -- callers treat both the same way. */
static char *sql_scalar(sqlite3 *db, const char *sql)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		return NULL;
	}

	char *out = NULL;
	if (sqlite3_step(stmt) == SQLITE_ROW) {
		const unsigned char *text = sqlite3_column_text(stmt, 0);
		if (text) {
			out = strdup((const char *)text);
		}
	}
	sqlite3_finalize(stmt);
	return out;
}

static int parse_journal_mode(const char *name, enum journal_mode *out)
{
	for (size_t i = 0; i < JOURNAL_MODE_COUNT; i++) {
		if (strcasecmp(name, JOURNAL_MODES[i].name) == 0) {
			*out = (enum journal_mode)i;
			return 0;
		}
	}
	return -1;
}

/* Choose the journal mode once, at startup, while this process is the only
 * one holding the file: switching *out* of WAL needs an exclusive lock, and
 * four workers asking for it at once is a race with no winner. The mode then
 * lives in the database header, and every worker inherits it by opening.
 *
 * Verifying the result matters — SQLite reports the mode it actually ended up
 * in rather than failing, so a refused switch would otherwise be silent. */
static int apply_journal_mode(enum journal_mode mode)
{
	sqlite3 *db = db_open();
	if (!db) {
		return -1;
	}

	sqlite3_exec(db, JOURNAL_MODES[mode].pragma, NULL, NULL, NULL);
	char *got = sql_scalar(db, "PRAGMA journal_mode");
	int applied = got && strcasecmp(got, JOURNAL_MODES[mode].name) == 0;
	if (!applied) {
		fprintf(stderr, "self-httpd: asked for journal_mode=%s, got %s\n",
		        JOURNAL_MODES[mode].name, got ? got : "?");
	}
	free(got);
	sqlite3_close(db);
	return applied ? 0 : -1;
}

/* ── the numbers on the page, each one a query against this file ─── */

enum value_kind {
	VALUE_NUMBER, /* emitted bare in JSON */
	VALUE_TEXT,   /* emitted quoted in JSON */
};

static const struct template_var {
	const char *name;
	const char *sql;
	enum value_kind kind;
} TEMPLATE_VARS[] = {
	/* what the site has collected, stored in the executable */
	{ "visits", "SELECT count(*) FROM visits", VALUE_NUMBER },
	{ "presses", "SELECT count(*) FROM presses", VALUE_NUMBER },
	/* what the site is made of */
	{ "routes", "SELECT count(*) FROM routes", VALUE_NUMBER },
	{ "site_bytes", "SELECT coalesce(sum(length(body)), 0) FROM routes", VALUE_NUMBER },
	/* what the executable is made of: the loader's own tables */
	{ "segments", "SELECT count(*) FROM segments", VALUE_NUMBER },
	{ "symbols", "SELECT count(*) FROM symbols", VALUE_NUMBER },
	{ "relocations", "SELECT count(*) FROM relocations", VALUE_NUMBER },
	{ "needed", "SELECT count(*) FROM needed", VALUE_NUMBER },
	{ "tables", "SELECT count(*) FROM sqlite_schema WHERE type='table'", VALUE_NUMBER },
	{ "file_bytes",
	  "SELECT (SELECT * FROM pragma_page_count) * (SELECT * FROM pragma_page_size)",
	  VALUE_NUMBER },
	{ "page_size", "SELECT * FROM pragma_page_size", VALUE_NUMBER },
	/* identity, straight out of self_meta */
	{ "machine", "SELECT value FROM self_meta WHERE key='machine'", VALUE_TEXT },
	{ "entry", "SELECT printf('0x%x', value) FROM self_meta WHERE key='entry'", VALUE_TEXT },
	{ "interp", "SELECT value FROM self_meta WHERE key='interp'", VALUE_TEXT },
	{ "sqlite_version", "SELECT sqlite_version()", VALUE_TEXT },
};
#define TEMPLATE_VAR_COUNT (sizeof TEMPLATE_VARS / sizeof TEMPLATE_VARS[0])

/* Substitute {{name}} in an HTML body with the value of that variable's
 * query. An unknown name is left verbatim so a typo is visible on the page
 * rather than silently blank. */
static struct buffer render(sqlite3 *db, const char *html, size_t len)
{
	struct buffer out = { 0 };
	size_t i = 0;
	while (i < len) {
		const char *open = memmem(html + i, len - i, "{{", 2);
		if (!open) {
			break;
		}
		const char *close = memmem(open, len - (size_t)(open - html), "}}", 2);
		if (!close) {
			break;
		}

		buf_append(&out, html + i, (size_t)(open - (html + i)));
		size_t name_len = (size_t)(close - (open + 2));
		char *value = NULL;
		for (size_t v = 0; v < TEMPLATE_VAR_COUNT; v++) {
			const char *name = TEMPLATE_VARS[v].name;
			if (strlen(name) == name_len && memcmp(name, open + 2, name_len) == 0) {
				value = sql_scalar(db, TEMPLATE_VARS[v].sql);
				break;
			}
		}

		if (value) {
			buf_append(&out, value, strlen(value));
			free(value);
		} else {
			buf_append(&out, open, (size_t)(close + 2 - open));
		}
		i = (size_t)(close + 2 - html);
	}

	buf_append(&out, html + i, len - i);
	return out;
}

/* ── HTTP ────────────────────────────────────────────────────────── */

struct request {
	char method[8];
	char path[MAX_PATH_BYTES];
	char user_agent[MAX_UA_BYTES];
	char body[MAX_BODY_BYTES + 1];
	size_t body_len;
};

static const char *status_text(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 500:
		return "Internal Server Error";
	default:
		return "Unknown";
	}
}

static void send_all(int fd, const void *bytes, size_t len)
{
	const char *p = bytes;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) {
			return;
		}
		p += n;
		len -= (size_t)n;
	}
}

static void respond(int fd, int status, const char *mime, const void *body, size_t len)
{
	char head[512];
	/* Connection: close keeps the accept loop honest -- one request per
	 * connection, no keep-alive bookkeeping in a demo server. */
	int n = snprintf(head, sizeof head,
	                 "HTTP/1.1 %d %s\r\n"
	                 "Server: self-httpd (SELF/SQLite %s)\r\n"
	                 "Content-Type: %s\r\n"
	                 "Content-Length: %zu\r\n"
	                 "X-Served-From: sqlite-row\r\n"
	                 "Connection: close\r\n"
	                 "\r\n",
	                 status, status_text(status), sqlite3_libversion(), mime, len);
	send_all(fd, head, (size_t)n);
	send_all(fd, body, len);
}

static void respond_text(int fd, int status, const char *message)
{
	respond(fd, status, "text/plain; charset=utf-8", message, strlen(message));
}

/* Read the request head, then as much body as Content-Length promises.
 * Returns 0 on success, -1 on a malformed or oversized request. */
static int read_request(int fd, struct request *req)
{
	char head[MAX_HEADER_BYTES + 1];
	size_t have = 0;
	char *blank = NULL;

	while (have < MAX_HEADER_BYTES) {
		ssize_t n = read(fd, head + have, MAX_HEADER_BYTES - have);
		if (n <= 0) {
			return -1;
		}
		have += (size_t)n;
		head[have] = '\0';
		blank = strstr(head, "\r\n\r\n");
		if (blank) {
			break;
		}
	}
	if (!blank) {
		return -1;
	}

	/* request line: METHOD SP PATH SP VERSION */
	if (sscanf(head, "%7s %511s", req->method, req->path) != 2) {
		return -1;
	}

	/* User-Agent, if the client sent one; recorded per visit. */
	req->user_agent[0] = '\0';
	const char *ua = strcasestr(head, "\r\nUser-Agent:");
	if (ua) {
		ua += strlen("\r\nUser-Agent:");
		while (*ua == ' ') {
			ua++;
		}
		const char *end = strstr(ua, "\r\n");
		size_t len = end ? (size_t)(end - ua) : 0;
		if (len > MAX_UA_BYTES - 1) {
			len = MAX_UA_BYTES - 1;
		}
		memcpy(req->user_agent, ua, len);
		req->user_agent[len] = '\0';
	}

	/* body: whatever Content-Length asks for, capped */
	req->body_len = 0;
	req->body[0] = '\0';
	long content_length = 0;
	const char *cl = strcasestr(head, "\r\nContent-Length:");
	if (cl) {
		content_length = strtol(cl + strlen("\r\nContent-Length:"), NULL, 10);
	}
	if (content_length <= 0) {
		return 0;
	}
	if (content_length > MAX_BODY_BYTES) {
		return -1;
	}

	const char *body_start = blank + 4;
	size_t already = have - (size_t)(body_start - head);
	if (already > (size_t)content_length) {
		already = (size_t)content_length;
	}
	memcpy(req->body, body_start, already);
	req->body_len = already;
	while (req->body_len < (size_t)content_length) {
		ssize_t n = read(fd, req->body + req->body_len,
		                 (size_t)content_length - req->body_len);
		if (n <= 0) {
			return -1;
		}
		req->body_len += (size_t)n;
	}
	req->body[req->body_len] = '\0';
	return 0;
}

/* ── handlers ────────────────────────────────────────────────────── */

/* Every request appends a row to the executable. */
static void record_visit(sqlite3 *db, const struct request *req)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db,
	                       "INSERT INTO visits (at, ua, path)"
	                       " VALUES (datetime('now'), ?, ?)",
	                       -1, &stmt, NULL) != SQLITE_OK) {
		return;
	}
	sqlite3_bind_text(stmt, 1, req->user_agent, -1, SQLITE_STATIC);
	sqlite3_bind_text(stmt, 2, req->path, -1, SQLITE_STATIC);
	sqlite3_step(stmt);
	sqlite3_finalize(stmt);
}

static void serve_stats(int fd, sqlite3 *db)
{
	struct buffer json = { 0 };
	buf_append(&json, "{", 1);
	for (size_t v = 0; v < TEMPLATE_VAR_COUNT; v++) {
		char *value = sql_scalar(db, TEMPLATE_VARS[v].sql);
		const char *shown = value ? value : "";
		if (TEMPLATE_VARS[v].kind == VALUE_NUMBER) {
			buf_printf(&json, "%s\"%s\":%s", v ? "," : "", TEMPLATE_VARS[v].name,
			           *shown ? shown : "0");
		} else {
			buf_printf(&json, "%s\"%s\":\"%s\"", v ? "," : "", TEMPLATE_VARS[v].name,
			           shown);
		}
		free(value);
	}
	buf_printf(&json, ",\"uptime_seconds\":%lld}", (long long)(time(NULL) - g_started_at));

	respond(fd, 200, JSON_MIME, json.data, json.len);
	buf_free(&json);
}

/* The file's own table of contents, counted live. `sqlite_schema` is the
 * section header table of this format, so this endpoint is `readelf -S`. */
static void serve_tables(int fd, sqlite3 *db)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db,
	                       "SELECT name FROM sqlite_schema WHERE type='table'"
	                       " AND name NOT LIKE 'sqlite_%' ORDER BY name",
	                       -1, &stmt, NULL) != SQLITE_OK) {
		respond_text(fd, 500, "cannot read sqlite_schema");
		return;
	}

	struct buffer json = { 0 };
	buf_append(&json, "[", 1);
	int first = 1;
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *name = (const char *)sqlite3_column_text(stmt, 0);
		if (!name) {
			continue;
		}
		char *count_sql = sqlite3_mprintf("SELECT count(*) FROM \"%w\"", name);
		char *rows = count_sql ? sql_scalar(db, count_sql) : NULL;
		sqlite3_free(count_sql);
		buf_printf(&json, "%s{\"name\":\"%s\",\"rows\":%s}", first ? "" : ",", name,
		           rows ? rows : "0");
		free(rows);
		first = 0;
	}
	sqlite3_finalize(stmt);
	buf_append(&json, "]", 1);

	respond(fd, 200, JSON_MIME, json.data, json.len);
	buf_free(&json);
}

/* Keep the stored button name to a harmless identifier: it is echoed back
 * out of the database into JSON. */
static void sanitize_button(const char *raw, size_t raw_len, char *out)
{
	size_t n = 0;
	for (size_t i = 0; i < raw_len && n < MAX_BUTTON_BYTES - 1; i++) {
		unsigned char c = (unsigned char)raw[i];
		if (isalnum(c) || c == '-' || c == '_') {
			out[n++] = (char)c;
		}
	}
	out[n] = '\0';
	if (n == 0) {
		snprintf(out, MAX_BUTTON_BYTES, "%s", DEFAULT_BUTTON);
	}
}

static void serve_press(int fd, sqlite3 *db, const struct request *req)
{
	char button[MAX_BUTTON_BYTES];
	sanitize_button(req->body, req->body_len, button);

	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db,
	                       "INSERT INTO presses (at, button)"
	                       " VALUES (datetime('now'), ?)",
	                       -1, &stmt, NULL) != SQLITE_OK) {
		respond_text(fd, 500, "cannot record the press");
		return;
	}
	sqlite3_bind_text(stmt, 1, button, -1, SQLITE_STATIC);
	int rc = sqlite3_step(stmt);
	sqlite3_finalize(stmt);
	if (rc != SQLITE_DONE) {
		respond_text(fd, 500, "cannot record the press");
		return;
	}

	char *total = sql_scalar(db, "SELECT count(*) FROM presses");
	char json[128];
	int n = snprintf(json, sizeof json, "{\"presses\":%s,\"button\":\"%s\"}",
	                 total ? total : "0", button);
	free(total);
	respond(fd, 200, JSON_MIME, json, (size_t)n);
}

/* Serve one row of `routes`. HTML bodies go through the template pass so the
 * counters on the page are as fresh as the request. */
static void serve_route(int fd, sqlite3 *db, const char *path)
{
	sqlite3_stmt *stmt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT mime, body FROM routes WHERE path = ?", -1, &stmt,
	                       NULL) != SQLITE_OK) {
		respond_text(fd, 500, "no routes table: this executable has no website in it");
		return;
	}
	sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
	if (sqlite3_step(stmt) != SQLITE_ROW) {
		sqlite3_finalize(stmt);
		respond_text(fd, 404, "no such row in routes\n");
		return;
	}

	const char *mime = (const char *)sqlite3_column_text(stmt, 0);
	const char *body = sqlite3_column_blob(stmt, 1);
	size_t len = (size_t)sqlite3_column_bytes(stmt, 1);
	if (!mime) {
		mime = "application/octet-stream";
	}

	if (strncmp(mime, "text/html", strlen("text/html")) == 0) {
		struct buffer page = render(db, body ? body : "", len);
		respond(fd, 200, mime, page.data ? page.data : "", page.len);
		buf_free(&page);
	} else {
		respond(fd, 200, mime, body ? body : "", len);
	}
	sqlite3_finalize(stmt);
}

static void handle(int fd, sqlite3 *db, struct request *req)
{
	/* strip the query string: routes are keyed on the path alone */
	char *query = strchr(req->path, '?');
	if (query) {
		*query = '\0';
	}

	record_visit(db, req);

	if (strcmp(req->method, "POST") == 0) {
		if (strcmp(req->path, "/api/press") == 0) {
			serve_press(fd, db, req);
			return;
		}
		respond_text(fd, 404, "no such endpoint\n");
		return;
	}

	if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
		respond_text(fd, 405, "GET, HEAD or POST only\n");
		return;
	}

	if (strcmp(req->path, "/api/stats") == 0) {
		serve_stats(fd, db);
		return;
	}
	if (strcmp(req->path, "/api/tables") == 0) {
		serve_tables(fd, db);
		return;
	}

	serve_route(fd, db, strcmp(req->path, "/") == 0 ? INDEX_ROUTE : req->path);
}

/* One connection, one request, then close. The timeouts are what stop a
 * client that opens a socket and says nothing from occupying a worker. */
static void serve_connection(int fd, sqlite3 *db)
{
	struct timeval timeout = { .tv_sec = REQUEST_TIMEOUT_SECONDS, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

	struct request req = { 0 };
	if (read_request(fd, &req) != 0) {
		respond_text(fd, 400, "malformed request\n");
		return;
	}

	handle(fd, db, &req);

	printf("%s %s ua=%.60s\n", req.method, req.path, req.user_agent);
	fflush(stdout);
}

/* A worker opens the executable once and keeps the connection for its whole
 * life. Opening a SQLite database per request costs several milliseconds --
 * far more than the queries themselves -- and there is no reason to pay it
 * on a socket that is already accepted. Every worker accept()s the same
 * listening socket; the kernel picks one. */
static void worker_loop(int listener)
{
	sqlite3 *db = db_open();
	if (!db) {
		_exit(1);
	}

	while (!g_stopping) {
		int fd = accept(listener, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR || errno == ECONNABORTED) {
				continue;
			}
			perror("accept");
			break;
		}
		serve_connection(fd, db);
		close(fd);
	}

	/* Close, do not just exit: a clean close is what checkpoints and
	 * unlinks any journal, leaving the artifact as the single file it
	 * claims to be. */
	sqlite3_close(db);
	_exit(g_stopping ? 0 : 1);
}

/* SIGTERM must interrupt accept(), so the handler is installed without
 * SA_RESTART -- signal(2) would give the opposite. */
static void on_terminate(int signum)
{
	(void)signum;
	g_stopping = 1;
}

static void install_terminate_handler(void)
{
	struct sigaction action = { .sa_handler = on_terminate };
	sigemptyset(&action.sa_mask);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGINT, &action, NULL);
}

/* ── startup ─────────────────────────────────────────────────────── */

/* The site's tables must already exist: build.sh puts them (and the pages)
 * into the executable with SQL. Refusing to create them keeps the artifact
 * honest -- a server with no website in it says so. */
static int check_site(void)
{
	sqlite3 *db = db_open();
	if (!db) {
		return -1;
	}

	char *routes = sql_scalar(db, "SELECT count(*) FROM routes");
	char *ok = sql_scalar(db,
	                      "SELECT count(*) FROM sqlite_schema"
	                      " WHERE type='table' AND name IN ('routes','visits','presses')");
	int complete = ok && atoi(ok) == 3;
	if (complete) {
		printf("self-httpd: serving %s routes out of %s\n", routes ? routes : "0",
		       g_db_path);
	} else {
		fprintf(stderr,
		        "self-httpd: %s has no website in it (need tables routes, visits,"
		        " presses -- see examples/server/build.sh)\n",
		        g_db_path);
	}
	free(routes);
	free(ok);
	sqlite3_close(db);
	return complete ? 0 : -1;
}

static int usage(const char *argv0)
{
	fprintf(stderr,
	        "usage: %s [--journal delete|wal] [port]\n"
	        "  delete  one file on disk while serving; ~1.4 ms/request (default)\n"
	        "  wal     ~3x faster, plus -wal and -shm while the server runs\n"
	        "environment: $PORT, $SELF_HTTPD_JOURNAL\n",
	        argv0);
	return 2;
}

static int listen_on(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return -1;
	}

	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons((uint16_t)port),
	};
	if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
		fprintf(stderr, "self-httpd: bind port %d: %s\n", port, strerror(errno));
		close(fd);
		return -1;
	}
	if (listen(fd, LISTEN_BACKLOG) != 0) {
		perror("listen");
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv)
{
	/* argv[0] is the .self file itself -- see the header comment. Resolve it
	 * once, before anything can change the working directory. */
	if (!realpath(argv[0], g_db_path)) {
		snprintf(g_db_path, sizeof g_db_path, "%s", argv[0]);
	}
	g_started_at = time(NULL);

	int port = DEFAULT_PORT;
	enum journal_mode journal = DEFAULT_JOURNAL_MODE;

	/* The environment first, then the command line, so a systemd unit can set
	 * either one and the flag still wins when both are present. */
	const char *port_env = getenv("PORT");
	if (port_env && *port_env) {
		port = atoi(port_env);
	}
	const char *journal_env = getenv("SELF_HTTPD_JOURNAL");
	if (journal_env && *journal_env && parse_journal_mode(journal_env, &journal) != 0) {
		fprintf(stderr, "self-httpd: unknown $SELF_HTTPD_JOURNAL '%s'\n", journal_env);
		return usage(argv[0]);
	}

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			usage(argv[0]);
			return 0;
		}
		if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
			if (parse_journal_mode(argv[++i], &journal) != 0) {
				fprintf(stderr, "self-httpd: unknown journal mode '%s'\n", argv[i]);
				return usage(argv[0]);
			}
			continue;
		}
		port = atoi(argv[i]);
	}

	if (port <= 0 || port > 65535) {
		return usage(argv[0]);
	}

	if (apply_journal_mode(journal) != 0 || check_site() != 0) {
		return 1;
	}

	int listener = listen_on(port);
	if (listener < 0) {
		return 1;
	}
	printf("self-httpd: listening on http://0.0.0.0:%d with %d workers"
	       " (journal=%s)\n",
	       port, WORKER_COUNT, JOURNAL_MODES[journal].name);
	fflush(stdout);

	/* A broken client must not take the process down with a SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);
	install_terminate_handler();

	pid_t workers[WORKER_COUNT];
	for (int i = 0; i < WORKER_COUNT; i++) {
		workers[i] = fork();
		if (workers[i] == 0) {
			worker_loop(listener);
		}
	}

	/* The parent owns nothing but the pool: it holds no database connection
	 * and answers no requests, so a worker that dies is replaced without the
	 * site going down. */
	while (!g_stopping) {
		int status = 0;
		pid_t gone = wait(&status);
		if (gone < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}

		fprintf(stderr, "self-httpd: worker %d exited (status %d), respawning\n",
		        (int)gone, status);
		sleep(RESPAWN_DELAY_SECONDS);
		for (int i = 0; i < WORKER_COUNT; i++) {
			if (workers[i] != gone) {
				continue;
			}
			workers[i] = fork();
			if (workers[i] == 0) {
				worker_loop(listener);
			}
			break;
		}
	}

	/* Shutdown: let every worker close its connection before exiting. */
	for (int i = 0; i < WORKER_COUNT; i++) {
		if (workers[i] > 0) {
			kill(workers[i], SIGTERM);
		}
	}
	for (;;) {
		errno = 0;
		if (wait(NULL) < 0 && errno != EINTR) {
			break;
		}
	}
	close(listener);
	printf("self-httpd: stopped\n");
	return 0;
}
