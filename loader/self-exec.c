/*
 * self-exec: the SELF binfmt_misc interpreter.
 *
 * Modes (DESIGN.md §5):
 *   memfd  (M1, default): reconstruct the ELF image from the `segments`
 *          rows into a memfd, then execveat() it. The rows are
 *          authoritative -- the row->image serialization lives in image.c
 *          and matches the Python self2elf twin.
 *   native (M2): map segments ourselves and hand off to ld.so; see native.c.
 *   selfld (M3b): resolve NEEDED .self libraries via SQL and bind them
 *          ourselves; see selfld.c.
 *
 * Kernel argv to the interpreter (fs/binfmt_misc.c, no P/O flags):
 *   [self-exec, <binary path>, <original argv[1:]>]
 * We open argv[1] and re-exec with argv[1:], so the target's argv[0] is the
 * binary path (basename() still satisfies multi-call binaries).
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

#include "image.h"
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

/* die-based wrappers over the exit-free image.c helpers, for native.c. */
sqlite3 *self_open(const char *path) {
	sqlite3 *db = self_db_open(path);
	if (!db)
		die("%s: cannot open or not a SELF database", path);
	return db;
}

int64_t self_meta_int(sqlite3 *db, const char *key) {
	int found;
	int64_t v = self_get_int(db, key, &found);
	if (!found)
		die("self_meta missing key '%s'", key);
	return v;
}

char *self_meta_text(sqlite3 *db, const char *key) {
	return self_get_text(db, key);
}

static int run_memfd(sqlite3 *db, char **argv, char **envp) {
	size_t len;
	void *img = self_build_elf(db, &len);
	if (!img)
		die("failed to reconstruct ELF image from rows");
	sqlite3_close(db);

	int fd = memfd_create("self", MFD_CLOEXEC);
	if (fd < 0)
		die("memfd_create: %m");
	for (size_t off = 0; off < len;) {
		ssize_t w = write(fd, (uint8_t *)img + off, len - off);
		if (w < 0)
			die("write memfd: %m");
		off += w;
	}
	free(img);

	syscall(SYS_execveat, fd, "", argv, envp, AT_EMPTY_PATH);
	die("execveat memfd: %m");
	return 127;
}

int main(int argc, char **argv, char **envp) {
	g_argv0 = argv[0];
	const char *mode = getenv("SELF_MODE");
	if (!mode)
		mode = "memfd";

	if (argc < 2)
		die("usage: self-exec <program.self> [args...]");
	const char *path = argv[1];

	sqlite3 *db = self_open(path);

	if (strcmp(mode, "memfd") == 0)
		return run_memfd(db, argv + 1, envp);
	if (strcmp(mode, "native") == 0)
		return run_native(db, path, argv + 1, envp);
	if (strcmp(mode, "selfld") == 0)
		return run_selfld(db, path, argv + 1, envp);
	die("unknown SELF_MODE '%s' (memfd|native|selfld)", mode);
}
