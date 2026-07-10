#ifndef SELF_IMAGE_H
#define SELF_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

#define SELF_APPLICATION_ID 0x53454C46u /* 'SELF' */
#define EHDR_SIZE 64
#define PHDR_SIZE 56

/* All exit-free: return NULL / -1 on error. */
sqlite3 *self_db_open(const char *path);
int64_t self_get_int(sqlite3 *db, const char *key, int *found);
char *self_get_text(sqlite3 *db, const char *key);
void *self_build_elf(sqlite3 *db, size_t *out_len);
int self_materialize_memfd(const char *path, const char *tag);

#endif
