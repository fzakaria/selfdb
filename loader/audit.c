/*
 * libself-audit.so: an rtld-audit library (M3a) that makes glibc's own
 * ld.so load SELF (SQLite) shared libraries.
 *
 * glibc calls la_objsearch() for every soname it is about to search for,
 * before touching the filesystem. We look the soname up in a resolver
 * database (built by `self scan`, path in $SELF_SYSTEM_DB) and, if the hit
 * is a .self library, reconstruct it into a memfd and hand ld.so
 * /proc/self/fd/<n>. Stock ld.so then maps and relocates it, none the
 * wiser -- so lazy binding, IFUNCs, TLS and symbol versioning all keep
 * working while library storage is rows and lookup is SQL.
 *
 * Enable with:  LD_AUDIT=libself-audit.so  SELF_SYSTEM_DB=/var/lib/self/system.db
 */
#define _GNU_SOURCE
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "image.h"

static sqlite3 *g_db;
static int g_debug;

unsigned int la_version(unsigned int version) {
	const char *dbpath = getenv("SELF_SYSTEM_DB");
	g_debug = getenv("SELF_AUDIT_DEBUG") != NULL;
	if (dbpath &&
	    sqlite3_open_v2(dbpath, &g_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		g_db = NULL;
	}
	if (g_debug)
		fprintf(stderr, "[self-audit] version=%u db=%s\n", version,
			g_db ? getenv("SELF_SYSTEM_DB") : "(none)");
	return version; /* accept whatever rtld offers */
}

/* Look a soname up in the resolver DB; return a malloc'd path or NULL. */
static char *resolve(const char *soname) {
	if (!g_db)
		return NULL;
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(g_db,
		"SELECT path, kind FROM objects WHERE soname = ? LIMIT 1",
		-1, &st, NULL) != SQLITE_OK)
		return NULL;
	sqlite3_bind_text(st, 1, soname, -1, SQLITE_STATIC);
	char *result = NULL;
	if (sqlite3_step(st) == SQLITE_ROW) {
		const char *path = (const char *)sqlite3_column_text(st, 0);
		const char *kind = (const char *)sqlite3_column_text(st, 1);
		if (path && kind && strcmp(kind, "self") == 0) {
			/* materialize the .self into a memfd, hand back its
			 * /proc path so ld.so can open it normally */
			int fd = self_materialize_memfd(path, soname);
			if (fd >= 0) {
				char buf[64];
				snprintf(buf, sizeof buf, "/proc/self/fd/%d", fd);
				result = strdup(buf);
			}
		} else if (path && kind && strcmp(kind, "elf") == 0) {
			result = strdup(path); /* plain ELF: use directly */
		}
	}
	sqlite3_finalize(st);
	return result;
}

char *la_objsearch(const char *name, uintptr_t *cookie, unsigned int flag) {
	(void)cookie;
	/* Only intervene on the initial soname (as it appears in DT_NEEDED /
	 * dlopen). Once we have rewritten it to a /proc path, let it pass. */
	if (flag == LA_SER_ORIG && strncmp(name, "/proc/self/fd/", 14) != 0 &&
	    !strchr(name, '/')) {
		char *hit = resolve(name);
		if (hit) {
			if (g_debug)
				fprintf(stderr, "[self-audit] %s -> %s\n", name, hit);
			return hit; /* ld.so frees this */
		}
	}
	return (char *)name; /* fall through to ld.so's normal search */
}
