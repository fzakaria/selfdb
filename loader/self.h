#ifndef SELF_H
#define SELF_H

#include <stdint.h>
#include <sqlite3.h>

#define SELF_APPLICATION_ID 0x53454C46u /* 'SELF' */
#define EHDR_SIZE 64
#define PHDR_SIZE 56

/* self-exec.c */
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
sqlite3 *self_open(const char *path);
int64_t self_meta_int(sqlite3 *db, const char *key);
/* returns a malloc'd string or NULL if the key is absent/NULL */
char *self_meta_text(sqlite3 *db, const char *key);

/* native.c (M2) */
int run_native(sqlite3 *db, const char *path, char **argv, char **envp);

#endif
