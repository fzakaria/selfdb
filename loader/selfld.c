/*
 * selfld.c: the M3b eager SQL binder. Stubbed until M3b; see native.c (M2)
 * and audit.c (M3a) for the working execution paths.
 */
#define _GNU_SOURCE
#include "self.h"

int run_selfld(sqlite3 *db, const char *path, char **argv, char **envp) {
	(void)db;
	(void)path;
	(void)argv;
	(void)envp;
	die("selfld mode not yet implemented (M3b)");
	return 127;
}
