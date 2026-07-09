/*
 * native.c: the M2 loader (map segments ourselves, hand off to ld.so).
 * Stubbed until M2; the memfd path in self-exec.c is the M1 deliverable.
 */
#define _GNU_SOURCE
#include "self.h"

int run_native(sqlite3 *db, const char *path, char **argv, char **envp) {
	(void)db;
	(void)path;
	(void)argv;
	(void)envp;
	die("native mode not yet implemented (M2)");
	return 127;
}
