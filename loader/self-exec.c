/*
 * self-exec: the SELF binfmt_misc interpreter.
 *
 * Modes (DESIGN.md §5):
 *   memfd  (M1, default): reconstruct the ELF image from the `segments`
 *          rows into a memfd, then execveat() it. The rows are
 *          authoritative -- this is the same row->image serialization as
 *          the Python self2elf, in C.
 *   native (M2): map segments ourselves and hand off to ld.so; see
 *          native.c.
 *
 * Invoked by the kernel as: self-exec <program.self> <original argv...>.
 * With binfmt 'P' (preserve argv[0]) the kernel already arranges argv; we
 * exec with argv untouched from argv[1] onward.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <sqlite3.h>

#include "self.h"

static const char *g_argv0 = "self-exec";

void die(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "%s: ", g_argv0);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(127);
}

/* Open a .self read-only and verify it is really ours. */
sqlite3 *self_open(const char *path) {
	sqlite3 *db;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		die("cannot open %s: %s", path, sqlite3_errmsg(db));

	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db, "PRAGMA application_id", -1, &st, NULL) != SQLITE_OK)
		die("prepare application_id: %s", sqlite3_errmsg(db));
	if (sqlite3_step(st) != SQLITE_ROW)
		die("%s: no application_id", path);
	int app_id = sqlite3_column_int(st, 0);
	sqlite3_finalize(st);
	if ((uint32_t)app_id != SELF_APPLICATION_ID)
		die("%s: application_id 0x%08x is not 'SELF'", path, (uint32_t)app_id);
	return db;
}

int64_t self_meta_int(sqlite3 *db, const char *key) {
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT value FROM self_meta WHERE key=?", -1, &st, NULL);
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	int64_t v = 0;
	if (sqlite3_step(st) == SQLITE_ROW)
		v = sqlite3_column_int64(st, 0);
	else
		die("self_meta missing key '%s'", key);
	sqlite3_finalize(st);
	return v;
}

char *self_meta_text(sqlite3 *db, const char *key) {
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db, "SELECT value FROM self_meta WHERE key=?", -1, &st, NULL);
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	char *v = NULL;
	if (sqlite3_step(st) == SQLITE_ROW &&
	    sqlite3_column_type(st, 0) != SQLITE_NULL) {
		const char *s = (const char *)sqlite3_column_text(st, 0);
		if (s)
			v = strdup(s);
	}
	sqlite3_finalize(st);
	return v;
}

/* Reconstruct the ELF image from self_meta + segments into a fresh buffer.
 * Byte-for-byte identical to selfconv.elfimage.serialize_image. */
static uint8_t *build_image(sqlite3 *db, size_t *out_len) {
	struct ehdr_fields {
		int64_t et, em, entry, phoff, eflags, phnum, phentsize, osabi;
	} e = {
	    .et = self_meta_int(db, "et"),
	    .em = self_meta_int(db, "em"),
	    .entry = self_meta_int(db, "entry"),
	    .phoff = self_meta_int(db, "phoff"),
	    .eflags = self_meta_int(db, "eflags"),
	    .phnum = self_meta_int(db, "phnum"),
	    .phentsize = self_meta_int(db, "phentsize"),
	    .osabi = self_meta_int(db, "osabi"),
	};

	/* Pass 1: figure out the total image size. */
	sqlite3_stmt *st;
	sqlite3_prepare_v2(db,
	    "SELECT ptype, offset, vaddr, filesz, memsz, r, w, x, align, content"
	    " FROM segments ORDER BY id", -1, &st, NULL);
	size_t size = EHDR_SIZE;
	if ((size_t)(e.phoff + e.phnum * PHDR_SIZE) > size)
		size = e.phoff + e.phnum * PHDR_SIZE;
	/* Stash rows so we do not run the query twice. The blob pointer from
	 * sqlite3_column_blob is only valid until the next sqlite3_step, so we
	 * copy the bytes out immediately. */
	struct row {
		int64_t ptype, offset, vaddr, filesz, memsz, r, w, x, align;
		void *content;
		int clen;
	} *rows = calloc(e.phnum, sizeof *rows);
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

	uint8_t *img = calloc(1, size);
	if (!img)
		die("out of memory for %zu-byte image", size);

	/* ELF header */
	static const uint8_t ident0[8] = {0x7f, 'E', 'L', 'F', 2, 1, 1, 0};
	memcpy(img, ident0, 8);
	img[7] = (uint8_t)e.osabi;
	struct __attribute__((packed)) {
		uint16_t type, machine;
		uint32_t version;
		uint64_t entry, phoff, shoff;
		uint32_t flags;
		uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
	} *eh = (void *)(img + 16);
	eh->type = e.et;
	eh->machine = e.em;
	eh->version = 1;
	eh->entry = e.entry;
	eh->phoff = e.phoff;
	eh->shoff = 0;
	eh->flags = e.eflags;
	eh->ehsize = EHDR_SIZE;
	eh->phentsize = PHDR_SIZE;
	eh->phnum = e.phnum;

	/* program headers + load contents */
	for (int i = 0; i < n; i++) {
		struct row *R = &rows[i];
		struct __attribute__((packed)) {
			uint32_t type, flags;
			uint64_t offset, vaddr, paddr, filesz, memsz, align;
		} *ph = (void *)(img + e.phoff + (size_t)i * PHDR_SIZE);
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
	sqlite3_finalize(st);
	free(rows);
	*out_len = size;
	return img;
}

static int run_memfd(sqlite3 *db, char **argv, char **envp) {
	size_t len;
	uint8_t *img = build_image(db, &len);

	int fd = memfd_create("self", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create: %m");
	for (size_t off = 0; off < len;) {
		ssize_t w = write(fd, img + off, len - off);
		if (w < 0)
			die("write memfd: %m");
		off += w;
	}
	free(img);
	sqlite3_close(db);

	/* execveat(fd, "", argv, envp, AT_EMPTY_PATH) runs the memfd image. */
	syscall(SYS_execveat, fd, "", argv, envp, AT_EMPTY_PATH);
	die("execveat memfd: %m");
	return 127;
}

int main(int argc, char **argv, char **envp) {
	g_argv0 = argv[0];
	const char *mode = getenv("SELF_MODE");
	if (!mode)
		mode = "memfd";

	/* argv layout from the kernel: [self-exec, program.self, real args...].
	 * We exec with argv+1 so the target sees its own argv[0]. */
	if (argc < 2)
		die("usage: self-exec <program.self> [args...]");
	const char *path = argv[1];

	sqlite3 *db = self_open(path);

	if (strcmp(mode, "memfd") == 0)
		return run_memfd(db, argv + 1, envp);
	if (strcmp(mode, "native") == 0)
		return run_native(db, path, argv + 1, envp);
	die("unknown SELF_MODE '%s' (memfd|native)", mode);
}
