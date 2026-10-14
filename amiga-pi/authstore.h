/* authstore -- a JSON object file of the agent directory edited one member at a time (plan phase O2): auth.json, where
 * pi keeps {"<provider>": {"type":"oauth"|"api_key", ...}}, and settings.json's deviceId. Every other member is kept as
 * it is (their raw text, in their order), whatever this version knows about it, so pi's own entries, API keys and fields
 * of a newer pi come back unchanged. The file is written in pi's layout, JSON.stringify(value, null, 2).
 * A write replaces the file whole: the new text goes to <name>.new first, then it takes the old one's place -- one
 * rename on a host; on AmigaDOS (where Rename will not overwrite) the old file steps aside to <name>.old for the moment,
 * and a read that finds <name> gone takes <name>.new, else <name>.old, so an interrupted write loses nothing.
 * Portable C89, host-tested (tests/test_pi_login.c). */
#ifndef PI_AUTHSTORE_H
#define PI_AUTHSTORE_H

#include "claude/json.h"
#include "claude/sys.h"

/* the file's text (malloc'ed, terminated, *n its length), 0 when there is none or it is empty; the recovery above */
char *pi_store_read(cl_sys *sys, const char *dir, const char *name, long *n);
/* the file replaced by text[0..n): 0, -1 with the words in err */
int pi_store_write(cl_sys *sys, const char *dir, const char *name, const char *text, long n, char *err, long cap);

/* the object text with member `key` set to the JSON text `value` (replaced where it stands, else added at the end) or,
 * value 0, removed, laid out as pi writes it; text 0 or blank is {}. 0, -1 when text is not a JSON object (err says so;
 * the file is then not to be written over), -2 out of memory */
int pi_jobj_set(const char *text, long n, const char *key, const char *value, jw *out, char *err, long cap);

/* settings.json's string member key1 set to val1 and, when key2 is not 0, key2 to val2; every other member kept (the file made when missing).
 * 0, -1 when it could not be read as an object or written */
int pi_store_set_member(cl_sys *sys, const char *dir, const char *key1, const char *val1, const char *key2, const char *val2);

/* the member's value as a malloc'ed string, 0 when absent or not a string */
char *pi_jobj_str(const char *text, long n, const char *key);

#endif
