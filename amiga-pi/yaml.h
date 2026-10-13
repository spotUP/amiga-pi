/* yaml -- the YAML reading that pi's frontmatter needs (utils/frontmatter.ts: `parse` of the yaml package, 2.9.0), with that
 * package's errors: the first error `parse` would throw, with its message, position and context lines (prettifyError), or
 * the values of some top-level keys. A port of the package's lexer, CST parser and composer (yaml.c, yaml_lex.c,
 * yaml_cst.c), not a YAML subset, so duplicate keys, an unclosed quote, tabs as indentation, a bad flow collection, a block
 * scalar header with extra characters, reserved characters, bad escapes and the rest are refused as pi refuses them.
 *
 * Differences: documents with a %YAML 1.1 directive are read as 1.2; the warnings of the package (unresolved tags) are not
 * kept (pi prints them to stderr); an explicit !!set, !!omap, !!pairs, !!binary or !!timestamp is read as a value that is
 * not a string and its own errors are not raised; "Excessive alias count" is not raised; a string value holding a NUL
 * character (the \0 escape) ends at it (the C string); the nesting depth is limited to 1500 (pi: the JavaScript stack, with
 * the message "Maximum call stack size exceeded").
 * Portable C89, host-tested (tests/test_pi_yaml.c against pi's own yaml package). */
#ifndef PI_YAML_H
#define PI_YAML_H

enum { YV_NONE, YV_STR, YV_TRUE, YV_OTHER };    /* the key is absent; a string; the boolean true; anything else (null, a number, false, a collection) */

typedef struct pi_yv {
    int kind;
    char *s;                    /* YV_STR: the string (malloc'ed) */
} pi_yv;

/* Reads text (n bytes, the YAML only) as `parse(text)` does and looks up the top-level keys. 0 ok: vals[i] says what keys[i] is
 * (a document that is not a mapping has none of them); -1 out of memory; -2 pi's parser throws: err holds its message. */
int pi_yaml_keys(const char *text, long n, const char *const *keys, int nkeys, pi_yv *vals, char *err, long errcap);
void pi_yv_free(pi_yv *vals, int n);

/* the text of the first error only, or "" when parse would not throw (for tests): 1 error, 0 none, -1 out of memory */
int pi_yaml_error(const char *text, long n, char *err, long errcap);

#endif
