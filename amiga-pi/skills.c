/* see skills.h */
#include <stdlib.h>
#include <string.h>
#include "skills.h"
#include "tools_pi.h"
#include "claude/path.h"
#include "claude/util.h"
#include "claude/glob.h"
#include "claude/ignore.h"

#define SK_MAX_NAME 64          /* pi: MAX_NAME_LENGTH */
#define SK_MAX_DESC 1024        /* pi: MAX_DESCRIPTION_LENGTH */
#define SK_FILE_MAX (256L * 1024)
#define SK_DEPTH 16

long pi_skills_reads;

/* ---- small things ---- */

static char *dupn(const char *s, long n)
{
    char *p = (char *)malloc((size_t)n + 1);
    if (p) {
        memcpy(p, s, (size_t)n);
        p[n] = 0;
    }
    return p;
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* JavaScript string length (UTF-16 code units) of UTF-8 text */
static long js_len(const char *s)
{
    long n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) == 0x80)
            continue;           /* a continuation byte */
        n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* ---- the frontmatter (pi: utils/frontmatter.ts over the yaml package) ---- */

enum { FK_NONE, FK_STR, FK_TRUE, FK_OTHER };

typedef struct fmv {
    int kind;
    char *s;
} fmv;

typedef struct fm {
    fmv name, desc, nomodel;
} fm;

static void fmv_set(fmv *v, int kind, char *s)
{
    free(v->s);
    v->kind = kind;
    v->s = s;
}

static void fm_free(fm *f)
{
    free(f->name.s);
    free(f->desc.s);
    free(f->nomodel.s);
    memset(f, 0, sizeof(*f));
}

/* the scalar kinds of YAML 1.2's core schema that are not strings */
static int plain_kind(const char *s, long n)
{
    long i = 0;
    int digits = 0, dot = 0, exp = 0;
    if (!n || !strcmp(s, "~") || !strcmp(s, "null") || !strcmp(s, "Null") || !strcmp(s, "NULL"))
        return FK_OTHER;
    if (!strcmp(s, "true") || !strcmp(s, "True") || !strcmp(s, "TRUE"))
        return FK_TRUE;
    if (!strcmp(s, "false") || !strcmp(s, "False") || !strcmp(s, "FALSE"))
        return FK_OTHER;
    if (!strcmp(s, ".nan") || !strcmp(s, ".NaN") || !strcmp(s, ".NAN") || !strcmp(s, ".inf") || !strcmp(s, ".Inf") ||
        !strcmp(s, ".INF") || !strcmp(s, "+.inf") || !strcmp(s, "-.inf") || !strcmp(s, "+.Inf") || !strcmp(s, "-.Inf") ||
        !strcmp(s, "+.INF") || !strcmp(s, "-.INF"))
        return FK_OTHER;
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o')) {
        for (i = 2; i < n; i++) {
            char c = s[i];
            int ok = s[1] == 'x' ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                                 : (c >= '0' && c <= '7');
            if (!ok)
                return FK_STR;
        }
        return FK_OTHER;
    }
    if (s[i] == '-' || s[i] == '+')
        i++;
    for (; i < n; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')
            digits++;
        else if (c == '.' && !dot && !exp)
            dot = 1;
        else if ((c == 'e' || c == 'E') && digits && !exp && i + 1 < n) {
            exp = 1;
            if (s[i + 1] == '-' || s[i + 1] == '+')
                i++;
            if (i + 1 >= n)
                return FK_STR;
        } else
            return FK_STR;
    }
    return digits ? FK_OTHER : FK_STR;
}

/* one line of the YAML text */
typedef struct yl {
    const char *s;
    long n;
    int ind;                    /* leading spaces */
    int blank;                  /* nothing but spaces, or a comment */
} yl;

static int split_lines(const char *y, long n, yl **out, int *nl)
{
    yl *L;
    int cnt = 1, k = 0;
    long i = 0;
    for (i = 0; i < n; i++)
        if (y[i] == '\n')
            cnt++;
    L = (yl *)malloc(sizeof(yl) * (size_t)cnt);
    if (!L)
        return -1;
    i = 0;
    while (i <= n && k < cnt) {
        long e = i, a;
        while (e < n && y[e] != '\n')
            e++;
        L[k].s = y + i;
        L[k].n = e - i;
        for (a = 0; a < L[k].n && L[k].s[a] == ' '; a++)
            ;
        L[k].ind = (int)a;
        L[k].blank = a == L[k].n || L[k].s[a] == '#';
        k++;
        i = e + 1;
    }
    *out = L;
    *nl = k;
    return 0;
}

/* the position of the key's colon in a mapping line (":" followed by a space or the end, outside quotes), -1 none */
static long key_colon(const char *s, long n)
{
    long i = 0;
    if (n && (s[0] == '"' || s[0] == '\'')) {
        char q = s[0];
        for (i = 1; i < n && s[i] != q; i++)
            if (q == '"' && s[i] == '\\')
                i++;
        i++;
    }
    for (; i < n; i++)
        if (s[i] == ':' && (i + 1 == n || s[i + 1] == ' '))
            return i;
    return -1;
}

/* a double-quoted scalar's escape at s (after the backslash): appends the character, returns the bytes used */
static long dq_escape(const char *s, long n, jw *w)
{
    char c = s[0];
    switch (c) {
    case 'n': jw_raw(w, "\n", 1); return 1;
    case 't': jw_raw(w, "\t", 1); return 1;
    case 'r': jw_raw(w, "\r", 1); return 1;
    case '0': jw_raw(w, "\0", 1); return 1;
    case 'a': jw_raw(w, "\a", 1); return 1;
    case 'b': jw_raw(w, "\b", 1); return 1;
    case 'e': jw_raw(w, "\033", 1); return 1;
    case 'f': jw_raw(w, "\f", 1); return 1;
    case 'v': jw_raw(w, "\v", 1); return 1;
    case ' ': jw_raw(w, " ", 1); return 1;
    case '/': jw_raw(w, "/", 1); return 1;
    case '"': jw_raw(w, "\"", 1); return 1;
    case '\\': jw_raw(w, "\\", 1); return 1;
    case 'N': jw_raw(w, "\302\205", 2); return 1;
    case '_': jw_raw(w, "\302\240", 2); return 1;
    case 'x': case 'u': case 'U': {
        int digits = c == 'x' ? 2 : c == 'u' ? 4 : 8, i;
        unsigned long cp = 0;
        if (n < 1 + digits)
            break;
        for (i = 1; i <= digits; i++) {
            char h = s[i];
            int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
            if (v < 0)
                return 1;
            cp = cp * 16 + (unsigned long)v;
        }
        if (cp < 0x80) {
            char b = (char)cp;
            jw_raw(w, &b, 1);
        } else if (cp < 0x800) {
            char b[2];
            b[0] = (char)(0xC0 | (cp >> 6));
            b[1] = (char)(0x80 | (cp & 0x3F));
            jw_raw(w, b, 2);
        } else if (cp < 0x10000) {
            char b[3];
            b[0] = (char)(0xE0 | (cp >> 12));
            b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            b[2] = (char)(0x80 | (cp & 0x3F));
            jw_raw(w, b, 3);
        } else {
            char b[4];
            b[0] = (char)(0xF0 | (cp >> 18));
            b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            b[3] = (char)(0x80 | (cp & 0x3F));
            jw_raw(w, b, 4);
        }
        return 1 + digits;
    }
    default:
        break;
    }
    jw_raw(w, "\\", 1);
    return 0;
}

/* the value of a quoted scalar that starts at line li, column col, and runs over following lines until its closing quote
 * (line folding: a line break is a space, an empty line a newline). 1 closed (into w, *end the line it ended on), 0 not closed */
static int quoted_value(yl *L, int nl, int li, long col, jw *w, int *end)
{
    char q = L[li].s[col];
    int cur = li, pending = 0, first = 1;
    long i = col + 1;
    for (;;) {
        const char *s = L[cur].s;
        long n = L[cur].n;
        if (!first) {
            /* a continuation line: leading blanks go, an empty line is a newline */
            i = 0;
            while (i < n && (s[i] == ' ' || s[i] == '\t'))
                i++;
            if (i == n) {
                pending++;
                if (++cur >= nl)
                    return 0;
                continue;
            }
            if (pending) {
                int t;
                for (t = 0; t < pending; t++)
                    jw_raw(w, "\n", 1);
            } else
                jw_raw(w, " ", 1);
            pending = 0;
        }
        first = 0;
        for (; i < n; i++) {
            if (q == '"' && s[i] == '\\') {
                if (i + 1 >= n)
                    break;
                i += dq_escape(s + i + 1, n - i - 1, w);
            } else if (q == '\'' && s[i] == '\'' && i + 1 < n && s[i + 1] == '\'') {
                jw_raw(w, "'", 1);
                i++;
            } else if (s[i] == q) {
                *end = cur;
                return 1;
            } else
                jw_raw(w, s + i, 1);
        }
        /* the line ended inside the quote: the blanks before the break are not text */
        while (w->n && (w->p[w->n - 1] == ' ' || w->p[w->n - 1] == '\t'))
            w->n--;
        if (++cur >= nl)
            return 0;
    }
}

/* a block scalar (| or >) whose header hdr (hn bytes) is on line li, in a mapping at indentation map_ind: its text into w, *end
 * the last line it uses (trailing empty lines included) */
static void block_value(yl *L, int nl, int li, const char *hdr, long hn, int map_ind, jw *w, int *end)
{
    int folded = hdr[0] == '>', chomp = 0 /* clip */, indicator = 0, from, cur, last, k, bind = -1;
    int prev_more = 0, have = 0, pend = 0;
    long i;
    jw b;
    for (i = 1; i < hn; i++) {
        if (hdr[i] == '-')
            chomp = -1;
        else if (hdr[i] == '+')
            chomp = 1;
        else if (hdr[i] >= '1' && hdr[i] <= '9')
            indicator = hdr[i] - '0';
        else
            break;
    }
    from = cur = li + 1;
    /* the block: the lines indented more than the key, empty ones included */
    while (cur < nl && (L[cur].n == L[cur].ind || L[cur].ind > map_ind))
        cur++;
    *end = cur - 1;
    last = cur - 1;
    while (last >= from && L[last].n == L[last].ind)
        last--;
    for (k = from; k <= last; k++)
        if (L[k].n != L[k].ind) {
            bind = indicator ? map_ind + indicator : L[k].ind;
            break;
        }
    if (bind < 0) {
        if (chomp == 1)
            for (k = from; k <= *end; k++)
                jw_raw(w, "\n", 1);
        return;
    }
    jw_init(&b);
    for (k = from; k <= last; k++) {
        const char *t;
        long tn;
        int more;
        if (L[k].n <= bind) {
            pend++;                     /* an empty line */
            continue;
        }
        t = L[k].s + bind;
        tn = L[k].n - bind;
        more = t[0] == ' ' || t[0] == '\t';
        if (have) {
            if (!folded)
                jw_raw(&b, "\n", 1);
            else if (!pend && !more && !prev_more)
                jw_raw(&b, " ", 1);
            else if (!pend)
                jw_raw(&b, "\n", 1);
        }
        for (; pend > 0; pend--)
            jw_raw(&b, "\n", 1);
        jw_raw(&b, t, tn);
        have = 1;
        prev_more = more;
    }
    if (chomp == 0)
        jw_raw(&b, "\n", 1);
    else if (chomp == 1)
        for (k = last; k <= *end; k++)
            jw_raw(&b, "\n", 1);
    if (b.p)
        jw_raw(w, b.p, b.n);
    jw_free(&b);
}

static void cat_num(char *dst, long cap, long v)
{
    char r[24];
    int t = 0;
    long len = (long)strlen(dst);
    if (!v)
        r[t++] = '0';
    while (v > 0) {
        r[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t && len + 1 < cap)
        dst[len++] = r[--t];
    dst[len] = 0;
}

/* pi's YAML error text for position (line index, col 1-based) of the lines L: "<msg> at line N, column C:\n\n<line>\n<spaces>^\n"
 * (the yaml package's prettifyError: the line cut to 80 characters around the column; ASCII columns) */
static void yaml_error(char *err, long cap, const char *msg, yl *L, int line, long col)
{
    char text[400];
    long ci = col - 1, len = L[line].n, start = 0, k;
    int front = 0, back = 0;
    cl_copy(err, msg, cap);
    cl_cat(err, " at line ", cap);
    cat_num(err, cap, line + 1);
    cl_cat(err, ", column ", cap);
    cat_num(err, cap, col);
    cl_cat(err, ":\n\n", cap);
    if (ci >= 60 && len > 80) {
        start = ci - 39 < len - 79 ? ci - 39 : len - 79;
        front = 1;
        ci -= start - 1;
    }
    len -= start;
    if (len + front > 80) {
        len = 79 - front;
        back = 1;
    }
    if (len > 300)
        len = 300;
    text[0] = 0;
    if (front)
        cl_copy(text, "\342\200\246", sizeof(text));
    k = (long)strlen(text);
    memcpy(text + k, L[line].s + start, (size_t)len);
    text[k + len] = 0;
    if (back)
        cl_cat(text, "\342\200\246", sizeof(text));
    cl_cat(err, text, cap);
    cl_cat(err, "\n", cap);
    for (k = 0; k < ci && (long)strlen(err) + 3 < cap; k++)
        cl_cat(err, " ", cap);
    cl_cat(err, "^\n", cap);
}

/* the mapping at the top of the YAML text y (n bytes): name, description and disable-model-invocation. 0 ok, -1 out of memory,
 * -2 pi's parser would refuse the text (err says how). Only the top-level keys are read; what is below a key is skipped. */
static int fm_parse(const char *y, long n, fm *out, char *err, long cap)
{
    yl *L;
    int nl, i, first, mi, rc = 0;
    err[0] = 0;
    memset(out, 0, sizeof(*out));
    if (split_lines(y, n, &L, &nl))
        return -1;
    first = -1;
    for (i = 0; i < nl; i++)
        if (!L[i].blank) {
            first = i;
            break;
        }
    if (first < 0) {
        free(L);
        return 0;
    }
    mi = L[first].ind;
    if (key_colon(L[first].s + mi, L[first].n - mi) < 0 || (L[first].s[mi] == '-' && (L[first].n - mi == 1 || L[first].s[mi + 1] == ' '))) {
        free(L);                /* a list or a plain string as the document: no keys */
        return 0;
    }
    i = first;
    while (i < nl && rc == 0) {
        long kc, vs;
        char key[80];
        long kn;
        int next, end;
        fmv *target = 0;
        const char *s;
        long sn;
        if (L[i].blank || L[i].ind != mi) {
            i++;
            continue;
        }
        s = L[i].s + mi;
        sn = L[i].n - mi;
        kc = key_colon(s, sn);
        if (kc < 0) {
            i++;
            continue;
        }
        /* the key, quotes off */
        kn = kc;
        while (kn > 0 && s[kn - 1] == ' ')
            kn--;
        if (kn >= 2 && (s[0] == '"' || s[0] == '\'') && s[kn - 1] == s[0]) {
            memcpy(key, s + 1, (size_t)(kn - 2 < 79 ? kn - 2 : 79));
            key[kn - 2 < 79 ? kn - 2 : 79] = 0;
        } else {
            memcpy(key, s, (size_t)(kn < 79 ? kn : 79));
            key[kn < 79 ? kn : 79] = 0;
        }
        if (!strcmp(key, "name"))
            target = &out->name;
        else if (!strcmp(key, "description"))
            target = &out->desc;
        else if (!strcmp(key, "disable-model-invocation"))
            target = &out->nomodel;
        vs = kc + 1;
        while (vs < sn && s[vs] == ' ')
            vs++;
        /* the value's extent: following lines indented deeper (or empty/comment lines up to the next key) */
        next = i + 1;
        while (next < nl && (L[next].blank || L[next].ind > mi))
            next++;
        end = next - 1;
        while (end > i && L[end].blank)
            end--;
        if (vs >= sn || s[vs] == '#') {
            /* no value on the line: a nested block (not a string) or null */
            if (target)
                fmv_set(target, FK_OTHER, 0);
        } else if (s[vs] == '|' || s[vs] == '>') {
            if (target) {
                jw w;
                int e2;
                jw_init(&w);
                block_value(L, nl, i, s + vs, sn - vs, mi, &w, &e2);
                if (w.oom)
                    rc = -1;
                else
                    fmv_set(target, FK_STR, dupn(w.p ? w.p : "", w.n));
                jw_free(&w);
            }
        } else if (s[vs] == '"' || s[vs] == '\'') {
            jw w;
            int e2 = i;
            jw_init(&w);
            if (quoted_value(L, nl, i, mi + vs, &w, &e2)) {
                if (target) {
                    if (w.oom)
                        rc = -1;
                    else
                        fmv_set(target, FK_STR, dupn(w.p ? w.p : "", w.n));
                }
                jw_free(&w);
                next = e2 + 1;
                while (next < nl && (L[next].blank || L[next].ind > mi))
                    next++;
            } else {
                jw_free(&w);
                if (target)
                    fmv_set(target, FK_OTHER, 0);
            }
        } else if (s[vs] == '[' || s[vs] == '{' || s[vs] == '&' || s[vs] == '*' || s[vs] == '!') {
            if (target)
                fmv_set(target, FK_OTHER, 0);
        } else {
            /* a plain scalar over one or more lines: a comment ends it, a line break is a space, an empty line a newline */
            jw w;
            int k, nested = 0;
            jw_init(&w);
            for (k = i; k <= end; k++) {
                const char *t;
                long tn, c;
                if (k == i) {
                    t = s + vs;
                    tn = sn - vs;
                } else {
                    t = L[k].s + L[k].ind;
                    tn = L[k].n - L[k].ind;
                    if (L[k].blank && L[k].s[L[k].ind] != '#') {
                        jw_raw(&w, "\n", 1);
                        continue;
                    }
                    if (L[k].blank)
                        break;      /* a comment line ends a plain scalar */
                }
                /* a comment: " #" */
                for (c = 0; c < tn; c++)
                    if (t[c] == '#' && (c == 0 || t[c - 1] == ' ' || t[c - 1] == '\t')) {
                        tn = c;
                        break;
                    }
                while (tn > 0 && (t[tn - 1] == ' ' || t[tn - 1] == '\t'))
                    tn--;
                if (key_colon(t, tn) >= 0) {
                    /* ": " inside a plain scalar: yaml reads a mapping where none is allowed */
                    yaml_error(err, cap, "Nested mappings are not allowed in compact mappings", L, i, mi + vs + 1);
                    nested = 1;
                    break;
                }
                if (k > i && w.n && w.p[w.n - 1] != '\n')
                    jw_raw(&w, " ", 1);
                jw_raw(&w, t, tn);
            }
            if (nested)
                rc = -2;
            else if (target) {
                if (w.oom)
                    rc = -1;
                else {
                    char *txt = dupn(w.p ? w.p : "", w.n);
                    fmv_set(target, txt ? plain_kind(txt, w.n) : FK_OTHER, txt);
                    if (txt && target->kind != FK_STR) {
                        /* an untyped value: only true matters */
                        free(target->s);
                        target->s = 0;
                    }
                }
            }
            jw_free(&w);
        }
        i = next;
    }
    free(L);
    return rc;
}

/* the text with line breaks normalised (CR, CRLF to LF) and a BOM off, malloc'ed */
static char *normalise(const char *t, long n, long *outn)
{
    char *p = (char *)malloc((size_t)n + 1);
    long i = 0, k = 0;
    if (!p)
        return 0;
    if (n >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF)
        i = 3;
    for (; i < n; i++) {
        if (t[i] == '\r') {
            p[k++] = '\n';
            if (i + 1 < n && t[i + 1] == '\n')
                i++;
        } else
            p[k++] = t[i];
    }
    p[k] = 0;
    *outn = k;
    return p;
}

/* pi's extractFrontmatter on the normalised text t (n bytes): the YAML is t[*ys .. *ys + *yn) (yn 0: none, which also covers an
 * empty block), the body starts at *bs (0: the whole text) */
static void fm_extract(const char *t, long n, long *ys, long *yn, long *bs)
{
    long i;
    *ys = 4;
    *yn = 0;
    *bs = 0;
    if (n < 3 || strncmp(t, "---", 3))
        return;
    for (i = 3; i + 3 < n; i++)
        if (t[i] == '\n' && t[i + 1] == '-' && t[i + 2] == '-' && t[i + 3] == '-')
            break;
    if (i + 3 >= n)
        return;                 /* no closing line: no frontmatter */
    *yn = i > 4 ? i - 4 : 0;
    *bs = i + 4;
}

/* ---- the skill files ---- */

typedef struct fl_ent {
    char path[300];
    int source, scope;
    int bad;                    /* a --skill path: 1 does not exist, 2 is not a markdown file (reported in its turn) */
    char sbase[300];            /* the source's baseDir ("" none) */
} fl_ent;

typedef struct fl_list {
    fl_ent *e;
    int n, cap;
} fl_list;

static void fl_free(fl_list *l)
{
    free(l->e);
    memset(l, 0, sizeof(*l));
}

/* a file once (pi's Map keyed by path; the first one's source stays): 0, -1 */
static int fl_add_base(fl_list *l, const char *path, int source, int scope, int bad, const char *sbase)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].path, path))
            return 0;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        fl_ent *q = (fl_ent *)realloc(l->e, (size_t)nc * sizeof(fl_ent));
        if (!q)
            return -1;
        l->e = q;
        l->cap = nc;
    }
    cl_copy(l->e[l->n].path, path, sizeof(l->e[0].path));
    l->e[l->n].source = source;
    l->e[l->n].scope = scope;
    l->e[l->n].bad = bad;
    cl_copy(l->e[l->n].sbase, sbase, sizeof(l->e[0].sbase));
    l->n++;
    return 0;
}

static int fl_add(fl_list *l, const char *path, int source, int scope)
{
    return fl_add_base(l, path, source, scope, 0, "");
}

typedef struct sk_ctx {
    cl_sys *sys;
    pi_skills *k;
    int oom;
    int cur_source, cur_scope;  /* the source of the files being collected */
    const char *cur_base;       /* ... and its baseDir */
} sk_ctx;

static int add_diag(pi_skills *k, int type, const char *msg, const char *path, const char *winner, const char *name)
{
    pi_skill_diag *d;
    if (k->nd == k->capd) {
        int nc = k->capd ? k->capd * 2 : 16;
        pi_skill_diag *q = (pi_skill_diag *)realloc(k->d, (size_t)nc * sizeof(pi_skill_diag));
        if (!q)
            return -1;
        k->d = q;
        k->capd = nc;
    }
    d = &k->d[k->nd];
    memset(d, 0, sizeof(*d));
    d->type = type;
    d->msg = dupn(msg, (long)strlen(msg));
    d->name = name ? dupn(name, (long)strlen(name)) : 0;
    cl_copy(d->path, path ? path : "", sizeof(d->path));
    cl_copy(d->winner, winner ? winner : "", sizeof(d->winner));
    if (!d->msg || (name && !d->name))
        return -1;
    k->nd++;
    return 0;
}

static int ends_md(const char *name)
{
    long n = (long)strlen(name);
    return n >= 3 && !strcmp(name + n - 3, ".md");
}

/* the last component of path (a volume's root: the volume) */
static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    if (s)
        return s + 1;
    s = strrchr(path, ':');
    return s ? s + 1 : path;
}

/* ---- directory scans (pi: collectSkillEntries, loadSkillsFromDirInternal) with the ignore files of skills.ts ---- */

typedef struct scan_ent {
    cl_dirent e[200];
    int n;
} scan_ent;

static int scan_cb(void *c, const cl_dirent *e)
{
    scan_ent *s = (scan_ent *)c;
    if (s->n < 200)
        s->e[s->n++] = *e;
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const cl_dirent *)a)->name, ((const cl_dirent *)b)->name);
}

/* pi's prefixIgnorePattern for each line of an ignore file below the root, added to set */
static int add_ignore_rules(sk_ctx *c, ig_set *set, const char *dir, const char *rel)
{
    static const char *const names[] = { ".gitignore", ".ignore", ".fdignore", 0 };
    int i;
    for (i = 0; names[i]; i++) {
        char p[300], *b = 0;
        long n = 0, a = 0;
        jw out;
        if (path_join(dir, names[i], p, sizeof(p)) || c->sys->kind(c->sys->u, p) != 1)
            continue;
        if (c->sys->read(c->sys->u, p, SK_FILE_MAX, &b, &n))
            continue;
        jw_init(&out);
        while (a < n) {
            long e = a, ts, te;
            const char *line;
            long ln;
            int neg = 0;
            while (e < n && b[e] != '\n')
                e++;
            line = b + a;
            ln = e - a;
            if (ln && line[ln - 1] == '\r')
                ln--;
            a = e + 1;
            for (ts = 0; ts < ln && is_ws(line[ts]); ts++)
                ;
            for (te = ln; te > ts && is_ws(line[te - 1]); te--)
                ;
            if (ts == te)
                continue;
            if (line[ts] == '#')
                continue;
            if (ln && line[0] == '!') {
                neg = 1;
                line++;
                ln--;
            } else if (ln > 1 && line[0] == '\\' && line[1] == '!') {
                line++;
                ln--;
            }
            if (ln && line[0] == '/') {
                line++;
                ln--;
            }
            if (neg)
                jw_raw(&out, "!", 1);
            if (rel[0]) {
                jw_rawz(&out, rel);
                jw_raw(&out, "/", 1);
            }
            jw_raw(&out, line, ln);
            jw_raw(&out, "\n", 1);
        }
        CL_FREE(b);
        if (out.oom || (out.n && ig_parse(set, out.p, out.n))) {
            jw_free(&out);
            return -1;
        }
        jw_free(&out);
    }
    return 0;
}

static void ig_clear(ig_set *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        free(s->r[i].pat);
    free(s->r);
    memset(s, 0, sizeof(*s));
}

/* the skill files below dir: SKILL.md of a directory ends the search there; else .md files (md_root: those of the root
 * only -- "pi" style; else those of the sub-directories only -- "agents" style) and the sub-directories. rel is dir
 * relative to the root ("" at the root) */
static int scan_dir(sk_ctx *c, const char *dir, const char *rel, int agents, ig_set *ig, fl_list *out, int depth)
{
    scan_ent *se;
    int i, j, rc = 0;
    if (depth > SK_DEPTH)
        return 0;
    if (c->sys->kind(c->sys->u, dir) != 2)
        return 0;
    if (add_ignore_rules(c, ig, dir, rel))
        return -1;
    se = (scan_ent *)malloc(sizeof(scan_ent));
    if (!se)
        return -1;
    se->n = 0;
    if (c->sys->list(c->sys->u, dir, scan_cb, se)) {
        free(se);
        return 0;
    }
    qsort(se->e, (size_t)se->n, sizeof(cl_dirent), ent_cmp);
    for (i = 0; i < se->n; i++) {
        char full[300], relp[300];
        if (strcmp(se->e[i].name, "SKILL.md"))
            continue;
        if (path_join(dir, se->e[i].name, full, sizeof(full)))
            continue;
        if (c->sys->kind(c->sys->u, full) != 1)
            continue;
        cl_copy(relp, rel, sizeof(relp));
        if (rel[0])
            cl_cat(relp, "/", sizeof(relp));
        cl_cat(relp, se->e[i].name, sizeof(relp));
        if (ig_match(ig, relp, 0) == 1)
            continue;
        rc = fl_add_base(out, full, c->cur_source, c->cur_scope, 0, c->cur_base ? c->cur_base : "");
        free(se);
        return rc;
    }
    for (j = 0; j < se->n && rc == 0; j++) {
        const cl_dirent *e = &se->e[j];
        char full[300], relp[300];
        int kind;
        if (e->name[0] == '.' || !strcmp(e->name, "node_modules"))
            continue;
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        kind = c->sys->kind(c->sys->u, full);
        cl_copy(relp, rel, sizeof(relp));
        if (rel[0])
            cl_cat(relp, "/", sizeof(relp));
        cl_cat(relp, e->name, sizeof(relp));
        if (kind == 1 && ends_md(e->name) && ig_match(ig, relp, 0) != 1 && ((!agents && !rel[0]) || (agents && rel[0]))) {
            rc = fl_add_base(out, full, c->cur_source, c->cur_scope, 0, c->cur_base ? c->cur_base : "");
            continue;
        }
        if (kind != 2)
            continue;
        if (ig_match(ig, relp, 1) == 1)
            continue;
        rc = scan_dir(c, full, relp, agents, ig, out, depth + 1);
    }
    free(se);
    return rc;
}

/* collectSkillEntries(dir, mode) into out */
static int collect_dir(sk_ctx *c, const char *dir, int agents, fl_list *out)
{
    ig_set ig;
    int rc;
    memset(&ig, 0, sizeof(ig));
    rc = scan_dir(c, dir, "", agents, &ig, out, 0);
    ig_clear(&ig);
    return rc;
}

/* ---- settings.json "skills" (pi: resolveLocalEntries, applyPatterns, isEnabledByOverrides) ---- */

#define SK_PATS 24

typedef struct sk_set {
    char plain[SK_PATS][300];
    int nplain;
    char pat[SK_PATS][300];     /* patterns: "!x", "+x", "-x", or an include glob */
    int npat;
} sk_set;

/* settings.json: the "skills" entries into set (plain paths and patterns), enableSkillCommands into *commands (-1 absent) */
static void read_settings(cl_sys *sys, const char *file, sk_set *set, int *commands)
{
    char *b = 0;
    long n = 0;
    jv o, v, x;
    jit it;
    set->nplain = set->npat = 0;
    *commands = -1;
    if (!file[0] || sys->kind(sys->u, file) != 1 || sys->read(sys->u, file, SK_FILE_MAX, &b, &n))
        return;
    if (!json_parse(b, n, &o) && json_type(o) == J_OBJ) {
        if (json_get(o, "enableSkillCommands", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
            *commands = json_type(v) == J_TRUE;
        if (json_get(o, "skills", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &x))
                if (json_type(x) == J_STR) {
                    char e[300];
                    json_str(x, e, sizeof(e));
                    if (e[0] == '!' || e[0] == '+' || e[0] == '-' || strchr(e, '*') || strchr(e, '?')) {
                        if (set->npat < SK_PATS)
                            cl_copy(set->pat[set->npat++], e, 300);
                    } else if (set->nplain < SK_PATS)
                        cl_copy(set->plain[set->nplain++], e, 300);
                }
        }
    }
    CL_FREE(b);
}

/* path relative to base ("" when it is base): 1 inside, else 0 with rel the path itself */
static int rel_to(const char *base, const char *path, char *rel, long cap)
{
    long bn = (long)strlen(base);
    if (bn && !strncmp(path, base, (size_t)bn) &&
        (path[bn] == '/' || (bn && (base[bn - 1] == '/' || base[bn - 1] == ':')))) {
        const char *r = path + bn + (path[bn] == '/' ? 1 : 0);
        cl_copy(rel, r, cap);
        return 1;
    }
    cl_copy(rel, path, cap);
    return 0;
}

static int norm_exact(const char *pat, char *out, long cap)
{
    if (pat[0] == '.' && pat[1] == '/')
        pat += 2;
    cl_copy(out, pat, cap);
    return 0;
}

/* pi's matchesAnyPattern / matchesAnyExactPattern for one file */
static int matches(const char *file, const char *base, char (*pats)[300], const int *idx, int n, int exact)
{
    char rel[300], parent[300], prel[300];
    const char *name = base_name(file);
    int is_skill = !strcmp(name, "SKILL.md"), i;
    rel_to(base, file, rel, sizeof(rel));
    parent[0] = prel[0] = 0;
    if (is_skill) {
        path_parent(file, parent, sizeof(parent));
        rel_to(base, parent, prel, sizeof(prel));
    }
    for (i = 0; i < n; i++) {
        const char *p = pats[idx[i]] + (pats[idx[i]][0] == '!' || pats[idx[i]][0] == '+' || pats[idx[i]][0] == '-' ? 1 : 0);
        char np[300];
        if (exact) {
            norm_exact(p, np, sizeof(np));
            if (!strcmp(np, rel) || !strcmp(np, file))
                return 1;
            if (is_skill && (!strcmp(np, prel) || !strcmp(np, parent)))
                return 1;
        } else {
            if (glob_match(p, rel) || glob_match(p, name) || glob_match(p, file))
                return 1;
            if (is_skill && (glob_match(p, prel) || glob_match(p, base_name(parent)) || glob_match(p, parent)))
                return 1;
        }
    }
    return 0;
}

/* isEnabledByOverrides: !x excludes, +x forces in, -x forces out */
static int enabled_by_overrides(const char *file, const char *base, char (*pats)[300], int npat)
{
    int idx_ex[SK_PATS], idx_in[SK_PATS], idx_out[SK_PATS], nex = 0, nin = 0, nout = 0, i, en = 1;
    for (i = 0; i < npat; i++) {
        if (pats[i][0] == '!')
            idx_ex[nex++] = i;
        else if (pats[i][0] == '+')
            idx_in[nin++] = i;
        else if (pats[i][0] == '-')
            idx_out[nout++] = i;
    }
    if (nex && matches(file, base, pats, idx_ex, nex, 0))
        en = 0;
    if (nin && matches(file, base, pats, idx_in, nin, 1))
        en = 1;
    if (nout && matches(file, base, pats, idx_out, nout, 1))
        en = 0;
    return en;
}

/* applyPatterns over the files of one settings file's plain entries */
static int enabled_by_patterns(const char *file, const char *base, char (*pats)[300], int npat)
{
    int idx_inc[SK_PATS], idx_ex[SK_PATS], idx_in[SK_PATS], idx_out[SK_PATS], ninc = 0, nex = 0, nin = 0, nout = 0, i, en = 1;
    for (i = 0; i < npat; i++) {
        if (pats[i][0] == '+')
            idx_in[nin++] = i;
        else if (pats[i][0] == '-')
            idx_out[nout++] = i;
        else if (pats[i][0] == '!')
            idx_ex[nex++] = i;
        else
            idx_inc[ninc++] = i;
    }
    if (ninc && !matches(file, base, pats, idx_inc, ninc, 0))
        en = 0;
    if (nex && matches(file, base, pats, idx_ex, nex, 0))
        en = 0;
    if (nin && matches(file, base, pats, idx_in, nin, 1))
        en = 1;
    if (nout && matches(file, base, pats, idx_out, nout, 1))
        en = 0;
    return en;
}

/* an absolute AmigaDOS path or a Unix one */
static int is_abs(const char *p)
{
    return p[0] == '/' || strchr(p, ':') != 0;
}

/* the plain entries of a settings file collected (their files, each enabled by the patterns), source local */
static int settings_files(sk_ctx *c, sk_set *set, const char *base, int scope, fl_list *out)
{
    int i, rc = 0;
    for (i = 0; i < set->nplain && rc == 0; i++) {
        char p[300];
        fl_list found;
        int j, kind;
        memset(&found, 0, sizeof(found));
        if (is_abs(set->plain[i]))
            cl_copy(p, set->plain[i], sizeof(p));
        else if (path_join(base, set->plain[i], p, sizeof(p)))
            continue;
        kind = c->sys->kind(c->sys->u, p);
        c->cur_source = SKO_LOCAL;
        c->cur_scope = scope;
        if (kind == 1)
            rc = fl_add(&found, p, SKO_LOCAL, scope);
        else if (kind == 2)
            rc = collect_dir(c, p, 0, &found);
        for (j = 0; j < found.n && rc == 0; j++)
            if (enabled_by_patterns(found.e[j].path, base, set->pat, set->npat))
                rc = fl_add(out, found.e[j].path, SKO_LOCAL, scope);
        fl_free(&found);
    }
    return rc;
}

/* auto-discovered files of dir ("pi" or "agents" style) kept when the overrides allow them */
static int auto_files(sk_ctx *c, const char *dir, int agents, const char *base, sk_set *set, int scope, fl_list *out)
{
    fl_list found;
    int j, rc;
    memset(&found, 0, sizeof(found));
    c->cur_source = SKO_AUTO;
    c->cur_scope = scope;
    c->cur_base = base;
    rc = collect_dir(c, dir, agents, &found);
    c->cur_base = 0;
    for (j = 0; j < found.n && rc == 0; j++)
        if (enabled_by_overrides(found.e[j].path, base, set->pat, set->npat))
            rc = fl_add_base(out, found.e[j].path, SKO_AUTO, scope, 0, base);
    fl_free(&found);
    return rc;
}

/* pi's collectAncestorAgentsSkillDirs: <dir>/.agents/skills for cwd and each ancestor up to the one holding .git (all the way up
 * without one) */
static int ancestor_agents(sk_ctx *c, const char *cwd, fl_list *out, sk_set *set)
{
    char dirs[32][300], top[300], cur[300], up[300], g[300];
    int n = 0, i, rc = 0, repo = 0;
    /* the repository's top: the nearest directory holding .git, starting at cwd */
    cl_copy(cur, cwd, sizeof(cur));
    for (;;) {
        if (path_join(cur, ".git", g, sizeof(g)) == 0 && c->sys->kind(c->sys->u, g) > 0) {
            cl_copy(top, cur, sizeof(top));
            repo = 1;
            break;
        }
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            break;
        cl_copy(cur, up, sizeof(cur));
    }
    cl_copy(cur, cwd, sizeof(cur));
    for (;;) {
        if (n < 32 && path_join(cur, ".agents/skills", dirs[n], 300) == 0)
            n++;
        if (repo && !strcmp(cur, top))
            break;
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            break;
        cl_copy(cur, up, sizeof(cur));
    }
    for (i = 0; i < n && rc == 0; i++) {
        char agents_base[300];
        path_parent(dirs[i], agents_base, sizeof(agents_base));
        rc = auto_files(c, dirs[i], 1, agents_base, set, SKS_PROJECT, out);
    }
    return rc;
}

/* ---- loading one file (pi: loadSkillFromFile) ---- */

static int add_skill(pi_skills *k, pi_skill *s)
{
    if (k->n == k->cap) {
        int nc = k->cap ? k->cap * 2 : 16;
        pi_skill *q = (pi_skill *)realloc(k->s, (size_t)nc * sizeof(pi_skill));
        if (!q)
            return -1;
        k->s = q;
        k->cap = nc;
    }
    k->s[k->n++] = *s;
    return 0;
}

/* a file's validation messages and, when it is a skill, the skill: 0, -1 out of memory. *got 1 when s is filled in */
static int load_file(sk_ctx *c, const char *path, int source, int scope, const char *sbase, pi_skill *s, int *got)
{
    char *b = 0, *norm = 0, err[600], dir[300];
    long n = 0, nn = 0, ys, yn, bs;
    fm f;
    int declared = !strcmp(base_name(path), "SKILL.md"), frc, has_desc, rc = 0;
    const char *name;
    *got = 0;
    if (c->sys->read(c->sys->u, path, SK_FILE_MAX, &b, &n)) {
        const char *why = c->sys->err ? c->sys->err(c->sys->u) : 0;
        return add_diag(c->k, SKD_WARNING, why && why[0] ? why : "failed to read skill file", path, 0, 0);
    }
    c->k->reads++;
    pi_skills_reads++;
    norm = normalise(b, n, &nn);
    CL_FREE(b);
    if (!norm)
        return -1;
    fm_extract(norm, nn, &ys, &yn, &bs);
    frc = yn > 0 ? fm_parse(norm + ys, yn, &f, err, sizeof(err)) : (memset(&f, 0, sizeof(f)), 0);
    if (frc == -1) {
        free(norm);
        return -1;
    }
    if (frc == -2) {
        free(norm);
        fm_free(&f);
        if (declared)
            return add_diag(c->k, SKD_WARNING, err, path, 0, 0);
        return 0;
    }
    has_desc = f.desc.kind == FK_STR && f.desc.s;
    if (has_desc) {
        long i = 0;
        while (f.desc.s[i] && is_ws(f.desc.s[i]))
            i++;
        has_desc = f.desc.s[i] != 0;
    }
    if (!declared && !has_desc) {
        free(norm);
        fm_free(&f);
        return 0;
    }
    path_parent(path, dir, sizeof(dir));
    /* the description */
    if (!has_desc)
        rc = add_diag(c->k, SKD_WARNING, "description is required", path, 0, 0);
    else if (js_len(f.desc.s) > SK_MAX_DESC) {
        char m[100];
        long v = js_len(f.desc.s);
        char r[16];
        int t = 0, q;
        while (v) {
            r[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        cl_copy(m, "description exceeds 1024 characters (", sizeof(m));
        q = (int)strlen(m);
        while (t)
            m[q++] = r[--t];
        m[q] = 0;
        cl_cat(m, ")", sizeof(m));
        rc = add_diag(c->k, SKD_WARNING, m, path, 0, 0);
    }
    /* the name: the frontmatter's, else the directory's */
    name = f.name.kind == FK_STR && f.name.s && f.name.s[0] ? f.name.s : base_name(dir);
    if (rc == 0) {
        long len = js_len(name);
        const char *p;
        int bad = 0;
        if (len > SK_MAX_NAME) {
            char m[100], r[16];
            long v = len;
            int t = 0, q;
            while (v) {
                r[t++] = (char)('0' + v % 10);
                v /= 10;
            }
            cl_copy(m, "name exceeds 64 characters (", sizeof(m));
            q = (int)strlen(m);
            while (t)
                m[q++] = r[--t];
            m[q] = 0;
            cl_cat(m, ")", sizeof(m));
            rc = add_diag(c->k, SKD_WARNING, m, path, 0, 0);
        }
        for (p = name; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                bad = 1;
        if (!*name)
            bad = 1;
        if (rc == 0 && bad)
            rc = add_diag(c->k, SKD_WARNING, "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)", path, 0, 0);
        if (rc == 0 && (name[0] == '-' || (*name && name[strlen(name) - 1] == '-')))
            rc = add_diag(c->k, SKD_WARNING, "name must not start or end with a hyphen", path, 0, 0);
        if (rc == 0 && strstr(name, "--"))
            rc = add_diag(c->k, SKD_WARNING, "name must not contain consecutive hyphens", path, 0, 0);
    }
    if (rc == 0 && has_desc) {
        memset(s, 0, sizeof(*s));
        s->name = dupn(name, (long)strlen(name));
        s->desc = dupn(f.desc.s, (long)strlen(f.desc.s));
        cl_copy(s->path, path, sizeof(s->path));
        cl_copy(s->base, dir, sizeof(s->base));
        s->hidden = f.nomodel.kind == FK_TRUE;
        s->source = source;
        s->scope = scope;
        /* a --skill path has no source directory of its own: pi's loadSkills gives the skill's directory */
        cl_copy(s->sbase, scope == SKS_TEMP && source == SKO_LOCAL ? dir : sbase, sizeof(s->sbase));
        if (!s->name || !s->desc) {
            free(s->name);
            free(s->desc);
            rc = -1;
        } else
            *got = 1;
    }
    free(norm);
    fm_free(&f);
    return rc;
}

/* pi's addSkills for one file: a skill kept unless its file or its name was seen */
static int keep_skill(sk_ctx *c, pi_skill *s, char (*canon)[300], int *ncanon)
{
    pi_skills *k = c->k;
    char real[300];
    int i;
    if (!c->sys->canon || c->sys->canon(c->sys->u, s->path, real, sizeof(real)))
        cl_copy(real, s->path, sizeof(real));
    for (i = 0; i < *ncanon; i++)
        if (!strcmp(canon[i], real)) {
            free(s->name);
            free(s->desc);
            return 0;           /* the same file reached twice (a link): silent */
        }
    for (i = 0; i < k->n; i++)
        if (!strcmp(k->s[i].name, s->name)) {
            /* a collision: reported after the other diagnostics (pi: collisionDiagnostics), see pi_skills_load */
            char m[200];
            int rc;
            cl_copy(m, "name \"", sizeof(m));
            cl_cat(m, s->name, sizeof(m));
            cl_cat(m, "\" collision", sizeof(m));
            rc = add_diag(k, SKD_COLLISION, m, s->path, k->s[i].path, s->name);
            free(s->name);
            free(s->desc);
            return rc;
        }
    if (*ncanon < 512)
        cl_copy(canon[(*ncanon)++], real, 300);
    return add_skill(k, s);
}

/* ---- the whole load ---- */

static int diag_is_coll(const pi_skill_diag *d)
{
    return d->type == SKD_COLLISION;
}

/* the collision diagnostics go after all the others (pi: [...allDiagnostics, ...collisionDiagnostics]) */
static void collisions_last(pi_skills *k)
{
    pi_skill_diag *t = (pi_skill_diag *)malloc(sizeof(pi_skill_diag) * (size_t)(k->nd ? k->nd : 1));
    int i, m = 0;
    if (!t)
        return;
    for (i = 0; i < k->nd; i++)
        if (!diag_is_coll(&k->d[i]))
            t[m++] = k->d[i];
    for (i = 0; i < k->nd; i++)
        if (diag_is_coll(&k->d[i]))
            t[m++] = k->d[i];
    memcpy(k->d, t, sizeof(pi_skill_diag) * (size_t)k->nd);
    free(t);
}

int pi_skills_load(pi_skills *k, cl_sys *sys, const pi_skill_opts *o)
{
    sk_ctx c;
    fl_list files;
    sk_set gset, pset;
    int gcmd = -1, pcmd = -1, rc = 0, i;
    char gfile[300], pfile[300], proj_base[300], user_skills[300], user_agents[300], pskills[300];
    static char canon[512][300];
    int ncanon = 0;
    memset(k, 0, sizeof(*k));
    k->commands = 1;
    k->sys = sys;
    memset(&c, 0, sizeof(c));
    c.sys = sys;
    c.k = k;
    memset(&files, 0, sizeof(files));
    gfile[0] = pfile[0] = proj_base[0] = 0;
    memset(&gset, 0, sizeof(gset));
    memset(&pset, 0, sizeof(pset));
    if (o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "settings.json", gfile, sizeof(gfile)))
        gfile[0] = 0;
    if (o->trusted && o->cwd && path_join(o->cwd, ".pi", proj_base, sizeof(proj_base)) == 0 &&
        path_join(proj_base, "settings.json", pfile, sizeof(pfile)))
        pfile[0] = 0;
    read_settings(sys, gfile, &gset, &gcmd);
    if (o->trusted)
        read_settings(sys, pfile, &pset, &pcmd);
    /* settings.json's enableSkillCommands: the project's over the global (pi's merged settings) */
    if (pcmd >= 0)
        k->commands = pcmd;
    else if (gcmd >= 0)
        k->commands = gcmd;
    if (!o->none) {
        /* resolve(): the settings entries (project first), then the auto-discovered directories */
        if (o->trusted && o->cwd)
            rc = settings_files(&c, &pset, proj_base, SKS_PROJECT, &files);
        if (rc == 0 && o->agent_dir && o->agent_dir[0])
            rc = settings_files(&c, &gset, o->agent_dir, SKS_USER, &files);
        if (rc == 0 && o->trusted && o->cwd && path_join(proj_base, "skills", pskills, sizeof(pskills)) == 0)
            rc = auto_files(&c, pskills, 0, proj_base, &pset, SKS_PROJECT, &files);
        if (rc == 0 && o->trusted && o->cwd)
            rc = ancestor_agents(&c, o->cwd, &files, &pset);
        if (rc == 0 && o->agent_dir && o->agent_dir[0]) {
            if (path_join(o->agent_dir, "skills", user_skills, sizeof(user_skills)) == 0)
                rc = auto_files(&c, user_skills, 0, o->agent_dir, &gset, SKS_USER, &files);
            if (rc == 0 && path_join(o->agent_dir, "agents-skills", user_agents, sizeof(user_agents)) == 0)
                rc = auto_files(&c, user_agents, 1, o->agent_dir, &gset, SKS_USER, &files);
        }
        /* D10: bulky data lives in the data directory (UP-Term:var/amiga-pi) when the kit is there: its skills count as the
         * user's, after the agent directory's own */
        if (rc == 0 && o->data_dir && o->data_dir[0] && !(o->agent_dir && !strcmp(o->agent_dir, o->data_dir)) &&
            path_join(o->data_dir, "skills", user_skills, sizeof(user_skills)) == 0)
            rc = auto_files(&c, user_skills, 0, o->data_dir, &gset, SKS_USER, &files);
    }
    /* --skill: loadSkills' skillPaths, after the discovered ones */
    for (i = 0; i < o->npaths && rc == 0; i++) {
        char p[300];
        int kind;
        if (is_abs(o->paths[i]))
            cl_copy(p, o->paths[i], sizeof(p));
        else if (!o->cwd || path_join(o->cwd, o->paths[i], p, sizeof(p)))
            continue;
        kind = sys->kind(sys->u, p);
        c.cur_source = SKO_LOCAL;
        c.cur_scope = SKS_TEMP;
        if (kind == 2)
            rc = collect_dir(&c, p, 0, &files);
        else if (kind == 1)
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, ends_md(p) ? 0 : 2, "");
        else
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, 1, "");
    }
    for (i = 0; i < files.n && rc == 0; i++) {
        pi_skill s;
        int got = 0;
        if (files.e[i].bad || !ends_md(files.e[i].path)) {
            rc = add_diag(k, SKD_WARNING, files.e[i].bad == 1 ? "skill path does not exist" : "skill path is not a markdown file",
                          files.e[i].path, 0, 0);
            continue;
        }
        rc = load_file(&c, files.e[i].path, files.e[i].source, files.e[i].scope, files.e[i].sbase, &s, &got);
        if (rc == 0 && got)
            rc = keep_skill(&c, &s, canon, &ncanon);
    }
    fl_free(&files);
    collisions_last(k);
    return rc ? -1 : 0;
}

void pi_skills_free(pi_skills *k)
{
    int i;
    for (i = 0; i < k->n; i++) {
        free(k->s[i].name);
        free(k->s[i].desc);
    }
    for (i = 0; i < k->nd; i++) {
        free(k->d[i].msg);
        free(k->d[i].name);
    }
    free(k->s);
    free(k->d);
    memset(k, 0, sizeof(*k));
}

/* ---- the prompt section ---- */

static void xml_escape(jw *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': jw_rawz(out, "&amp;"); break;
        case '<': jw_rawz(out, "&lt;"); break;
        case '>': jw_rawz(out, "&gt;"); break;
        case '"': jw_rawz(out, "&quot;"); break;
        case '\'': jw_rawz(out, "&apos;"); break;
        default: jw_raw(out, s, 1); break;
        }
    }
}

int pi_skills_format(const pi_skills *k, const char *reader, jw *out)
{
    int i, vis = 0;
    for (i = 0; i < k->n; i++)
        if (!k->s[i].hidden)
            vis++;
    if (!vis)
        return 0;
    jw_rawz(out, "The following skills provide specialized instructions for specific tasks.\n");
    jw_rawz(out, !reader ? "Load a skill's file when the task matches its description."
                         : !strcmp(reader, "read") ? "Use the read tool to load a skill's file when the task matches its description."
                                                   : "Use bash to load a skill's file when the task matches its description.");
    jw_rawz(out, "\nWhen a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / "
                 "dirname of the path) and use that absolute path in tool commands.\n\n<available_skills>");
    for (i = 0; i < k->n; i++) {
        if (k->s[i].hidden)
            continue;
        jw_rawz(out, "\n  <skill>\n    <name>");
        xml_escape(out, k->s[i].name);
        jw_rawz(out, "</name>\n    <description>");
        xml_escape(out, k->s[i].desc);
        jw_rawz(out, "</description>\n    <location>");
        xml_escape(out, k->s[i].path);
        jw_rawz(out, "</location>\n  </skill>");
    }
    jw_rawz(out, "\n</available_skills>");
    return out->oom ? -1 : 0;
}

/* ---- /skill:name ---- */

int pi_skill_expand(const pi_skills *k, const char *text, jw *out)
{
    const char *sp, *args, *nm;
    long nl, an;
    int i;
    char *b = 0, *norm;
    long n = 0, nn = 0, ys, yn, bs, e;
    if (strncmp(text, "/skill:", 7))
        return 0;
    sp = strchr(text, ' ');
    nm = text + 7;
    nl = sp ? (long)(sp - nm) : (long)strlen(nm);
    args = sp ? sp + 1 : "";
    for (i = 0; i < k->n; i++)
        if ((long)strlen(k->s[i].name) == nl && !strncmp(k->s[i].name, nm, (size_t)nl))
            break;
    if (i == k->n)
        return 0;
    if (!k->sys || k->sys->read(k->sys->u, k->s[i].path, SK_FILE_MAX, &b, &n))
        return 0;
    pi_skills_reads++;
    norm = normalise(b, n, &nn);
    CL_FREE(b);
    if (!norm)
        return -1;
    fm_extract(norm, nn, &ys, &yn, &bs);
    /* stripFrontmatter(content).trim(): the body after the frontmatter, or the whole text, trimmed */
    {
        long a = bs;
        e = nn;
        while (a < e && is_ws(norm[a]))
            a++;
        while (e > a && is_ws(norm[e - 1]))
            e--;
        jw_rawz(out, "<skill name=\"");
        jw_rawz(out, k->s[i].name);
        jw_rawz(out, "\" location=\"");
        jw_rawz(out, k->s[i].path);
        jw_rawz(out, "\">\nReferences are relative to ");
        jw_rawz(out, k->s[i].base);
        jw_rawz(out, ".\n\n");
        jw_raw(out, norm + a, e - a);
        jw_rawz(out, "\n</skill>");
    }
    free(norm);
    /* args: text after the first space, trimmed */
    an = (long)strlen(args);
    while (an && is_ws(*args)) {
        args++;
        an--;
    }
    while (an && is_ws(args[an - 1]))
        an--;
    if (an) {
        jw_rawz(out, "\n\n");
        jw_raw(out, args, an);
    }
    return out->oom ? -1 : 1;
}

void pi_skill_command(const pi_skill *s, char *name, long ncap, char *desc, long dcap)
{
    const char *tag = s->scope == SKS_USER ? "[u] " : s->scope == SKS_PROJECT ? "[p] " : "[t] ";
    cl_copy(name, "skill:", ncap);
    cl_cat(name, s->name, ncap);
    cl_copy(desc, tag, dcap);
    cl_cat(desc, s->desc, dcap);
}

int pi_skills_commands(const pi_skills *k, jw *out)
{
    int i;
    for (i = 0; i < k->n; i++) {
        const pi_skill *s = &k->s[i];
        char nm[400];
        cl_copy(nm, "skill:", sizeof(nm));
        cl_cat(nm, s->name, sizeof(nm));
        jw_rawz(out, i ? "," : "");
        jw_rawz(out, "{\"name\":");
        jw_str(out, nm, (long)strlen(nm));
        jw_rawz(out, ",\"description\":");
        jw_str(out, s->desc, (long)strlen(s->desc));
        jw_rawz(out, ",\"source\":\"skill\",\"sourceInfo\":{\"path\":");
        jw_str(out, s->path, (long)strlen(s->path));
        jw_rawz(out, ",\"source\":\"");
        jw_rawz(out, s->source == SKO_AUTO ? "auto" : "local");
        jw_rawz(out, "\",\"scope\":\"");
        jw_rawz(out, s->scope == SKS_USER ? "user" : s->scope == SKS_PROJECT ? "project" : "temporary");
        jw_rawz(out, "\",\"origin\":\"top-level\"");
        if (s->sbase[0]) {
            jw_rawz(out, ",\"baseDir\":");
            jw_str(out, s->sbase, (long)strlen(s->sbase));
        }
        jw_rawz(out, "}}");
    }
    return out->oom ? -1 : i;
}

/* ---- the startup report ---- */

void pi_skill_label(const pi_skill *s, char *out, long cap)
{
    if (s->source == SKO_AUTO)
        cl_copy(out, s->scope == SKS_USER ? "auto (user)" : "auto (project)", cap);
    else if (s->scope == SKS_USER)
        cl_copy(out, "user", cap);
    else if (s->scope == SKS_PROJECT)
        cl_copy(out, "project", cap);
    else
        cl_copy(out, "path (temp)", cap);
}

/* pi's formatPathWithSource: "<label> <path>" for a loaded skill's file, else the path */
static void path_with_source(const pi_skills *k, const char *path, char *out, long cap)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (!strcmp(k->s[i].path, path)) {
            pi_skill_label(&k->s[i], out, cap);
            cl_cat(out, " ", cap);
            cl_cat(out, path, cap);
            return;
        }
    cl_copy(out, path, cap);
}

static int name_cmp(const void *a, const void *b)
{
    return pi_locale_compare(*(const char *const *)a, *(const char *const *)b);
}

void pi_skills_listing(const pi_skills *k, pi_skl_fn fn, void *u)
{
    const char **names;
    int i;
    jw w;
    if (!k->n)
        return;
    names = (const char **)malloc(sizeof(char *) * (size_t)k->n);
    if (!names)
        return;
    for (i = 0; i < k->n; i++)
        names[i] = k->s[i].name;
    qsort(names, (size_t)k->n, sizeof(char *), name_cmp);
    jw_init(&w);
    jw_rawz(&w, "  ");
    for (i = 0; i < k->n; i++) {
        if (i)
            jw_rawz(&w, ", ");
        jw_rawz(&w, names[i]);
    }
    if (!w.oom)
        fn(u, SKL_LISTING, w.p);
    jw_free(&w);
    free(names);
}

int pi_skills_report(const pi_skills *k, pi_skl_fn fn, void *u)
{
    int i, lines = 0;
    char buf[900], p[700];
    /* the collisions grouped by name, in the order the names first collided */
    for (i = 0; i < k->nd; i++) {
        int j, seen = 0;
        if (k->d[i].type != SKD_COLLISION)
            continue;
        for (j = 0; j < i; j++)
            if (k->d[j].type == SKD_COLLISION && !strcmp(k->d[j].name, k->d[i].name))
                seen = 1;
        if (seen)
            continue;
        cl_copy(buf, "  \"", sizeof(buf));
        cl_cat(buf, k->d[i].name, sizeof(buf));
        cl_cat(buf, "\" collision:", sizeof(buf));
        fn(u, SKL_COLL_HEAD, buf);
        lines++;
        path_with_source(k, k->d[i].winner, p, sizeof(p));
        cl_copy(buf, "    \342\234\223 ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, SKL_COLL_WIN, buf);
        lines++;
        for (j = i; j < k->nd; j++)
            if (k->d[j].type == SKD_COLLISION && !strcmp(k->d[j].name, k->d[i].name)) {
                path_with_source(k, k->d[j].path, p, sizeof(p));
                cl_copy(buf, "    \342\234\227 ", sizeof(buf));
                cl_cat(buf, p, sizeof(buf));
                cl_cat(buf, " (skipped)", sizeof(buf));
                fn(u, SKL_COLL_LOSE, buf);
                lines++;
            }
    }
    for (i = 0; i < k->nd; i++) {
        if (k->d[i].type == SKD_COLLISION)
            continue;
        path_with_source(k, k->d[i].path, p, sizeof(p));
        cl_copy(buf, "  ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, k->d[i].type == SKD_ERROR ? SKL_ERROR : SKL_WARN, buf);
        cl_copy(buf, "    ", sizeof(buf));
        cl_cat(buf, k->d[i].msg, sizeof(buf));
        fn(u, k->d[i].type == SKD_ERROR ? SKL_ERROR : SKL_WARN, buf);
        lines += 2;
    }
    return lines;
}
