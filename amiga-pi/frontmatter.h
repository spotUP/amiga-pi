/* frontmatter -- pi's utils/frontmatter.ts parseFrontmatter, the one reader skills and prompt templates share: the file is
 * read as UTF-8 text with a byte order mark taken off and CR / CRLF turned into LF; when it starts with "---" and a later
 * "\n---" closes it, what is between is YAML (read as the yaml package does, yaml.h, with its errors) and what follows is the
 * body, trimmed the way JavaScript's trim() does. No frontmatter (or none that closes): no keys, the whole text is the body.
 * Portable C89, host-tested (tests/test_pi_yaml.c, test_pi_resources.c, test_pi_templates.c). */
#ifndef PI_FRONTMATTER_H
#define PI_FRONTMATTER_H

#include "yaml.h"

/* content (n bytes) as parseFrontmatter reads it. keys[0..nkeys) are top-level keys to look up; vals[i] says what keys[i] is
 * (YV_NONE for absent, a document that is no mapping, or no frontmatter). *body, when body is not 0, is the body (malloc'ed,
 * NUL ended; *bodylen its length). 0 ok, -1 out of memory, -2 the YAML is refused: err holds pi's message (vals and *body are
 * then empty). */
int pi_fm_parse(const char *content, long n, const char *const *keys, int nkeys, pi_yv *vals, char **body, long *bodylen, char *err,
                long errcap);

/* JavaScript's trim() whitespace (WhiteSpace and LineTerminator) in UTF-8: the length of the space starting at p (0 when p is
 * not one), and of the space that ends at e (not before s) */
int pi_js_space_len(const unsigned char *p);
int pi_js_space_back(const unsigned char *s, const unsigned char *e);

#endif
