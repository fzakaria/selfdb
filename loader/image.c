/*
 * image.c: shared, exit-free helpers to open a .self and reconstruct its
 * ELF image from the rows. Used by self-exec (memfd mode) and by
 * libself-audit.so (materializing .self libraries for stock ld.so).
 *
 * These functions never call exit(): they return NULL / -1 so a library
 * caller can fall back gracefully.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <sqlite3.h>

#include "image.h"

sqlite3 *self_db_open(const char *path) {
	sqlite3 *db = NULL;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return NULL;
	}
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "PRAGMA application_id", -1, &st, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return NULL;
	}
	uint32_t app_id = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		app_id = (uint32_t)sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	if (app_id != SELF_APPLICATION_ID) {
		sqlite3_close(db);
		return NULL;
	}
	return db;
}

int64_t self_get_int(sqlite3 *db, const char *key, int *found) {
	sqlite3_stmt *st;
	int64_t v = 0, ok = 0;
	sqlite3_prepare_v2(db, "SELECT value FROM self_meta WHERE key=?", -1, &st, NULL);
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW) {
		v = sqlite3_column_int64(st, 0);
		ok = 1;
	}
	sqlite3_finalize(st);
	if (found)
		*found = ok;
	return v;
}

char *self_get_text(sqlite3 *db, const char *key) {
	sqlite3_stmt *st;
	char *v = NULL;
	sqlite3_prepare_v2(db, "SELECT value FROM self_meta WHERE key=?", -1, &st, NULL);
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	if (sqlite3_step(st) == SQLITE_ROW &&
	    sqlite3_column_type(st, 0) != SQLITE_NULL) {
		const char *s = (const char *)sqlite3_column_text(st, 0);
		if (s)
			v = strdup(s);
	}
	sqlite3_finalize(st);
	return v;
}

/* Reconstruct the ELF image from self_meta + segments. Returns a malloc'd
 * buffer of *out_len bytes, or NULL on error. Identical byte layout to
 * selfconv.elfimage.serialize_image and the C self2elf twin. */
void *self_build_elf(sqlite3 *db, size_t *out_len) {
	int ok = 1, f;
	int64_t et = self_get_int(db, "et", &f);
	ok &= f;
	int64_t em = self_get_int(db, "em", &f);
	ok &= f;
	int64_t entry = self_get_int(db, "entry", &f);
	ok &= f;
	int64_t phoff = self_get_int(db, "phoff", &f);
	ok &= f;
	int64_t eflags = self_get_int(db, "eflags", &f);
	ok &= f;
	int64_t phnum = self_get_int(db, "phnum", &f);
	ok &= f;
	int64_t osabi = self_get_int(db, "osabi", &f);
	ok &= f;
	if (!ok)
		return NULL;

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db,
		"SELECT ptype, offset, vaddr, filesz, memsz, r, w, x, align, content"
		" FROM segments ORDER BY id", -1, &st, NULL) != SQLITE_OK)
		return NULL;

	size_t size = EHDR_SIZE;
	if ((size_t)(phoff + phnum * PHDR_SIZE) > size)
		size = phoff + phnum * PHDR_SIZE;
	struct row {
		int64_t ptype, offset, vaddr, filesz, memsz, r, w, x, align;
		void *content;
		int clen;
	} *rows = calloc(phnum ? phnum : 1, sizeof *rows);
	int n = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		struct row *R = &rows[n++];
		R->ptype = sqlite3_column_int64(st, 0);
		R->offset = sqlite3_column_int64(st, 1);
		R->vaddr = sqlite3_column_int64(st, 2);
		R->filesz = sqlite3_column_int64(st, 3);
		R->memsz = sqlite3_column_int64(st, 4);
		R->r = sqlite3_column_int64(st, 5);
		R->w = sqlite3_column_int64(st, 6);
		R->x = sqlite3_column_int64(st, 7);
		R->align = sqlite3_column_int64(st, 8);
		const void *blob = sqlite3_column_blob(st, 9);
		R->clen = sqlite3_column_bytes(st, 9);
		if (blob && R->clen) {
			R->content = malloc(R->clen);
			memcpy(R->content, blob, R->clen);
		}
		if ((size_t)(R->offset + R->filesz) > size)
			size = R->offset + R->filesz;
	}
	sqlite3_finalize(st);

	uint8_t *img = calloc(1, size);
	if (!img) {
		for (int i = 0; i < n; i++)
			free(rows[i].content);
		free(rows);
		return NULL;
	}
	static const uint8_t ident0[8] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
	memcpy(img, ident0, 8);
	img[7] = (uint8_t)osabi;
	struct __attribute__((packed)) {
		uint16_t type, machine;
		uint32_t version;
		uint64_t entry, phoff, shoff;
		uint32_t flags;
		uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
	} *eh = (void *)(img + 16);
	eh->type = et;
	eh->machine = em;
	eh->version = 1;
	eh->entry = entry;
	eh->phoff = phoff;
	eh->flags = eflags;
	eh->ehsize = EHDR_SIZE;
	eh->phentsize = PHDR_SIZE;
	eh->phnum = phnum;
	for (int i = 0; i < n; i++) {
		struct row *R = &rows[i];
		struct __attribute__((packed)) {
			uint32_t type, flags;
			uint64_t offset, vaddr, paddr, filesz, memsz, align;
		} *ph = (void *)(img + phoff + (size_t)i * PHDR_SIZE);
		ph->type = R->ptype;
		ph->flags = (R->r ? 4 : 0) | (R->w ? 2 : 0) | (R->x ? 1 : 0);
		ph->offset = R->offset;
		ph->vaddr = R->vaddr;
		ph->paddr = R->vaddr;
		ph->filesz = R->filesz;
		ph->memsz = R->memsz;
		ph->align = R->align;
		if (R->content && R->clen)
			memcpy(img + R->offset, R->content, R->clen);
		free(R->content);
	}
	free(rows);
	*out_len = size;
	return img;
}

/* Open a .self, reconstruct its ELF, and stash it in a memfd. Returns the
 * fd (seekable, at offset 0) or -1. Caller can pass /proc/self/fd/<n> to
 * anything that opens files by path (e.g. ld.so). */
int self_materialize_memfd(const char *path, const char *tag) {
	sqlite3 *db = self_db_open(path);
	if (!db)
		return -1;
	size_t len = 0;
	void *img = self_build_elf(db, &len);
	sqlite3_close(db);
	if (!img)
		return -1;
	int fd = memfd_create(tag ? tag : "self", MFD_CLOEXEC);
	if (fd < 0) {
		free(img);
		return -1;
	}
	for (size_t off = 0; off < len;) {
		ssize_t w = write(fd, (uint8_t *)img + off, len - off);
		if (w < 0) {
			free(img);
			close(fd);
			return -1;
		}
		off += w;
	}
	free(img);
	lseek(fd, 0, SEEK_SET);
	return fd;
}
