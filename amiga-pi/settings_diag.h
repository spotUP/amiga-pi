/* settings_diag -- what pi tells of a settings.json it cannot read, after pi 1.1.0 core/settings-manager.ts (loadFromStorage,
 * tryLoadFromStorage, migrateSettings: JSON.parse of the file without its BOM, then `"queueMode" in settings`) and
 * core/settings-diagnostics.ts (collectSettingsDiagnostics): the warning "Invalid settings file <path>: <message>", the message
 * the one V8's JSON.parse (Node 24) gives: "Unexpected non-whitespace character after JSON at position 11 (line 1 column 12)",
 * "Expected property name or '}' in JSON at position 1 (line 1 column 2)", "Unexpected token '}', "{"a":}" is not valid JSON",
 * "Unexpected end of JSON input". Positions are UTF-16 code units, as V8 counts. Portable C89, host-tested (tests/test_pi_tui.c). */
#ifndef PI_SETTINGS_DIAG_H
#define PI_SETTINGS_DIAG_H

#include "claude/json.h"

/* the text of a settings file (n bytes, UTF-8, a leading BOM allowed) as pi's loader meets it: 0 when it loads (an empty file loads
 * as {}), else 1 with the error's message appended to msg (JSON.parse's, or the TypeError of a JSON value that is no object) */
int pi_settings_error(const char *text, long n, jw *msg);
/* "Invalid settings file <path>: <message>" appended to out, one diagnostic as collectSettingsDiagnostics words it */
void pi_settings_warning(const char *path, const char *message, jw *out);

#endif
