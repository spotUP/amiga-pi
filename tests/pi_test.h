/* amiga-pi's host suites: vtcon's harness (CHECK, CHECK_INT, CHECK_STR) and
 * goldens of their own under tests/ (PI_TESTS, set by the Makefile; the
 * suites run in VTCON_DIR for its recordings). PI_UPDATE_GOLDEN=1 writes them. */
#ifndef PI_TEST_H
#define PI_TEST_H

#include "harness.h"

/* got against PI_TESTS/<name>: 1 equal, else a failed check and both shown */
int pi_golden(const char *name, const char *got);

#endif
