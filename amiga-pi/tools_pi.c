/* see tools_pi.h */
#include <stdlib.h>
#include <string.h>
#include "tools_pi.h"
#include "truncate.h"
#include "image.h"
#include "claude/tools_int.h"   /* tl_resolve: ls's path as C:Claude's tools resolve one */
#include "claude/schema.h"
#include "claude/util.h"
#include "llm/llm.h"

/* ---- pi's definitions (descriptions and schemas as pi f10993bc has them;
 * where amiga-pi differs the text says so: the shell) ---- */

static const char d_read[] =
    "Read the contents of a file. Supports text files and images (jpg, png, gif, webp, bmp). Images are sent "
    "as attachments. For text files, output is truncated to 2000 lines or 50KB (whichever is hit first). Use "
    "offset/limit for large files. When you need the full file, continue with offset until complete.";
static const char s_read[] =
    "{\"type\":\"object\",\"required\":[\"path\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the file to read (relative or absolute)\"}"
    ",\"offset\":{\"type\":\"number\",\"description\":\"Line number to start reading from (1-indexed)\"},\"limit\":{\"type\":"
    "\"number\",\"description\":\"Maximum number of lines to read\"}}}";
static const char *const g_read[] = { "Use read to examine files instead of cat or sed.", 0 };

static const char d_write[] =
    "Write content to a file. Creates the file if it doesn't exist, overwrites if it does. Automatically "
    "creates parent directories.";
static const char s_write[] =
    "{\"type\":\"object\",\"required\":[\"path\",\"content\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the file to write (relative or absolute)\"}"
    ",\"content\":{\"type\":\"string\",\"description\":\"Content to write to the file\"}}}";
static const char *const g_write[] = { "Use write only for new files or complete rewrites.", 0 };

static const char d_edit[] =
    "Edit a single file using exact text replacement. Every edits[].oldText must match a unique, "
    "non-overlapping region of the original file. If two changes affect the same block or nearby lines, merge "
    "them into one edit instead of emitting overlapping edits. Do not include large unchanged regions just to "
    "connect distant changes.";
/* in parts (C90 caps a string literal at 509 bytes), joined by pi_tools_init */
static const char *const s_edit[] = {
    "{\"type\":\"object\",\"required\":[\"path\",\"edits\"],\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Path to the file to edit (relative or absolute)\"}"
    ",\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"required\":[\"oldText\",\"newText\"],\"properties\":{\"oldText\":"
    "{\"type\":\"string\",\"description\":\"Exact text for one targeted replacement. It must be unique in the original file and must not overlap with any other edits[",
    "].oldText in the same call.\"},\"newText\":{\"type\":\"string\",\"description\":\"Replacement text for this targeted edit.\"}"
    "}},\"description\":\"One or more targeted replacements. Each edit is matched against the original file,"
    " not incrementally. Do not include overlapping or nested edits. If two changes touch the same block or nearby lines,"
    " merge them into one edit instead.\"}}}",
    0
};
static const char *const g_edit[] = {
    "Use edit for precise changes (edits[].oldText must match exactly)",
    "When changing multiple separate locations in one file, use one edit call with multiple entries in edits[] "
    "instead of multiple edit calls",
    "Each edits[].oldText is matched against the original file, not after earlier edits are applied. Do not "
    "emit overlapping or nested edits. Merge nearby changes into one edit.",
    "Keep edits[].oldText as small as possible while still being unique in the file. Do not pad with large "
    "unchanged regions.",
    0
};

static const char s_bash[] =
    "{\"type\":\"object\",\"required\":[\"command\"],\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"}"
    ",\"timeout\":{\"type\":\"number\",\"description\":\"Timeout in seconds (optional, no default timeout)\"}}}";

static const char d_grep[] =
    "Search file contents for a pattern. Returns matching lines with file paths and line numbers. Respects "
    ".gitignore. Output is truncated to 100 matches or 50KB (whichever is hit first). Long lines are truncated "
    "to 500 chars.";
static const char *const s_grep[] = {
    "{\"type\":\"object\",\"required\":[\"pattern\"],\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Search pattern (regex or literal string)\"}"
    ",\"path\":{\"type\":\"string\",\"description\":\"Directory or file to search (default: current directory)\"},\"glob\":"
    "{\"type\":\"string\",\"description\":\"Filter files by glob pattern, e.g. '*.ts' or '**/*.spec.ts'\"},\"ignoreCase\":"
    "{\"type\":\"boolean\",\"description\":\"Case-insensitive search (default: false)\"},\"literal\":{\"type\":\"boolean\",",
    "\"description\":\"Treat pattern as literal string instead of regex (default: false)\"},\"context\":{\"type\":"
    "\"number\",\"description\":\"Number of lines to show before and after each match (default: 0)\"},\"limit\":{"
    "\"type\":\"number\",\"description\":\"Maximum number of matches to return (default: 100)\"}}}",
    0
};

static const char d_find[] =
    "Search for files by glob pattern. Returns matching file paths relative to the search directory. Respects "
    ".gitignore. Output is truncated to 1000 results or 50KB (whichever is hit first).";
static const char s_find[] =
    "{\"type\":\"object\",\"required\":[\"pattern\"],\"properties\":{\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern to match files,"
    " e.g. '*.ts', '**/*.json', or 'src/**/*.spec.ts'\"},\"path\":{\"type\":\"string\",\"description\":\"Directory to search in (default:"
    " current directory)\"},\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of results (default: 1000)\"}"
    "}}";

static const char d_ls[] =
    "List directory contents. Returns entries sorted alphabetically, with '/' suffix for directories. Includes "
    "dotfiles. Output is truncated to 500 entries or 50KB (whichever is hit first).";
static const char s_ls[] =
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Directory to list (default: current directory)\"}"
    ",\"limit\":{\"type\":\"number\",\"description\":\"Maximum number of entries to return (default: 500)\"}}}";

long pi_tool_runs[PT_COUNT];

static const char *const names[PT_COUNT] = { "read", "write", "edit", "bash", "grep", "find", "ls" };

int pi_tools_kind(const char *name)
{
    int i;
    for (i = 0; i < PT_COUNT; i++)
        if (!strcmp(names[i], name))
            return i;
    return -1;
}

const char *pi_tools_shell_note(const pi_tools *p)
{
    switch (p->shell) {
    case PI_SH_VSH:
        return "The bash tool runs your commands in vsh, a bash-compatible shell for AmigaOS: Unix-style "
               "command lines (pipes, redirection, $(...), functions) with AmigaDOS commands and programs.";
    case PI_SH_AMIGASHELL:
        return "vsh is not installed: the bash tool runs your commands in the AmigaShell. Write AmigaDOS "
               "command lines (List, Type, Copy, Delete, Search), not Unix ones.";
    default:
        return "The bash tool runs your commands with /bin/sh.";
    }
}

/* ---- pi's call as C:Claude's input ---- */

/* in's string key copied as key2 (when present) */
static void put_str(jw *w, jv a, const char *key, const char *key2, int *first)
{
    jv x;
    char *s;
    long n = 0;
    if (!json_get(a, key, &x) || json_type(x) != J_STR || !(s = json_strdup(x, &n)))
        return;
    jw_rawz(w, *first ? "" : ",");
    *first = 0;
    jw_strz(w, key2);
    jw_rawz(w, ":");
    jw_str(w, s, n);
    free(s);
}

static void put_num(jw *w, const char *key, long v, int *first)
{
    jw_rawz(w, *first ? "" : ",");
    *first = 0;
    jw_strz(w, key);
    jw_rawz(w, ":");
    jw_long(w, v);
}

static long num_of(jv a, const char *key, long def)
{
    jv x;
    return json_get(a, key, &x) && json_type(x) == J_NUM ? json_long(x, def) : def;
}

static int is_true(jv a, const char *key)
{
    jv x;
    return json_get(a, key, &x) && json_type(x) == J_TRUE;
}

/* grep's literal: the pattern with the regular expression's specials escaped */
static void put_literal(jw *w, jv a)
{
    jv x;
    char *s;
    long n = 0, i;
    jw e;
    if (!json_get(a, "pattern", &x) || !(s = json_strdup(x, &n)))
        return;
    jw_init(&e);
    for (i = 0; i < n; i++) {
        if (strchr("\\.^$|?*+()[]{}", s[i]) && s[i])
            jw_raw(&e, "\\", 1);
        jw_raw(&e, s + i, 1);
    }
    jw_strz(w, "pattern");
    jw_rawz(w, ":");
    jw_str(w, e.p ? e.p : "", e.n);
    jw_free(&e);
    free(s);
}

int pi_tools_claude(int kind, const char *args, long an, int *tool, jw *w)
{
    jv a;
    int first = 1;
    if (json_parse(args, an, &a) || json_type(a) != J_OBJ)
        return -1;
    jw_rawz(w, "{");
    switch (kind) {
    case PT_READ:
        *tool = T_READ;
        put_str(w, a, "path", "file_path", &first);
        if (num_of(a, "offset", 0) > 0)
            put_num(w, "offset", num_of(a, "offset", 0), &first);
        if (num_of(a, "limit", 0) > 0)
            put_num(w, "limit", num_of(a, "limit", 0), &first);
        break;
    case PT_WRITE:
        *tool = T_WRITE;
        put_str(w, a, "path", "file_path", &first);
        put_str(w, a, "content", "content", &first);
        break;
    case PT_EDIT:
        *tool = T_EDIT;
        put_str(w, a, "path", "file_path", &first);
        break;
    case PT_BASH:
        *tool = T_BASH;
        put_str(w, a, "command", "command", &first);
        if (num_of(a, "timeout", 0) > 0)
            put_num(w, "timeout", num_of(a, "timeout", 0) * 1000L, &first);
        break;
    case PT_GREP:
        *tool = T_GREP;
        if (is_true(a, "literal")) {
            put_literal(w, a);
            first = 0;
        } else
            put_str(w, a, "pattern", "pattern", &first);
        put_str(w, a, "path", "path", &first);
        put_str(w, a, "glob", "glob", &first);
        jw_rawz(w, first ? "" : ",");
        first = 0;
        jw_rawz(w, "\"output_mode\":\"content\",\"-n\":true");
        if (is_true(a, "ignoreCase"))
            jw_rawz(w, ",\"-i\":true");
        if (num_of(a, "context", 0) > 0)
            put_num(w, "-C", num_of(a, "context", 0), &first);
        put_num(w, "head_limit", num_of(a, "limit", 100) > 0 ? num_of(a, "limit", 100) : 100, &first);
        break;
    case PT_FIND:
        *tool = T_GLOB;
        put_str(w, a, "pattern", "pattern", &first);
        put_str(w, a, "path", "path", &first);
        break;
    case PT_LS:
        *tool = T_GLOB;
        jw_rawz(w, "\"pattern\":\"*\"");
        first = 0;
        put_str(w, a, "path", "path", &first);
        break;
    default:
        return -1;
    }
    jw_rawz(w, "}");
    return w->oom ? -1 : 0;
}

/* ---- ls: the one tool C:Claude has none of ---- */

typedef struct ls_list {
    char **e;
    long n, cap;
    int oom;
} ls_list;

static int ls_one(void *c, const cl_dirent *d)
{
    ls_list *l = (ls_list *)c;
    long k = (long)strlen(d->name);
    char *s;
    if (l->n == l->cap) {
        long nc = l->cap ? l->cap * 2 : 64;
        char **ne = (char **)realloc(l->e, (size_t)nc * sizeof(char *));
        if (!ne) {
            l->oom = 1;
            return 1;
        }
        l->e = ne;
        l->cap = nc;
    }
    s = (char *)malloc((size_t)k + 2);
    if (!s) {
        l->oom = 1;
        return 1;
    }
    memcpy(s, d->name, (size_t)k);
    s[k] = d->dir ? '/' : 0;
    s[k + 1] = 0;
    l->e[l->n++] = s;
    return 0;
}

/* pi sorts with a.toLowerCase().localeCompare(b.toLowerCase()): ICU's
 * order (English), base letters first, accents to break a tie
 * (collate_tab.h); a name that is not UTF-8 is Latin-1, as AmigaDOS's */
#include "collate_tab.h"

/* the next weights of s at *i: up to two (an expansion), 0 at the end */
static int co_next(const unsigned char *s, long *i, unsigned long *w, int sec)
{
    for (;;) {
        unsigned long c;
        int k;
        if (!s[*i])
            return 0;
        k = s[*i] < 0x80 ? 1 : json_utf8((const char *)s + *i, (long)strlen((const char *)s + *i), &c);
        if (s[*i] < 0x80)
            c = s[*i];
        else if (k <= 0) {
            c = s[*i];          /* Latin-1 */
            k = 1;
        }
        *i += k;
        if (c >= 0x100) {
            w[0] = 0x1000 + c;  /* past the Latin-1 letters, in code point order */
            return 1;
        }
        if (!sec && (c == 0xE6 || c == 0xC6)) {
            w[0] = co_pri['a'];
            w[1] = co_pri['e'];
            return 2;
        }
        if (!sec && c == 0xDF) {
            w[0] = w[1] = co_pri['s'];
            return 2;
        }
        if (!co_pri[c])
            continue;           /* ignored (the soft hyphen, controls) */
        w[0] = sec ? co_sec[c] : co_pri[c];
        return 1;
    }
}

static int co_level(const unsigned char *a, const unsigned char *b, int sec)
{
    long i = 0, j = 0;
    unsigned long wa[2], wb[2];
    int na = 0, nb = 0, ka = 0, kb = 0;
    for (;;) {
        if (ka == na) {
            na = co_next(a, &i, wa, sec);
            ka = 0;
        }
        if (kb == nb) {
            nb = co_next(b, &j, wb, sec);
            kb = 0;
        }
        if (!na || !nb)
            return na ? 1 : nb ? -1 : 0;
        if (wa[ka] != wb[kb])
            return wa[ka] < wb[kb] ? -1 : 1;
        ka++;
        kb++;
    }
}

static int ls_cmp(const void *x, const void *y);

int pi_locale_compare(const char *a, const char *b)
{
    return ls_cmp(&a, &b);
}

static int ls_cmp(const void *x, const void *y)
{
    const unsigned char *a = *(const unsigned char *const *)x, *b = *(const unsigned char *const *)y;
    int c = co_level(a, b, 0);
    if (!c)
        c = co_level(a, b, 1);
    for (; !c && *a && *b; a++, b++) {
        int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        c = ca - cb;
    }
    return c;
}

static int run_ls(pi_tools *p, jv a, jw *text)
{
    char *arg;
    char full[512], num[16], note[80];
    long limit = num_of(a, "limit", 500), i;
    int outside = 0, kind, err = 0;
    ls_list l;
    jv x;
    memset(&l, 0, sizeof(l));
    arg = json_get(a, "path", &x) && json_type(x) == J_STR ? json_strdup(x, 0) : 0;
    if (tl_resolve(&p->t, arg && *arg ? arg : ".", full, sizeof(full), &outside)) {
        jw_rawz(text, "Path not found: ");
        jw_rawz(text, arg ? arg : ".");
        free(arg);
        return 1;
    }
    free(arg);
    kind = p->t.sys->kind(p->t.sys->u, full);
    if (kind != 2) {
        jw_rawz(text, kind ? "Not a directory: " : "Path not found: ");
        jw_rawz(text, full);
        return 1;
    }
    if (p->t.sys->list(p->t.sys->u, full, ls_one, &l) && !l.n) {
        jw_rawz(text, "Cannot read directory: ");
        jw_rawz(text, p->t.sys->err(p->t.sys->u));
        err = 1;
    } else if (l.oom) {
        jw_rawz(text, "out of memory");
        err = 1;
    } else if (!l.n || limit <= 0)
        jw_rawz(text, "(empty directory)");
    else {
        jw rows;
        jw_init(&rows);
        qsort(l.e, (size_t)l.n, sizeof(char *), ls_cmp);
        for (i = 0; i < l.n && i < limit; i++) {
            if (i)
                jw_raw(&rows, "\n", 1);
            jw_rawz(&rows, l.e[i]);
        }
        note[0] = 0;
        if (l.n > limit) {
            cl_ltoa(limit, num);
            cl_copy(note, num, sizeof(note));
            cl_cat(note, " entries limit reached. Use limit=", sizeof(note));
            cl_ltoa(limit * 2, num);
            cl_cat(note, num, sizeof(note));
            cl_cat(note, " for more", sizeof(note));
        }
        pi_head(rows.p ? rows.p : "", rows.n, note[0] ? note : 0, 0, text);
        jw_free(&rows);
    }
    for (i = 0; i < l.n; i++)
        free(l.e[i]);
    free(l.e);
    return err;
}

/* ---- read as pi's (read.ts) ---- */

#define PI_READ_MAX (16L * 1024 * 1024)     /* the most read takes into memory */

/* is s[0..n) UTF-8 throughout? */
static int utf8_ok(const char *s, long n)
{
    long i = 0;
    unsigned long c;
    while (i < n) {
        int k = (unsigned char)s[i] < 0x80 ? 1 : json_utf8(s + i, n - i, &c);
        if (k <= 0)
            return 0;
        i += k;
    }
    return 1;
}

static void put_long(jw *w, long v)
{
    char num[16];
    cl_ltoa(v, num);
    jw_rawz(w, num);
}

static int run_read(pi_tools *p, jv a, jw *text, jw *imgs, int *nimg)
{
    char *arg, *b = 0, full[512], what[300], sz[32];
    const char *mime;
    long n = 0, total, start, end, i, line, from, to, offset = num_of(a, "offset", 0);
    int outside = 0, kind, rc, limited = 0;
    jv x;
    arg = json_get(a, "path", &x) && json_type(x) == J_STR ? json_strdup(x, 0) : 0;
    if (!arg || tl_resolve(&p->t, arg, full, sizeof(full), &outside)) {
        jw_rawz(text, "ENOENT: no such file or directory, access '");
        jw_rawz(text, arg ? arg : "");
        jw_rawz(text, "'");
        free(arg);
        return 1;
    }
    tl_summary(what, sizeof(what), full, 0);
    if (p->t.show)
        p->t.show(p->t.u, tools_name(T_READ), what);
    kind = p->t.sys->kind(p->t.sys->u, full);
    if (kind != 1) {
        jw_rawz(text, kind == 2 ? "EISDIR: illegal operation on a directory, read"
                                : "ENOENT: no such file or directory, access '");
        if (kind != 2) {
            jw_rawz(text, full);
            jw_rawz(text, "'");
        }
        free(arg);
        return 1;
    }
    rc = p->t.sys->read(p->t.sys->u, full, PI_READ_MAX, &b, &n);
    if (rc) {
        jw_rawz(text, rc == SYS_TOO_BIG ? "the file is larger than 16 MB, the most read takes on the Amiga: use offset "
                                          "with bash (sed -n '1,2000p' file), or grep"
                                        : "cannot read the file: ");
        if (rc != SYS_TOO_BIG)
            jw_rawz(text, p->t.sys->err(p->t.sys->u));
        free(arg);
        return 1;
    }
    mime = pi_image_type((const unsigned char *)b, n);
    if (mime) {
        pi_img im;
        int omitted = pi_image_process((const unsigned char *)b, n, mime, &im);
        jw_rawz(text, "Read image file [");
        jw_rawz(text, omitted ? mime : im.mime);
        jw_rawz(text, "]");
        if (im.note[0]) {
            jw_rawz(text, "\n");
            jw_rawz(text, im.note);
        }
        if (!omitted) {
            jw d;
            jw_init(&d);
            tl_base64(&d, im.data, im.n);
            jw_rawz(imgs, ",");
            llm_b_image(imgs, d.p ? d.p : "", im.mime);
            (*nimg)++;
            jw_free(&d);
        }
        pi_image_free(&im);
        free(b);
        free(arg);
        return 0;
    }
    if (!utf8_ok(b, n)) {
        /* not UTF-8: Latin-1, AmigaDOS's text (pi would decode it as UTF-8) */
        jw u;
        jw_init(&u);
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)b[i];
            char e[2];
            if (c < 0x80)
                jw_raw(&u, b + i, 1);
            else {
                e[0] = (char)(0xc0 | (c >> 6));
                e[1] = (char)(0x80 | (c & 0x3f));
                jw_raw(&u, e, 2);
            }
        }
        free(b);
        b = u.p;
        n = u.n;
        if (!b) {
            b = (char *)malloc(1);
            n = 0;
        }
    }
    /* content.split("\n") */
    for (total = 1, i = 0; i < n; i++)
        total += b[i] == '\n';
    start = offset > 0 ? offset - 1 : 0;
    if (start >= total) {
        jw_rawz(text, "Offset ");
        put_long(text, offset);
        jw_rawz(text, " is beyond end of file (");
        put_long(text, total);
        jw_rawz(text, " lines total)");
        free(b);
        free(arg);
        return 1;
    }
    end = total;
    if (json_get(a, "limit", &x) && json_type(x) == J_NUM) {
        long lim = json_long(x, 0);
        limited = 1;
        end = start + lim < total ? start + lim : total;
    }
    /* the bytes of lines [start, end) joined by '\n' */
    for (line = 0, from = 0; line < start; from++)
        line += b[from] == '\n';
    to = from;
    for (line = start; line < end; line++) {
        while (to < n && b[to] != '\n')
            to++;
        if (line + 1 < end && to < n)
            to++;
    }
    if (end <= start)
        to = from;
    {
        pi_trunc tr;
        pi_truncate(b + from, to - from, 0, PI_MAX_LINES, PI_MAX_BYTES, &tr);
        if (tr.first_over) {
            long le = from;
            while (le < n && b[le] != '\n')
                le++;
            jw_rawz(text, "[Line ");
            put_long(text, start + 1);
            jw_rawz(text, " is ");
            pi_size(le - from, sz, sizeof(sz));
            jw_rawz(text, sz);
            jw_rawz(text, ", exceeds ");
            pi_size(PI_MAX_BYTES, sz, sizeof(sz));
            jw_rawz(text, sz);
            jw_rawz(text, " limit. Use bash: sed -n '");
            put_long(text, start + 1);
            jw_rawz(text, "p' ");
            jw_rawz(text, arg);
            jw_rawz(text, " | head -c ");
            put_long(text, PI_MAX_BYTES);
            jw_rawz(text, "]");
        } else if (tr.by != PT_NONE) {
            jw_raw(text, b + from + tr.off, tr.n);
            jw_rawz(text, "\n\n[Showing lines ");
            put_long(text, start + 1);
            jw_rawz(text, "-");
            put_long(text, start + tr.out_lines);
            jw_rawz(text, " of ");
            put_long(text, total);
            if (tr.by == PT_BYTES) {
                pi_size(PI_MAX_BYTES, sz, sizeof(sz));
                jw_rawz(text, " (");
                jw_rawz(text, sz);
                jw_rawz(text, " limit)");
            }
            jw_rawz(text, ". Use offset=");
            put_long(text, start + tr.out_lines + 1);
            jw_rawz(text, " to continue.]");
        } else {
            jw_raw(text, b + from, to - from);
            if (limited && end < total) {
                jw_rawz(text, "\n\n[");
                put_long(text, total - end);
                jw_rawz(text, " more lines in file. Use offset=");
                put_long(text, end + 1);
                jw_rawz(text, " to continue.]");
            }
        }
    }
    free(b);
    free(arg);
    return 0;
}

/* grep and find (claude/search_pi.c) with pi's endings */
static int run_search(pi_tools *p, int kind, const char *args, long an, jw *text)
{
    pi_rows r;
    char first[120], num[16];
    int e = kind == PT_GREP ? tools_pi_grep(&p->t, args, an, &r) : tools_pi_find(&p->t, args, an, &r);
    if (e)
        jw_raw(text, r.text.p ? r.text.p : "", r.text.n);
    else if (!r.rows)
        jw_rawz(text, kind == PT_GREP ? "No matches found" : "No files found matching pattern");
    else {
        first[0] = 0;
        if (r.limit_hit) {
            cl_ltoa(r.limit, num);
            cl_copy(first, num, sizeof(first));
            cl_cat(first, kind == PT_GREP ? " matches limit reached. Use limit=" : " results limit reached. Use limit=",
                   sizeof(first));
            cl_ltoa(r.limit * 2, num);
            cl_cat(first, num, sizeof(first));
            cl_cat(first, " for more, or refine pattern", sizeof(first));
        }
        pi_head(r.text.p ? r.text.p : "", r.text.n, first[0] ? first : 0,
                r.lines_cut ? "Some lines truncated to 500 chars. Use read tool to see full lines" : 0, text);
    }
    jw_free(&r.text);
    return e;
}

/* ---- C:Claude's tool_result as pi content ---- */

/* the result block's text into text, its images as pi image blocks into imgs: is_error */
static int from_result(const char *block, long n, jw *text, jw *imgs, int *nimg)
{
    jv b, c, x, el, src, d, mt;
    jit it;
    if (json_parse(block, n, &b) || !json_get(b, "content", &c)) {
        jw_rawz(text, "the tool gave no result");
        return 1;
    }
    if (json_type(c) == J_STR) {
        char *s;
        long sn = 0;
        if ((s = json_strdup(c, &sn)) != 0) {
            jw_raw(text, s, sn);
            free(s);
        }
    } else if (json_type(c) == J_ARR) {
        json_iter(c, &it);
        while (json_next(&it, 0, &el)) {
            if (json_get(el, "type", &x) && json_streq(x, "text") && json_get(el, "text", &x)) {
                char *s;
                long sn = 0;
                if ((s = json_strdup(x, &sn)) != 0) {
                    jw_raw(text, s, sn);
                    free(s);
                }
            } else if (json_get(el, "type", &x) && json_streq(x, "image") && json_get(el, "source", &src) &&
                       json_get(src, "data", &d) && json_get(src, "media_type", &mt)) {
                char *ds = json_strdup(d, 0), *ms = json_strdup(mt, 0);
                if (ds && ms) {
                    jw_rawz(imgs, ",");
                    llm_b_image(imgs, ds, ms);
                    if (!text->n) {
                        jw_rawz(text, "Read image file [");
                        jw_rawz(text, ms);
                        jw_rawz(text, "]");
                    }
                    (*nimg)++;
                }
                free(ds);
                free(ms);
            } else if (json_get(el, "type", &x) && json_streq(x, "document"))
                jw_rawz(text, "This is a PDF document: read cannot show it here.");
        }
    }
    return json_get(b, "is_error", &x) && json_type(x) == J_TRUE;
}

static llm_ts clock_of(pi_session *s)
{
    llm_ts t;
    t.sec = 0;
    t.ms = 0;
    return s && s->now ? s->now(s->h.u) : t;
}

/* ---- structuredContent of a result (pi-agent-core AgentToolResult.structuredContent), what tool_execution_end carries ---- */

/* read (pi read.js toReadOutput): the text, or for an image {type, data, mimeType, note} */
static void structured_read(jw *st, const jw *text, const jw *imgs)
{
    jv arr, img, d, mt;
    jw wrap;
    int ok = 0;
    jw_reset(st);
    if (imgs->n > 1) {
        jw_init(&wrap);
        jw_rawz(&wrap, "[");
        jw_raw(&wrap, imgs->p + 1, imgs->n - 1);        /* imgs starts with the "," that joins it to the text block */
        jw_rawz(&wrap, "]");
        if (!wrap.oom && json_parse(wrap.p, wrap.n, &arr) == 0) {
            jit it;
            json_iter(arr, &it);
            if (json_next(&it, 0, &img) && json_get(img, "data", &d) && json_get(img, "mimeType", &mt)) {
                jw_rawz(st, "{\"type\":\"image\",\"data\":");
                jw_raw(st, d.p, d.n);
                jw_rawz(st, ",\"mimeType\":");
                jw_raw(st, mt.p, mt.n);
                jw_rawz(st, ",\"note\":");
                jw_str(st, text->p ? text->p : "", text->n);
                jw_rawz(st, "}");
                ok = 1;
            }
        }
        jw_free(&wrap);
    }
    if (!ok)
        jw_str(st, text->p ? text->p : "", text->n);
}

/* bash (pi bash.js BashToolOutput): the output without the status, truncated false (PI_CAPTURE is under pi's
 * 1 MiB structured limit), the command's exit code and its wall time in tenths of a second */
static void structured_bash(jw *st, const char *out, long n, long exit_code, long tenths)
{
    jw_reset(st);
    jw_rawz(st, "{\"output\":");
    if (n == 11 && !memcmp(out, "(no output)", 11))
        n = 0;                  /* the placeholder is in the model's text only */
    jw_str(st, out, n);
    jw_rawz(st, ",\"truncated\":false,\"exit_code\":");
    jw_long(st, exit_code);
    jw_rawz(st, ",\"wall_time_seconds\":");
    jw_long(st, tenths / 10);
    if (tenths % 10) {
        jw_rawz(st, ".");
        jw_long(st, tenths % 10);
    }
    jw_rawz(st, "}");
}

/* the exit code in a bash result's status ("Command exited with code N"); 0 without a status, -1 the command did not
 * finish (timed out, aborted: pi throws, the result has no structured content) */
static int bash_exit(const char *status, long sn, long *code)
{
    static const char ex[] = "Command exited with code ";
    long i = 0, v = 0;
    while (i < sn && status[i] == '\n')
        i++;
    if (i >= sn) {
        *code = 0;
        return 0;
    }
    if (sn - i <= (long)sizeof(ex) - 1 || memcmp(status + i, ex, sizeof(ex) - 1))
        return -1;
    i += (long)sizeof(ex) - 1;
    for (; i < sn && status[i] >= '0' && status[i] <= '9'; i++)
        v = v * 10 + (status[i] - '0');
    *code = v;
    return 0;
}

/* ---- the run ---- */

/* bash's onUpdate (bash.ts): tool_execution_update with the output so far -- the empty one when the command starts
 * (content [], no details), the output (details {}) once it is done; text 0 is the empty one */
static void bash_update(pi_session *s, const char *id, const char *args, long an, const char *text, long n)
{
    pi_event e;
    jw c;
    if (!s || !s->call_id)      /* the user's bash command (!, rpc bash) is no tool call */
        return;
    jw_init(&c);
    jw_rawz(&c, "[");
    if (text)
        llm_b_text(&c, text, n, 0);
    jw_rawz(&c, "]");
    pi_event_init(&e, PE_TOOL_UPDATE);
    e.tool_id = id;
    e.tool_name = "bash";
    e.args = s->call_args ? s->call_args : args;       /* toolCall.arguments: what the model sent, not what an extension rewrote */
    e.args_n = s->call_args ? s->call_args_n : an;
    e.result = c.p;
    e.result_n = c.n;
    e.structured = text ? "{}" : 0;
    e.structured_n = 2;
    pi_emit(&s->em, &e);
    jw_free(&c);
}

/* bash's output as it arrives (bash.ts handleData / scheduleOutputUpdate / emitOutputUpdate): every chunk is appended and
 * marks the update dirty; an update goes out when 100 ms (BASH_UPDATE_THROTTLE_MS, renderers/bash.ts:19) have passed since
 * the last one (the first at once: lastUpdateAt 0), else it waits for the next look (tools_chunk's n 0, bash.ts's timer)
 * or the end (finishOutput) */
int pi_update_due(int has_last, unsigned long last, unsigned long now)
{
    return !has_last || now - last >= PI_BASH_UPDATE_MS;
}

typedef struct bash_stream {
    pi_tools *p;
    pi_session *s;
    const char *id, *args;
    long an;
    jw acc;                     /* the output so far (its tail beyond PI_CAPTURE) */
    int dirty, has_last, got;
    unsigned long last;
} bash_stream;

static unsigned long ms_of(const pi_session *s)
{
    return s && s->h.ms ? s->h.ms(s->h.u) : 0;
}

static void bash_emit(bash_stream *b)
{
    jw cut;
    jw_init(&cut);
    pi_cut(b->p->t.sys, b->p->tmp, b->id, b->acc.p ? b->acc.p : "", b->acc.n, 1, &cut);
    bash_update(b->s, b->id, b->args, b->an, cut.p ? cut.p : "", cut.n);
    jw_free(&cut);
    b->dirty = 0;
    b->has_last = 1;
    b->last = ms_of(b->s);
}

static void bash_chunk(void *u, const char *data, long n)
{
    bash_stream *b = (bash_stream *)u;
    if (n > 0) {
        b->got = 1;
        jw_raw(&b->acc, data, n);
        if (b->acc.n > PI_CAPTURE && b->acc.p) {
            memmove(b->acc.p, b->acc.p + (b->acc.n - PI_CAPTURE), (size_t)PI_CAPTURE);
            b->acc.n = PI_CAPTURE;
        }
        b->dirty = 1;
    }
    if (b->dirty && pi_update_due(b->has_last, b->last, ms_of(b->s)))
        bash_emit(b);
}

static int run(void *u, pi_session *s, const char *args, long an, jw *content, int *is_error)
{
    pi_tslot *sl = (pi_tslot *)u;
    pi_tools *p = sl->p;
    int kind = sl->kind, tool = 0, stopped = 0, nimg = 0;
    const char *id = s && s->call_id ? s->call_id : "call";
    char err[300];
    jv a, sch;
    jw text, res, imgs, in;
    llm_ts t0 = { 0, 0 }, t1 = { 0, 0 };
    long sn0 = 0;
    int ran = 0;
    bash_stream bs;
    bs.p = 0;
    jw_init(&text);
    jw_init(&res);
    jw_init(&imgs);
    jw_init(&in);
    *is_error = 0;
    if (s)
        jw_reset(&s->structured);
    if (json_parse(args, an, &a) || json_type(a) != J_OBJ) {
        jw_rawz(&text, "Validation failed for tool \"");
        jw_rawz(&text, names[kind]);
        jw_rawz(&text, "\": the arguments are not a JSON object");
        *is_error = 1;
    } else if (kind != PT_EDIT && /* edit takes pi's legacy forms too (prepareEditArguments) */
               json_parse(p->tool[kind].parameters, (long)strlen(p->tool[kind].parameters), &sch) == 0 &&
               schema_check(sch, a, err, sizeof(err))) {
        jw_rawz(&text, "Validation failed for tool \"");
        jw_rawz(&text, names[kind]);
        jw_rawz(&text, "\":\n  - ");
        jw_rawz(&text, err);
        *is_error = 1;
    } else {
        pi_tool_runs[kind]++;
        p->t.pi_status_n = 0;
        ran = 1;
        if (kind == PT_BASH)
            bash_update(s, id, args, an, 0, 0);
        t0 = clock_of(s);
        if (kind == PT_LS)
            *is_error = run_ls(p, a, &text);
        else if (kind == PT_GREP || kind == PT_FIND)
            *is_error = run_search(p, kind, args, an, &text);
        else if (kind == PT_READ)
            *is_error = run_read(p, a, &text, &imgs, &nimg);
        else {
            if (kind == PT_EDIT)
                tools_edit_orig(&p->t, id, args, an, &res);
            else if (pi_tools_claude(kind, args, an, &tool, &in) == 0) {
                if (kind == PT_BASH && s && s->call_id) {
                    memset(&bs, 0, sizeof(bs));
                    bs.p = p;
                    bs.s = s;
                    bs.id = id;
                    bs.args = args;
                    bs.an = an;
                    jw_init(&bs.acc);
                    p->t.chunk = bash_chunk;
                    p->t.chunk_u = &bs;
                }
                tools_run(&p->t, id, tools_name(tool), 1, in.p, in.n, &res);
                p->t.chunk = 0;
            }
            *is_error = from_result(res.p ? res.p : "", res.n, &text, &imgs, &nimg);
        }
        t1 = clock_of(s);
        sn0 = p->t.pi_status_n > 0 && p->t.pi_status_n <= text.n ? p->t.pi_status_n : 0;
        if (p->t.stop) {
            /* Ctrl+C stopped the command: the prompt stops too */
            p->t.stop = 0;
            stopped = 1;
        }
    }
    if (s && ran && !*is_error && kind == PT_READ)
        structured_read(&s->structured, &text, &imgs);
    else if (s && ran && kind == PT_BASH) {
        long code;
        if (bash_exit(text.p ? text.p + text.n - sn0 : "", sn0, &code) == 0 && (sn0 > 0 || !*is_error))
            structured_bash(&s->structured, text.p ? text.p : "", text.n - sn0, code,
                            ((t1.sec - t0.sec) * 1000L + (t1.ms - t0.ms) + 50) / 100);
    }
    jw_rawz(content, "[");
    if (kind == PT_BASH) {
        /* pi: the output's tail (the whole of it kept in a file when cut),
         * then the status (its bytes at the end, tools.c pi_status_n) */
        jw cut;
        long sn = p->t.pi_status_n > 0 && p->t.pi_status_n <= text.n ? p->t.pi_status_n : 0;
        jw_init(&cut);
        pi_cut(p->t.sys, p->tmp, id, text.p ? text.p : "", text.n - sn, 1, &cut);
        if (sn)
            jw_raw(&cut, text.p + text.n - sn, sn);
        if (bs.p) {
            /* pi's finishOutput: the update held back goes out; a host without streamed output (no job
             * table) gives the whole output as one chunk */
            if (!bs.got && text.n - sn > 0)
                bash_chunk(&bs, text.p, text.n - sn);
            if (bs.dirty)
                bash_emit(&bs);
            jw_free(&bs.acc);
        }
        llm_b_text(content, cut.p ? cut.p : "", cut.n, 0);
        jw_free(&cut);
    } else
        llm_b_text(content, text.p ? text.p : "", text.n, 0);
    if (imgs.n)
        jw_raw(content, imgs.p, imgs.n);
    jw_rawz(content, "]");
    jw_free(&text);
    jw_free(&res);
    jw_free(&imgs);
    jw_free(&in);
    return stopped;
}

/* pi never asks (the gate is an extension): C:Claude's question answered yes */
static int yes(void *u, const char *tool, const char *what, int outside, char *note, long cap)
{
    (void)u;
    (void)tool;
    (void)what;
    (void)outside;
    (void)note;
    (void)cap;
    return ASK_ONCE;
}

int pi_tools_init(pi_tools *p, cl_sys *sys, const char *root, const char *tmp, int shell)
{
    static const char *const descs[PT_COUNT] = { d_read, d_write, d_edit, 0, d_grep, d_find, d_ls };
    static const char *const schemas[PT_COUNT] = { s_read, s_write, 0, s_bash, 0, s_find, s_ls };
    static const char *const snippets[PT_COUNT] = {
        "Read file contents", "Create or overwrite files",
        "Make precise file edits with exact text replacement, including multiple disjoint edits in one call",
        "Execute bash commands (ls, grep, find, etc.)", "Search file contents for patterns (respects .gitignore)",
        "Find files by glob pattern (respects .gitignore)", "List directory contents"
    };
    static const char *const *const guides[PT_COUNT] = { g_read, g_write, g_edit, 0, 0, 0, 0 };
    int i;
    memset(p, 0, sizeof(*p));
    p->t.sys = sys;
    if (sys->canon(sys->u, root, p->t.root, sizeof(p->t.root)))
        cl_copy(p->t.root, root, sizeof(p->t.root));
    p->t.ask = yes;
    p->t.pi_rules = 1;
    p->t.no_background = 1;     /* pi: a command stops at its time limit */
    p->t.no_cd_keep = 1;        /* pi: every command starts in the working directory */
    cl_copy(p->t.cwd, p->t.root, sizeof(p->t.cwd));     /* ... and runs there (tools.c puts a cd in front) */
    p->t.timeout_s = 86400;     /* pi: no default time limit */
    p->t.out_max = p->t.out_inline = PI_CAPTURE;
    p->t.web_search = 0;
    cl_copy(p->tmp, tmp ? tmp : "", sizeof(p->tmp));
    p->shell = shell;
    cl_copy(p->bash_desc, shell == PI_SH_AMIGASHELL ? "Execute an AmigaShell command line" : "Execute a bash command",
            sizeof(p->bash_desc));
    cl_cat(p->bash_desc, shell == PI_SH_VSH ? " (vsh, a bash-compatible shell for AmigaOS)" : "", sizeof(p->bash_desc));
    cl_cat(p->bash_desc, " in the current working directory. Returns stdout and stderr. Output is truncated to "
                         "last 2000 lines or 50KB (whichever is hit first). If truncated, full output is saved "
                         "to a temp file. Optionally provide a timeout in seconds.",
           sizeof(p->bash_desc));
    p->edit_schema[0] = p->grep_schema[0] = 0;
    for (i = 0; s_edit[i]; i++)
        cl_cat(p->edit_schema, s_edit[i], sizeof(p->edit_schema));
    for (i = 0; s_grep[i]; i++)
        cl_cat(p->grep_schema, s_grep[i], sizeof(p->grep_schema));
    for (i = 0; i < PT_COUNT; i++) {
        p->slot[i].p = p;
        p->slot[i].kind = i;
        p->tool[i].name = names[i];
        p->tool[i].description = i == PT_BASH ? p->bash_desc : descs[i];
        p->tool[i].parameters = i == PT_EDIT ? p->edit_schema : i == PT_GREP ? p->grep_schema : schemas[i];
        p->tool[i].snippet = snippets[i];
        p->tool[i].u = &p->slot[i];
        p->tool[i].run = run;
        p->tool[i].guide = guides[i];
        /* pi's read, bash, edit and write declare constrainedSampling json_schema "prefer" (pi's dist/core/tools: read, bash, edit, write); grep,
         * find and ls do not */
        if (i == PT_READ || i == PT_BASH || i == PT_EDIT || i == PT_WRITE)
            p->tool[i].constrained = "{\"type\":\"json_schema\",\"strict\":\"prefer\"}";
    }
    return tools_init(&p->t);
}

void pi_tools_free(pi_tools *p)
{
    tools_free(&p->t);
}

/* ---- the tool selection (pi 1.1.0 core/settings-manager.ts:214-270, core/sdk.ts:280-290) ---- */

/* the next entry of a comma-separated list, trimmed of blanks, empty entries skipped (cli/args.ts:152-155): 1 and the
 * entry in e, 0 at the end */
static int next_entry(const char **q, char *e, int cap)
{
    int k = 0;
    const char *t;
    for (;;) {
        while (**q == ',')
            (*q)++;
        if (!**q)
            return 0;
        k = 0;
        while (**q && **q != ',') {
            if (k < cap - 1)
                e[k++] = **q;
            (*q)++;
        }
        e[k] = 0;
        while (k > 0 && (e[k - 1] == ' ' || e[k - 1] == '\t'))
            e[--k] = 0;
        for (t = e; *t == ' ' || *t == '\t'; t++)
            ;
        if (t != e)
            memmove(e, t, strlen(t) + 1);
        if (e[0])
            return 1;
    }
}

static int is_modifier(const char *e)
{
    return e[0] == '+' || e[0] == '-';
}

const char *pi_tool_list_error(const char *list, char *buf, long cap)
{
    const char *q = list ? list : "";
    char e[64], pat[64];
    int n = 0, mods = 0;
    pat[0] = 0;
    while (next_entry(&q, e, sizeof(e))) {
        n++;
        if (is_modifier(e)) {
            mods++;
            if (strchr(e, '*') && !pat[0])
                cl_copy(pat, e, sizeof(pat));
        }
    }
    if (!mods)
        return 0;
    if (mods < n)
        return "tool names cannot be mixed with +name or -name entries";
    if (pat[0]) {
        cl_copy(buf, "+name and -name entries take exact tool names, not patterns: ", cap);
        cl_cat(buf, pat, cap);
        return buf;
    }
    return 0;
}

/* applyToolModifiers: the +name and -name entries of list, in order, on the mask. A name this build has no tool of
 * (pi's codemode, an extension's tool, a typo) selects nothing and is no error: pi 1.1.0 sdk.ts:289-294 filters the
 * names it has no tool for without a word, on every stream (checked against the 1.1.0 oracle, --tools +nosuch) */
static void apply_modifiers(unsigned *on, const char *list)
{
    const char *q = list ? list : "";
    char e[64];
    while (next_entry(&q, e, sizeof(e))) {
        int k;
        if (!is_modifier(e))
            continue;
        k = pi_tools_kind(e + 1);
        if (k < 0)
            continue;
        if (e[0] == '+')
            *on |= 1u << k;
        else
            *on &= ~(1u << k);
    }
}

#define PI_DEFAULT_TOOLS ((1u << PT_READ) | (1u << PT_BASH) | (1u << PT_EDIT) | (1u << PT_WRITE))

void pi_tools_defaults(pi_tools *p, const char *entries, int no_tools)
{
    p->defaults_set = entries != 0;
    cl_copy(p->defaults, entries ? entries : "", sizeof(p->defaults));
    p->no_tools = no_tools;
}

/* resolveDefaultTools (settings-manager.ts:267): plain names replace the defaults, then the modifiers; an empty list
 * is no tools */
static unsigned resolve_defaults(const pi_tools *p)
{
    const char *q = p->defaults;
    char e[64];
    unsigned plain = 0, on;
    int n = 0, np = 0;
    if (!p->defaults_set)
        return PI_DEFAULT_TOOLS;
    while (next_entry(&q, e, sizeof(e))) {
        n++;
        if (!is_modifier(e)) {
            np++;
            if (pi_tools_kind(e) >= 0)
                plain |= 1u << pi_tools_kind(e);
        }
    }
    on = np > 0 || n == 0 ? plain : PI_DEFAULT_TOOLS;
    apply_modifiers(&on, p->defaults);
    return on;
}

int pi_tools_add(pi_tools *p, pi_session *s, const char *list, char *err, long cap)
{
    unsigned on = p->no_tools ? 0 : resolve_defaults(p);
    int i;
    if (list && *list) {
        const char *bad = pi_tool_list_error(list, err, cap);
        char name[64];
        const char *q = list;
        int mods = 0;
        if (bad) {
            if (bad != err)
                cl_copy(err, bad, cap);
            return -1;
        }
        while (next_entry(&q, name, sizeof(name)))
            mods |= is_modifier(name);
        if (mods) {
            /* only +name/-name entries: they change the default selection (sdk.ts:284-286) */
            apply_modifiers(&on, list);
        } else {
            /* an allowlist of plain names, "*" for every tool */
            on = 0;
            q = list;
            while (next_entry(&q, name, sizeof(name))) {
                if (!strcmp(name, "*"))
                    on = (1u << PT_COUNT) - 1;
                else if (pi_tools_kind(name) >= 0)
                    on |= 1u << pi_tools_kind(name);
            }
        }
    }
    /* user bash runs the backend whatever the list (agent-session.ts:3841-3858 createLocalBashOperations) */
    s->user_bash = &p->tool[PT_BASH];
    /* pi's order: read, bash, edit, write, grep, find, ls */
    {
        static const int order[PT_COUNT] = { PT_READ, PT_BASH, PT_EDIT, PT_WRITE, PT_GREP, PT_FIND, PT_LS };
        for (i = 0; i < PT_COUNT; i++)
            if ((on & (1u << order[i])) && pi_tool_add(s, &p->tool[order[i]])) {
                cl_copy(err, "too many tools", cap);
                return -1;
            }
    }
    return 0;
}
