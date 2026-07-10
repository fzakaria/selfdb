#ifndef SELF_H
#define SELF_H

#include <stdint.h>
#include <sqlite3.h>

/* self-exec.c: die() and die-based meta accessors used across the loader. */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
sqlite3 *self_open(const char *path);
int64_t self_meta_int(sqlite3 *db, const char *key);
char *self_meta_text(sqlite3 *db, const char *key); /* malloc'd or NULL */

/* native.c (M2) */
int run_native(sqlite3 *db, const char *path, char **argv, char **envp);

/* selfld.c (M3b) */
int run_selfld(sqlite3 *db, const char *path, char **argv, char **envp);

#endif
