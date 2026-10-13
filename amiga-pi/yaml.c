/* yaml.c -- the composer of the yaml package (yaml 2.9.0 compose, all files, doc/directives.js, errors.js prettifyError, the part of
 * public-api.js parse() and Document.toJS() that decides what `parse` throws) over the CST of yaml_cst.c. See yaml.h. */
#include <stdlib.h>
#include <string.h>
#include "yaml.h"
#include "yaml_int.h"

#define Y_DEPTH 1500

/* ---- the nodes the composer makes ---- */

enum { YN_SCALAR, YN_MAP, YN_SEQ, YN_ALIAS };
enum { VK_NULL, VK_BOOL, VK_NUM, VK_STR, VK_OTHER };

typedef struct ynode ynode;

typedef struct ypair {
    ynode *key, *val;
} ypair;

struct ynode {
    int kind;
    long range[3];
    int vk, b;
    double num;
    char *str;
    long strn;
    char *anchor;               /* 0: none; "" is an empty anchor name */
    char *alias;                /* YN_ALIAS: the name */
    ynode *target;              /* YN_ALIAS: the node it names */
    ypair *pairs;               /* YN_MAP */
    ynode **items;              /* YN_SEQ */
    int n, cap;
};

typedef struct yerr {
    long p0, p1;                /* byte offsets of the range */
    long ulen;                  /* its length in UTF-16 units, as yaml's source.length counts it */
    char *msg;
} yerr;

typedef struct yerrs {
    yerr *v;
    int n, cap;
} yerrs;

typedef struct ydoc {
    ynode *contents;
    long range[3];
    yerrs errors;
    int docStart;
} ydoc;

typedef struct comp {
    ya *a;
    const char *src;
    long srclen;
    const long *ls;             /* the parser's line starts */
    long nls;
    yerrs errors;               /* composer.errors */
    ydoc *doc;
    ydoc *yielded[2];
    int nyield;
    int atDirectives;
    int atKey, atRoot;
    char *thandle[8], *tprefix[8];
    int ntags;
    int depth;
    int oom;
} comp;

typedef struct props {
    cn *comma, *found;
    int spaceBefore;
    char *comment;
    long commentn;
    int hasNewline;
    cn *anchor, *tag, *newlineAfterProp;
    long end, start;
} props;

/* ---- small things ---- */

static long u16_len(const char *s, long n);

static char *mkv(comp *C, const char *const *parts, int np)
{
    long n = 0;
    int i;
    char *out;
    for (i = 0; i < np; i++)
        n += (long)strlen(parts[i]);
    out = (char *)ya_alloc(C->a, (size_t)n + 1);
    if (!out) {
        C->oom = 1;
        return (char *)"";
    }
    n = 0;
    for (i = 0; i < np; i++) {
        memcpy(out + n, parts[i], strlen(parts[i]));
        n += (long)strlen(parts[i]);
    }
    out[n] = 0;
    return out;
}

static char *mk2(comp *C, const char *a, const char *b)
{
    const char *p[2];
    p[0] = a;
    p[1] = b;
    return mkv(C, p, 2);
}

static char *mk3(comp *C, const char *a, const char *b, const char *c)
{
    const char *p[3];
    p[0] = a;
    p[1] = b;
    p[2] = c;
    return mkv(C, p, 3);
}

static char *mk4(comp *C, const char *a, const char *b, const char *c, const char *d)
{
    const char *p[4];
    p[0] = a;
    p[1] = b;
    p[2] = c;
    p[3] = d;
    return mkv(C, p, 4);
}

static char *mkn(comp *C, const char *s, long n)
{
    char *p = ya_dup(C->a, s, n);
    if (!p) {
        C->oom = 1;
        return (char *)"";
    }
    return p;
}

static void errs_add(comp *C, yerrs *l, long p0, long p1, long ulen, const char *msg)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        yerr *q = (yerr *)ya_alloc(C->a, sizeof(yerr) * (size_t)nc);
        if (!q) {
            C->oom = 1;
            return;
        }
        if (l->n)
            memcpy(q, l->v, sizeof(yerr) * (size_t)l->n);
        l->v = q;
        l->cap = nc;
    }
    l->v[l->n].p0 = p0;
    l->v[l->n].p1 = p1;
    l->v[l->n].ulen = ulen;
    l->v[l->n].msg = (char *)msg;
    l->n++;
}

static void e_rng(comp *C, long a, long b, const char *msg)
{
    long ua = u16_len(C->src, a > C->srclen ? C->srclen : a), ub = u16_len(C->src, b > C->srclen ? C->srclen : b);
    errs_add(C, &C->errors, a, b, ub - ua, msg);
}

static void e_off(comp *C, long off, const char *msg)
{
    errs_add(C, &C->errors, off, off + 1, 1, msg);
}

static void e_tok(comp *C, const cn *t, const char *msg)
{
    errs_add(C, &C->errors, t->offset, t->offset + (t->has_src ? t->srclen : 1), t->has_src ? u16_len(t->src, t->srclen) : 1, msg);
}

static ynode *new_node(comp *C, int kind)
{
    ynode *nd = (ynode *)ya_alloc(C->a, sizeof(ynode));
    if (!nd) {
        C->oom = 1;
        return 0;
    }
    nd->kind = kind;
    return nd;
}

static void node_pair(comp *C, ynode *m, ynode *k, ynode *v)
{
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 4;
        ypair *q = (ypair *)ya_alloc(C->a, sizeof(ypair) * (size_t)nc);
        if (!q) {
            C->oom = 1;
            return;
        }
        if (m->n)
            memcpy(q, m->pairs, sizeof(ypair) * (size_t)m->n);
        m->pairs = q;
        m->cap = nc;
    }
    m->pairs[m->n].key = k;
    m->pairs[m->n].val = v;
    m->n++;
}

static void node_item(comp *C, ynode *s, ynode *v)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 4;
        ynode **q = (ynode **)ya_alloc(C->a, sizeof(ynode *) * (size_t)nc);
        if (!q) {
            C->oom = 1;
            return;
        }
        if (s->n)
            memcpy(q, s->items, sizeof(ynode *) * (size_t)s->n);
        s->items = q;
        s->cap = nc;
    }
    s->items[s->n++] = v;
}

/* a growing string */
typedef struct sb {
    char *p;
    long n, cap;
    int oom;
} sb;

static void sb_add(sb *b, const char *s, long n)
{
    if (n <= 0)
        return;
    if (b->n + n + 1 > b->cap) {
        long nc = b->cap ? b->cap * 2 : 64;
        char *q;
        while (nc < b->n + n + 1)
            nc *= 2;
        q = (char *)realloc(b->p, (size_t)nc);
        if (!q) {
            b->oom = 1;
            return;
        }
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_ch(sb *b, int c)
{
    char ch = (char)c;
    sb_add(b, &ch, 1);
}

static void sb_utf8(sb *b, unsigned long cp)
{
    char t[4];
    if (cp < 0x80) {
        t[0] = (char)cp;
        sb_add(b, t, 1);
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        sb_add(b, t, 2);
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        sb_add(b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        sb_add(b, t, 4);
    }
}

/* the number of UTF-16 units of the UTF-8 text s[0..n) */
static long u16_len(const char *s, long n)
{
    long k = 0, i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) == 0x80)
            continue;
        k += c >= 0xF0 ? 2 : 1;
    }
    return k;
}

/* the byte offset after `units` UTF-16 units from byte offset from (JavaScript's substr over UTF-8 text) */
static long u16_adv(const char *s, long n, long from, long units)
{
    long i = from;
    while (units > 0 && i < n) {
        unsigned char c = (unsigned char)s[i];
        long len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        units -= c >= 0xF0 ? 2 : 1;
        i += len;
    }
    return i > n ? n : i;
}

/* ---- resolveProps, resolveEnd ---- */

static int is_space_tab(const cn *t)
{
    long i;
    for (i = 0; i < t->srclen; i++)
        if (t->src[i] == '\t')
            return 1;
    return 0;
}

static void resolve_props(comp *C, const cnl *tokens, const char *flow, int indicator, const cn *next, long offset, long parentIndent,
                          int startOnNewline, props *out)
{
    int spaceBefore = 0, atNewline = startOnNewline, hasSpace = startOnNewline, hasNewline = 0, reqSpace = 0, i;
    sb comment, commentSep;
    cn *tab = 0, *anchor = 0, *tag = 0, *newlineAfterProp = 0, *comma = 0, *found = 0;
    long start = -1, end;
    memset(&comment, 0, sizeof(comment));
    memset(&commentSep, 0, sizeof(commentSep));
    for (i = 0; i < (tokens ? tokens->n : 0); i++) {
        cn *token = tokens->v[i];
        if (reqSpace) {
            if (token->type != T_SPACE && token->type != T_NEWLINE && token->type != T_COMMA)
                e_off(C, token->offset, "Tags and anchors must be separated from the next token by white space");
            reqSpace = 0;
        }
        if (tab) {
            if (atNewline && token->type != T_COMMENT && token->type != T_NEWLINE)
                e_tok(C, tab, "Tabs are not allowed as indentation");
            tab = 0;
        }
        if (token->type == T_SPACE) {
            if (!flow && (indicator != T_DOC_START || !next || next->type != T_FLOW_COLL) && is_space_tab(token))
                tab = token;
            hasSpace = 1;
        } else if (token->type == T_COMMENT) {
            if (!hasSpace)
                e_tok(C, token, "Comments must be separated from other tokens by white space characters");
            if (!comment.n)
                sb_add(&comment, token->srclen > 1 ? token->src + 1 : " ", token->srclen > 1 ? token->srclen - 1 : 1);
            else {
                sb_add(&comment, commentSep.p, commentSep.n);
                sb_add(&comment, token->srclen > 1 ? token->src + 1 : " ", token->srclen > 1 ? token->srclen - 1 : 1);
            }
            commentSep.n = 0;
            atNewline = 0;
        } else if (token->type == T_NEWLINE) {
            if (atNewline) {
                if (comment.n)
                    sb_add(&comment, token->src, token->srclen);
                else if (!found || indicator != T_SEQ_IND)
                    spaceBefore = 1;
            } else
                sb_add(&commentSep, token->src, token->srclen);
            atNewline = 1;
            hasNewline = 1;
            if (anchor || tag)
                newlineAfterProp = token;
            hasSpace = 1;
        } else if (token->type == T_ANCHOR) {
            if (anchor)
                e_tok(C, token, "A node can have at most one anchor");
            anchor = token;
            if (start < 0)
                start = token->offset;
            atNewline = 0;
            hasSpace = 0;
            reqSpace = 1;
        } else if (token->type == T_TAG) {
            if (tag)
                e_tok(C, token, "A node can have at most one tag");
            tag = token;
            if (start < 0)
                start = token->offset;
            atNewline = 0;
            hasSpace = 0;
            reqSpace = 1;
        } else if (token->type == indicator) {
            if (anchor || tag) {
                char *srcs = mkn(C, token->src, token->srclen);
                e_tok(C, token, mk3(C, "Anchors and tags must be after the ", srcs, " indicator"));
            }
            if (found) {
                char *s = mkn(C, token->src, token->srclen);
                e_tok(C, token, mk4(C, "Unexpected ", s, " in ", flow ? flow : "collection"));
            }
            found = token;
            atNewline = indicator == T_SEQ_IND || indicator == T_EXPL_KEY;
            hasSpace = 0;
        } else if (token->type == T_COMMA && flow) {
            if (comma)
                e_tok(C, token, mk2(C, "Unexpected , in ", flow));
            comma = token;
            atNewline = 0;
            hasSpace = 0;
        } else {
            e_tok(C, token, mk3(C, "Unexpected ", yaml_type_name(token->type), " token"));
            atNewline = 0;
            hasSpace = 0;
        }
    }
    if (tokens && tokens->n) {
        cn *last = tokens->v[tokens->n - 1];
        end = last->offset + last->srclen;
    } else
        end = offset;
    if (reqSpace && next && next->type != T_SPACE && next->type != T_NEWLINE && next->type != T_COMMA &&
        (next->type != T_SCALAR || next->srclen != 0))
        e_off(C, next->offset, "Tags and anchors must be separated from the next token by white space");
    if (tab && ((atNewline && tab->indent <= parentIndent) || (next && (next->type == T_BLOCK_MAP || next->type == T_BLOCK_SEQ))))
        e_tok(C, tab, "Tabs are not allowed as indentation");
    memset(out, 0, sizeof(*out));
    out->comma = comma;
    out->found = found;
    out->spaceBefore = spaceBefore;
    out->commentn = comment.n;
    out->comment = comment.n ? mkn(C, comment.p, comment.n) : (char *)"";
    out->hasNewline = hasNewline;
    out->anchor = anchor;
    out->tag = tag;
    out->newlineAfterProp = newlineAfterProp;
    out->end = end;
    out->start = start >= 0 ? start : end;
    free(comment.p);
    free(commentSep.p);
}

/* resolveEnd(end, offset, reqSpace): the offset after the tokens */
static long resolve_end(comp *C, const cnl *end, long offset, int reqSpace)
{
    if (end) {
        int hasSpace = 0, i;
        for (i = 0; i < end->n; i++) {
            const cn *t = end->v[i];
            switch (t->type) {
            case T_SPACE:
                hasSpace = 1;
                break;
            case T_COMMENT:
                if (reqSpace && !hasSpace)
                    e_tok(C, t, "Comments must be separated from other tokens by white space characters");
                break;
            case T_NEWLINE:
                hasSpace = 1;
                break;
            default:
                e_tok(C, t, mk3(C, "Unexpected ", yaml_type_name(t->type), " at node end"));
            }
            offset += t->srclen;
        }
    }
    return offset;
}

static long empty_pos(long offset, const cnl *before, int pos)
{
    if (before) {
        int i;
        if (pos < 0)
            pos = before->n;
        for (i = pos - 1; i >= 0; --i) {
            const cn *st = before->v[i];
            if (st->type == T_SPACE || st->type == T_COMMENT || st->type == T_NEWLINE) {
                offset -= st->srclen;
                continue;
            }
            ++i;
            while (i < before->n && before->v[i]->type == T_SPACE) {
                offset += before->v[i]->srclen;
                ++i;
            }
            break;
        }
    }
    return offset;
}

/* ---- scalars ---- */

/* foldLines(source) of resolve-flow-scalar.js */
static void fold_lines(sb *out, const char *s, long n)
{
    long first_nl = -1, i, pos, e;
    for (i = 0; i < n; i++)
        if (s[i] == '\n') {
            first_nl = i;
            break;
        }
    if (first_nl < 0) {
        sb_add(out, s, n);
        return;
    }
    /* the first line, trailing blanks and CR off */
    e = first_nl;
    if (e > 0 && s[e - 1] == '\r')
        e--;
    while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t'))
        e--;
    sb_add(out, s, e);
    {
        char sep = ' ';
        pos = first_nl + 1;
        for (;;) {
            long nl = -1, a, b;
            for (i = pos; i < n; i++)
                if (s[i] == '\n') {
                    nl = i;
                    break;
                }
            if (nl < 0)
                break;
            a = pos;
            while (a < nl && (s[a] == ' ' || s[a] == '\t'))
                a++;
            b = nl;
            if (b > a && s[b - 1] == '\r')
                b--;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
                b--;
            if (b == a) {
                if (sep == '\n')
                    sb_ch(out, sep);
                else
                    sep = '\n';
            } else {
                sb_ch(out, sep);
                sb_add(out, s + a, b - a);
                sep = ' ';
            }
            pos = nl + 1;
        }
        sb_ch(out, sep);
        while (pos < n && (s[pos] == ' ' || s[pos] == '\t'))
            pos++;
        sb_add(out, s + pos, n - pos);
    }
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* doubleQuotedValue(source) */
static void double_quoted_value(comp *C, long offset, const char *s, long n, sb *res)
{
    long i;
    for (i = 1; i < n - 1; ++i) {
        char ch = s[i];
        if (ch == '\r' && i + 1 < n && s[i + 1] == '\n')
            continue;
        if (ch == '\n') {
            int fold = 0;
            long j = i;
            char c2;
            c2 = j + 1 < n ? s[j + 1] : 0;
            while (c2 == ' ' || c2 == '\t' || c2 == '\n' || c2 == '\r') {
                if (c2 == '\r' && !(j + 2 < n && s[j + 2] == '\n'))
                    break;
                if (c2 == '\n')
                    fold++;
                j += 1;
                c2 = j + 1 < n ? s[j + 1] : 0;
            }
            if (!fold)
                sb_ch(res, ' ');
            else {
                int k;
                for (k = 0; k < fold; k++)
                    sb_ch(res, '\n');
            }
            i = j;
        } else if (ch == '\\') {
            int next;
            ++i;
            next = i < n ? (unsigned char)s[i] : -1;
            switch (next) {
            case '0': sb_ch(res, 0); break;
            case 'a': sb_ch(res, 7); break;
            case 'b': sb_ch(res, 8); break;
            case 'e': sb_ch(res, 27); break;
            case 'f': sb_ch(res, 12); break;
            case 'n': sb_ch(res, 10); break;
            case 'r': sb_ch(res, 13); break;
            case 't': sb_ch(res, 9); break;
            case 'v': sb_ch(res, 11); break;
            case 'N': sb_utf8(res, 0x85); break;
            case '_': sb_utf8(res, 0xA0); break;
            case 'L': sb_utf8(res, 0x2028); break;
            case 'P': sb_utf8(res, 0x2029); break;
            case ' ': sb_ch(res, ' '); break;
            case '"': sb_ch(res, '"'); break;
            case '/': sb_ch(res, '/'); break;
            case '\\': sb_ch(res, '\\'); break;
            case '\t': sb_ch(res, '\t'); break;
            case '\n': {
                int c3 = i + 1 < n ? (unsigned char)s[i + 1] : -1;
                while (c3 == ' ' || c3 == '\t')
                    c3 = (++i) + 1 < n ? (unsigned char)s[i + 1] : -1;
                break;
            }
            case 'x':
            case 'u':
            case 'U': {
                long len = next == 'x' ? 2 : next == 'u' ? 4 : 8, k, b0, b1;
                int ok = 1;
                unsigned long cp = 0;
                b0 = i + 1;
                b1 = u16_adv(s, n, b0, len);
                if (u16_len(s + b0, b1 - b0) != len)
                    ok = 0;
                for (k = b0; ok && k < b1; k++) {
                    int h = hexval((unsigned char)s[k]);
                    if (h < 0)
                        ok = 0;
                    else
                        cp = cp * 16 + (unsigned long)h;
                }
                if (ok && cp > 0x10FFFFUL)
                    ok = 0;
                if (ok)
                    sb_utf8(res, cp);
                else {
                    long q = i - 1, rend = u16_adv(s, n, q, len + 2);
                    char *raw = mkn(C, s + q, rend - q);
                    e_off(C, offset + q, mk2(C, "Invalid escape sequence ", raw));
                    sb_add(res, raw, (long)strlen(raw));
                }
                i = b1 - 1;
                break;
            }
            default: {
                if (next == '\r' && i + 1 < n && s[i + 1] == '\n') {
                    int c3;
                    i += 1;
                    c3 = i + 1 < n ? (unsigned char)s[i + 1] : -1;
                    while (c3 == ' ' || c3 == '\t')
                        c3 = (++i) + 1 < n ? (unsigned char)s[i + 1] : -1;
                } else {
                    long q = i - 1, rend = u16_adv(s, n, q, 2);
                    char *raw = mkn(C, s + q, rend - q);
                    e_off(C, offset + q, mk2(C, "Invalid escape sequence ", raw));
                    sb_add(res, raw, (long)strlen(raw));
                    /* the second character of raw was consumed with it when it is a multi-byte one */
                    if (rend - q > 2)
                        i = rend - 1;
                }
            }
            }
        } else if (ch == ' ' || ch == '\t') {
            long ws = i;
            char next = i + 1 < n ? s[i + 1] : 0;
            while (next == ' ' || next == '\t') {
                ++i;
                next = i + 1 < n ? s[i + 1] : 0;
            }
            if (next != '\n' && !(next == '\r' && i + 2 < n && s[i + 2] == '\n')) {
                if (i > ws)
                    sb_add(res, s + ws, i - ws + 1);
                else
                    sb_ch(res, ch);
            }
        } else
            sb_ch(res, ch);
    }
    if (n == 0 || s[n - 1] != '"' || n == 1)
        e_off(C, offset + n, "Missing closing \"quote");
}

/* ---- tags ---- */

static const char YAML_TAG[] = "tag:yaml.org,2002:";

/* decodeURIComponent: 0 ok (appended), -1 URIError */
static int decode_uri(sb *out, const char *s, long n)
{
    long i = 0;
    while (i < n) {
        if (s[i] != '%') {
            sb_ch(out, s[i]);
            i++;
            continue;
        }
        {
            int h1 = i + 1 < n ? hexval((unsigned char)s[i + 1]) : -1, h2 = i + 2 < n ? hexval((unsigned char)s[i + 2]) : -1;
            int b, need, k;
            unsigned char bytes[4];
            if (h1 < 0 || h2 < 0)
                return -1;
            b = h1 * 16 + h2;
            i += 3;
            if (b < 0x80) {
                sb_ch(out, b);
                continue;
            }
            need = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : -1;
            if (need < 0)
                return -1;
            bytes[0] = (unsigned char)b;
            for (k = 1; k <= need; k++) {
                int g1, g2;
                if (i >= n || s[i] != '%')
                    return -1;
                g1 = i + 1 < n ? hexval((unsigned char)s[i + 1]) : -1;
                g2 = i + 2 < n ? hexval((unsigned char)s[i + 2]) : -1;
                if (g1 < 0 || g2 < 0 || ((g1 * 16 + g2) & 0xC0) != 0x80)
                    return -1;
                bytes[k] = (unsigned char)(g1 * 16 + g2);
                i += 3;
            }
            sb_add(out, (const char *)bytes, need + 1);
        }
    }
    return 0;
}

/* Directives.tagName(source): the resolved name (arena), 0 after an error */
static const char *tag_name(comp *C, const cn *tok)
{
    const char *s = tok->src;
    long n = tok->srclen, i, hl;
    char *srcs = mkn(C, s, n);
    if (n == 1 && s[0] == '!')
        return "!";
    if (n < 1 || s[0] != '!') {
        e_tok(C, tok, mk2(C, "Not a valid tag: ", srcs));
        return 0;
    }
    if (n >= 2 && s[1] == '<') {
        char *verb = n >= 3 ? mkn(C, s + 2, n - 3) : mkn(C, "", 0);
        if (!strcmp(verb, "!") || !strcmp(verb, "!!")) {
            e_tok(C, tok, mk3(C, "Verbatim tags aren't resolved, so ", srcs, " is invalid."));
            return 0;
        }
        if (s[n - 1] != '>')
            e_tok(C, tok, "Verbatim tags must end with a >");
        return verb;
    }
    hl = 0;
    for (i = n - 1; i >= 0; i--)
        if (s[i] == '!') {
            hl = i + 1;
            break;
        }
    if (hl == n)
        e_tok(C, tok, mk3(C, "The ", srcs, " tag has no suffix"));
    for (i = 0; i < C->ntags; i++)
        if ((long)strlen(C->thandle[i]) == hl && !memcmp(C->thandle[i], s, (size_t)hl)) {
            sb b;
            memset(&b, 0, sizeof(b));
            sb_add(&b, C->tprefix[i], (long)strlen(C->tprefix[i]));
            if (decode_uri(&b, s + hl, n - hl)) {
                free(b.p);
                e_tok(C, tok, "URIError: URI malformed");
                return 0;
            }
            {
                const char *r = mkn(C, b.p ? b.p : "", b.n);
                free(b.p);
                return r;
            }
        }
    if (hl == 1 && s[0] == '!')
        return srcs;
    e_tok(C, tok, mk2(C, "Could not resolve tag: ", srcs));
    return 0;
}

/* ---- the core schema's scalar tags ---- */

enum { TK_STR, TK_NULL, TK_BOOL, TK_OCT, TK_INT, TK_HEX, TK_FNAN, TK_FEXP, TK_FLOAT, TK_OTHER };

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int tk_test(int tk, const char *v, long n)
{
    long i = 0;
    switch (tk) {
    case TK_NULL:
        return n == 0 || (n == 1 && v[0] == '~') || !strcmp(v, "null") || !strcmp(v, "Null") || !strcmp(v, "NULL");
    case TK_BOOL:
        return !strcmp(v, "true") || !strcmp(v, "True") || !strcmp(v, "TRUE") || !strcmp(v, "false") || !strcmp(v, "False") ||
               !strcmp(v, "FALSE");
    case TK_OCT:
        if (n < 3 || v[0] != '0' || v[1] != 'o')
            return 0;
        for (i = 2; i < n; i++)
            if (v[i] < '0' || v[i] > '7')
                return 0;
        return 1;
    case TK_INT:
        if (i < n && (v[i] == '-' || v[i] == '+'))
            i++;
        if (i >= n)
            return 0;
        for (; i < n; i++)
            if (!is_digit(v[i]))
                return 0;
        return 1;
    case TK_HEX:
        if (n < 3 || v[0] != '0' || v[1] != 'x')
            return 0;
        for (i = 2; i < n; i++)
            if (hexval((unsigned char)v[i]) < 0)
                return 0;
        return 1;
    case TK_FNAN: {
        const char *p = v;
        if (p[0] == '-' || p[0] == '+')
            p++;
        if (p != v || p[0] == '.') {
            if (p[0] == '.' && (!strcmp(p + 1, "inf") || !strcmp(p + 1, "Inf") || !strcmp(p + 1, "INF")))
                return 1;
        }
        return !strcmp(v, ".nan") || !strcmp(v, ".NaN") || !strcmp(v, ".NAN");
    }
    case TK_FEXP: {
        long d;
        if (i < n && (v[i] == '-' || v[i] == '+'))
            i++;
        if (i < n && v[i] == '.') {
            i++;
            d = 0;
            while (i < n && is_digit(v[i])) {
                i++;
                d++;
            }
            if (!d)
                return 0;
        } else {
            d = 0;
            while (i < n && is_digit(v[i])) {
                i++;
                d++;
            }
            if (!d)
                return 0;
            if (i < n && v[i] == '.') {
                i++;
                while (i < n && is_digit(v[i]))
                    i++;
            }
        }
        if (i >= n || (v[i] != 'e' && v[i] != 'E'))
            return 0;
        i++;
        if (i < n && (v[i] == '-' || v[i] == '+'))
            i++;
        d = 0;
        while (i < n && is_digit(v[i])) {
            i++;
            d++;
        }
        return d && i == n;
    }
    case TK_FLOAT: {
        long d;
        if (i < n && (v[i] == '-' || v[i] == '+'))
            i++;
        if (i < n && v[i] == '.') {
            i++;
            d = 0;
            while (i < n && is_digit(v[i])) {
                i++;
                d++;
            }
            return d && i == n;
        }
        d = 0;
        while (i < n && is_digit(v[i])) {
            i++;
            d++;
        }
        if (!d || i >= n || v[i] != '.')
            return 0;
        i++;
        while (i < n && is_digit(v[i]))
            i++;
        return i == n;
    }
    default:
        return 0;
    }
}

static void scalar_value(ynode *nd, int tk, const char *v, long n)
{
    switch (tk) {
    case TK_NULL:
        nd->vk = VK_NULL;
        break;
    case TK_BOOL:
        nd->vk = VK_BOOL;
        nd->b = v[0] == 't' || v[0] == 'T';
        break;
    case TK_OCT: {
        double acc = 0;
        long i;
        nd->vk = VK_NUM;
        for (i = 2; i < n; i++)
            acc = acc * 8 + (v[i] - '0');
        nd->num = acc;
        break;
    }
    case TK_HEX: {
        double acc = 0;
        long i;
        nd->vk = VK_NUM;
        for (i = 2; i < n; i++)
            acc = acc * 16 + hexval((unsigned char)v[i]);
        nd->num = acc;
        break;
    }
    case TK_INT:
    case TK_FEXP:
    case TK_FLOAT:
        nd->vk = VK_NUM;
        nd->num = strtod(v, 0);
        break;
    case TK_FNAN: {
        volatile double zero = 0.0, big = 1e308;
        nd->vk = VK_NUM;
        if (v[n - 1] == 'N' || v[n - 1] == 'n')
            nd->num = zero / zero;
        else
            nd->num = (v[0] == '-' ? -1.0 : 1.0) * big * 10.0;
        break;
    }
    case TK_OTHER:
        nd->vk = VK_OTHER;
        break;
    default:
        nd->vk = VK_STR;
        nd->str = (char *)v;
        nd->strn = n;
    }
}

/* the tag kind of an explicit tag name for the value v: findScalarTagByName */
static int tag_kind_by_name(const char *name, const char *v, long n)
{
    static const int cands_null[] = { TK_NULL, -1 };
    static const int cands_bool[] = { TK_BOOL, -1 };
    static const int cands_int[] = { TK_OCT, TK_INT, TK_HEX, -1 };
    static const int cands_float[] = { TK_FNAN, TK_FEXP, TK_FLOAT, -1 };
    const int *c = 0;
    if (strncmp(name, YAML_TAG, strlen(YAML_TAG)))
        return -1;
    name += strlen(YAML_TAG);
    if (!strcmp(name, "str"))
        return TK_STR;
    if (!strcmp(name, "null"))
        c = cands_null;
    else if (!strcmp(name, "bool"))
        c = cands_bool;
    else if (!strcmp(name, "int"))
        c = cands_int;
    else if (!strcmp(name, "float"))
        c = cands_float;
    else if (!strcmp(name, "binary") || !strcmp(name, "timestamp") || !strcmp(name, "merge"))
        return TK_OTHER;
    if (c)
        for (; *c >= 0; c++)
            if (tk_test(*c, v, n))
                return *c;
    return -1;
}

/* timestamp.test */
static int ts_digits(const char *v, long n, long *i, int min, int max)
{
    int k = 0;
    while (*i < n && k < max && is_digit((unsigned char)v[*i])) {
        (*i)++;
        k++;
    }
    return k >= min;
}

static int ts_match(const char *v, long n)
{
    long i = 0, save;
    if (!ts_digits(v, n, &i, 4, 4) || i >= n || v[i] != '-')
        return 0;
    i++;
    if (!ts_digits(v, n, &i, 1, 2) || i >= n || v[i] != '-')
        return 0;
    i++;
    if (!ts_digits(v, n, &i, 1, 2))
        return 0;
    if (i == n)
        return 1;
    if (v[i] == 't' || v[i] == 'T')
        i++;
    else if (v[i] == ' ' || v[i] == '\t') {
        while (i < n && (v[i] == ' ' || v[i] == '\t'))
            i++;
    } else
        return 0;
    if (!ts_digits(v, n, &i, 1, 2) || i >= n || v[i] != ':')
        return 0;
    i++;
    if (!ts_digits(v, n, &i, 1, 2) || i >= n || v[i] != ':')
        return 0;
    i++;
    if (!ts_digits(v, n, &i, 1, 2))
        return 0;
    if (i < n && v[i] == '.') {
        long j = i + 1;
        if (ts_digits(v, n, &j, 1, 1000000)) {
            i = j;
        }
    }
    if (i == n)
        return 1;
    save = i;
    while (i < n && (v[i] == ' ' || v[i] == '\t'))
        i++;
    if (i < n && v[i] == 'Z') {
        i++;
        return i == n;
    }
    if (i < n && (v[i] == '-' || v[i] == '+')) {
        long j = i + 1, k;
        /* [012]?[0-9](?::[0-9]{2})? then the end: try with and without the optional [012] */
        for (k = 0; k < 2; k++) {
            long p = j;
            if (k == 0) {
                if (!(p < n && (v[p] == '0' || v[p] == '1' || v[p] == '2')))
                    continue;
                p++;
            }
            if (!(p < n && is_digit((unsigned char)v[p])))
                continue;
            p++;
            if (p == n)
                return 1;
            if (p + 2 < n + 0 && v[p] == ':' && is_digit((unsigned char)v[p + 1]) && is_digit((unsigned char)v[p + 2]) && p + 3 == n)
                return 1;
        }
    }
    (void)save;
    return 0;
}

/* the value of a scalar with its tag resolved: tname the explicit tag (0 none), plain whether the schema's tests apply */
static void apply_tag(comp *C, ynode *nd, const char *tname, const cn *tagTok, const char *v, long vn, int plain)
{
    int tk = TK_STR;
    if (tname) {
        if (strcmp(tname, "!")) {
            tk = tag_kind_by_name(tname, v, vn);
            if (tk < 0)
                tk = TK_STR;
            else if (!strcmp(tname, "tag:yaml.org,2002:timestamp") && !ts_match(v, vn)) {
                e_tok(C, tagTok, "!!timestamp expects a date, starting with yyyy-mm-dd");
                tk = TK_STR;
            }
        }
    } else if (plain) {
        static const int order[] = { TK_NULL, TK_BOOL, TK_OCT, TK_INT, TK_HEX, TK_FNAN, TK_FEXP, TK_FLOAT, -1 };
        int k;
        for (k = 0; order[k] >= 0; k++)
            if (tk_test(order[k], v, vn)) {
                tk = order[k];
                break;
            }
    }
    scalar_value(nd, tk, v, vn);
}

/* ---- block scalars (resolve-block-scalar.js) ---- */

typedef struct bline {
    long indent;
    const char *c;
    long cn;
} bline;

static ynode *compose_block_scalar(comp *C, cn *tok)
{
    ynode *nd = new_node(C, YN_SCALAR);
    long start = tok->offset;
    const cn *h;
    const char *hs;
    long hn, i;
    int indent = 0, error = -1, hasSpace = 0;
    char mode, chomp = 0;
    long length;
    sb val;
    bline *lines = 0;
    long nlines = 0, linecap = 0;
    long chompStart, trimIndent, offset, contentStart, end;
    int prevMore = 0;
    const char *sep = "";
    if (!nd)
        return 0;
    nd->vk = VK_STR;
    if (tok->props.n == 0 || tok->props.v[0]->type != T_BLOCK_HDR) {
        e_tok(C, tok, "Block scalar header not found");
        nd->str = (char *)"";
        nd->range[0] = nd->range[1] = nd->range[2] = start;
        return nd;
    }
    h = tok->props.v[0];
    hs = h->src;
    hn = h->srclen;
    mode = hs[0];
    for (i = 1; i < hn; ++i) {
        char ch = hs[i];
        if (!chomp && (ch == '-' || ch == '+'))
            chomp = ch;
        else {
            int dn = (ch >= '0' && ch <= '9') ? ch - '0' : 0;
            if (!indent && dn)
                indent = dn;
            else if (error == -1)
                error = (int)(tok->offset + i);
        }
    }
    if (error != -1)
        e_off(C, error, mk2(C, "Block scalar header includes extra characters: ", mkn(C, hs, hn)));
    length = hn;
    for (i = 1; i < tok->props.n; ++i) {
        const cn *t = tok->props.v[i];
        switch (t->type) {
        case T_SPACE:
            hasSpace = 1;
            length += t->srclen;
            break;
        case T_NEWLINE:
            length += t->srclen;
            break;
        case T_COMMENT:
            if (!hasSpace)
                e_tok(C, t, "Comments must be separated from other tokens by white space characters");
            length += t->srclen;
            break;
        case T_ERROR:
            e_tok(C, t, t->msg);
            length += t->srclen;
            break;
        default:
            e_tok(C, t, mk2(C, "Unexpected token in block scalar header: ", yaml_type_name(t->type)));
            if (t->has_src)
                length += t->srclen;
        }
    }
    /* splitLines */
    if (tok->srclen > 0) {
        const char *s = tok->src;
        long n = tok->srclen, p = 0, e, m;
        e = 0;
        while (e < n && s[e] != '\n')
            e++;
        m = 0;
        while (m < e && s[m] == ' ')
            m++;
        for (;;) {
            if (nlines == linecap) {
                long nc = linecap ? linecap * 2 : 16;
                bline *q = (bline *)realloc(lines, sizeof(bline) * (size_t)nc);
                if (!q) {
                    nd->str = (char *)"";
                    return nd;
                }
                lines = q;
                linecap = nc;
            }
            lines[nlines].indent = m;
            lines[nlines].c = s + p + m;
            lines[nlines].cn = e - p - m;
            nlines++;
            if (e >= n)
                break;
            p = e + 1;
            m = 0;
            while (p + m < n && s[p + m] == ' ')
                m++;
            e = p + m;
            while (e < n && s[e] != '\n')
                e++;
        }
    }
    chompStart = nlines;
    for (i = nlines - 1; i >= 0; --i) {
        if (lines[i].cn == 0 || (lines[i].cn == 1 && lines[i].c[0] == '\r'))
            chompStart = i;
        else
            break;
    }
    memset(&val, 0, sizeof(val));
    if (chompStart == 0) {
        long e2;
        if (chomp == '+' && nlines > 0) {
            long cnt = nlines - 1 > 1 ? nlines - 1 : 1, k;
            for (k = 0; k < cnt; k++)
                sb_ch(&val, '\n');
        }
        e2 = start + length;
        if (tok->srclen > 0)
            e2 += tok->srclen;
        nd->str = mkn(C, val.p ? val.p : "", val.n);
        nd->strn = val.n;
        nd->range[0] = start;
        nd->range[1] = nd->range[2] = e2;
        free(val.p);
        free(lines);
        return nd;
    }
    trimIndent = tok->indent + indent;
    offset = tok->offset + length;
    contentStart = 0;
    for (i = 0; i < chompStart; ++i) {
        long ind = lines[i].indent;
        int empty = lines[i].cn == 0 || (lines[i].cn == 1 && lines[i].c[0] == '\r');
        if (empty) {
            if (indent == 0 && ind > trimIndent)
                trimIndent = ind;
        } else {
            if (ind < trimIndent)
                e_off(C, offset + ind, "Block scalars with more-indented leading empty lines must use an explicit indentation indicator");
            if (indent == 0)
                trimIndent = ind;
            contentStart = i;
            if (trimIndent == 0 && !C->atRoot)
                e_off(C, offset, "Block scalar values in collections must be indented");
            break;
        }
        offset += lines[i].indent + lines[i].cn + 1;
    }
    for (i = nlines - 1; i >= chompStart; --i)
        if (lines[i].indent > trimIndent)
            chompStart = i + 1;
    for (i = 0; i < contentStart; ++i) {
        long k;
        for (k = trimIndent; k < lines[i].indent; k++)
            sb_ch(&val, ' ');
        sb_ch(&val, '\n');
    }
    for (i = contentStart; i < chompStart; ++i) {
        long ind = lines[i].indent, cl = lines[i].cn, k;
        const char *c = lines[i].c;
        int crlf;
        offset += ind + cl + 1;
        crlf = cl > 0 && c[cl - 1] == '\r';
        if (crlf)
            cl -= 1;
        if (cl > 0 && ind < trimIndent) {
            const char *srcn = indent ? "explicit indentation indicator" : "first line";
            e_off(C, offset - cl - (crlf ? 2 : 1), mk2(C, "Block scalar lines must not be less indented than their ", srcn));
            ind = 0;
        }
        if (mode != '>') {
            sb_add(&val, sep, (long)strlen(sep));
            for (k = trimIndent; k < ind; k++)
                sb_ch(&val, ' ');
            sb_add(&val, c, cl);
            sep = "\n";
        } else if (ind > trimIndent || (cl > 0 && c[0] == '\t')) {
            if (!strcmp(sep, " "))
                sep = "\n";
            else if (!prevMore && !strcmp(sep, "\n"))
                sep = "\n\n";
            sb_add(&val, sep, (long)strlen(sep));
            for (k = trimIndent; k < ind; k++)
                sb_ch(&val, ' ');
            sb_add(&val, c, cl);
            sep = "\n";
            prevMore = 1;
        } else if (cl == 0) {
            if (!strcmp(sep, "\n"))
                sb_ch(&val, '\n');
            else
                sep = "\n";
        } else {
            sb_add(&val, sep, (long)strlen(sep));
            sb_add(&val, c, cl);
            sep = " ";
            prevMore = 0;
        }
    }
    switch (chomp) {
    case '-':
        break;
    case '+':
        for (i = chompStart; i < nlines; ++i) {
            long k;
            sb_ch(&val, '\n');
            for (k = trimIndent; k < lines[i].indent; k++)
                sb_ch(&val, ' ');
        }
        if (!val.n || val.p[val.n - 1] != '\n')
            sb_ch(&val, '\n');
        break;
    default:
        sb_ch(&val, '\n');
    }
    end = start + length + tok->srclen;
    nd->str = mkn(C, val.p ? val.p : "", val.n);
    nd->strn = val.n;
    nd->range[0] = start;
    nd->range[1] = nd->range[2] = end;
    free(val.p);
    free(lines);
    return nd;
}

/* composeScalar(ctx, token, tagToken) */
static ynode *compose_scalar(comp *C, cn *tok, const cn *tagTok)
{
    ynode *nd;
    const char *tname = 0;
    if (tok->type == T_BLOCK_SCALAR) {
        nd = compose_block_scalar(C, tok);
        if (!nd)
            return 0;
        if (tagTok)
            tname = tag_name(C, tagTok);
        apply_tag(C, nd, tname, tagTok, nd->str, nd->strn, 0);
        return nd;
    }
    {
        sb val;
        long offset = tok->offset, valueEnd, re;
        const char *s = tok->src;
        long n = tok->srclen;
        nd = new_node(C, YN_SCALAR);
        if (!nd)
            return 0;
        memset(&val, 0, sizeof(val));
        switch (tok->type) {
        case T_SCALAR: {
            const char *bad = 0;
            switch (n ? s[0] : 0) {
            case '\t': bad = "a tab character"; break;
            case ',': bad = "flow indicator character ,"; break;
            case '%': bad = "directive indicator character %"; break;
            case '|': bad = "block scalar indicator |"; break;
            case '>': bad = "block scalar indicator >"; break;
            case '@': bad = "reserved character @"; break;
            case '`': bad = "reserved character `"; break;
            default: break;
            }
            if (bad)
                e_off(C, offset, mk2(C, "Plain value cannot start with ", bad));
            fold_lines(&val, s, n);
            break;
        }
        case T_SQ_SCALAR: {
            sb tmp;
            long i;
            if (n == 0 || s[n - 1] != '\'' || n == 1)
                e_off(C, offset + n, "Missing closing 'quote");
            memset(&tmp, 0, sizeof(tmp));
            if (n >= 2)
                fold_lines(&tmp, s + 1, n - 2);
            for (i = 0; i < tmp.n; i++) {
                if (tmp.p[i] == '\'' && i + 1 < tmp.n && tmp.p[i + 1] == '\'') {
                    sb_ch(&val, '\'');
                    i++;
                } else
                    sb_ch(&val, tmp.p[i]);
            }
            free(tmp.p);
            break;
        }
        case T_DQ_SCALAR:
            double_quoted_value(C, offset, s, n, &val);
            break;
        default:
            e_tok(C, tok, mk2(C, "Expected a flow scalar value, but found: ", yaml_type_name(tok->type)));
            break;
        }
        valueEnd = offset + n;
        re = resolve_end(C, tok->has_end ? &tok->end : 0, valueEnd, 1);
        nd->range[0] = offset;
        nd->range[1] = valueEnd;
        nd->range[2] = re;
        if (tagTok)
            tname = tag_name(C, tagTok);
        {
            char *v = mkn(C, val.p ? val.p : "", val.n);
            long vn = val.n;
            free(val.p);
            apply_tag(C, nd, tname, tagTok, v, vn, tok->type == T_SCALAR);
        }
        return nd;
    }
}

static ynode *compose_node(comp *C, cn *token, props *pr);
static ynode *compose_empty_node(comp *C, long offset, const cnl *before, int pos, props *pr);

static ynode *compose_empty_node(comp *C, long offset, const cnl *before, int pos, props *pr)
{
    cn tok;
    ynode *nd;
    memset(&tok, 0, sizeof(tok));
    tok.type = T_SCALAR;
    tok.offset = empty_pos(offset, before, pos);
    tok.indent = -1;
    tok.src = "";
    tok.srclen = 0;
    tok.has_src = 1;
    nd = compose_scalar(C, &tok, pr->tag);
    if (!nd)
        return 0;
    if (pr->anchor) {
        nd->anchor = mkn(C, pr->anchor->src + 1, pr->anchor->srclen - 1);
        if (!nd->anchor[0])
            e_tok(C, pr->anchor, "Anchor cannot be an empty string");
    }
    if (pr->commentn)
        nd->range[2] = pr->end;
    return nd;
}

static ynode *compose_alias(comp *C, cn *tok)
{
    ynode *nd = new_node(C, YN_ALIAS);
    long valueEnd, re;
    if (!nd)
        return 0;
    nd->alias = mkn(C, tok->src + 1, tok->srclen - 1);
    if (!nd->alias[0])
        e_off(C, tok->offset, "Alias cannot be an empty string");
    valueEnd = tok->offset + tok->srclen;
    re = resolve_end(C, tok->has_end ? &tok->end : 0, valueEnd, 1);
    nd->range[0] = tok->offset;
    nd->range[1] = valueEnd;
    nd->range[2] = re;
    return nd;
}

/* ---- collections ---- */

static int contains_newline(const cn *key);

static int cnl_has_newline(const cnl *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (l->v[i]->type == T_NEWLINE)
            return 1;
    return 0;
}

static int contains_newline(const cn *key)
{
    long i;
    int k;
    if (!key)
        return 0;
    switch (key->type) {
    case T_ALIAS:
    case T_SCALAR:
    case T_DQ_SCALAR:
    case T_SQ_SCALAR:
        for (i = 0; i < key->srclen; i++)
            if (key->src[i] == '\n')
                return 1;
        if (key->has_end && cnl_has_newline(&key->end))
            return 1;
        return 0;
    case T_FLOW_COLL:
        for (k = 0; k < key->nitems; k++) {
            const citem *it = &key->items[k];
            if (cnl_has_newline(&it->start))
                return 1;
            if (it->has_sep && cnl_has_newline(&it->sep))
                return 1;
            if (contains_newline(it->key) || contains_newline(it->value))
                return 1;
        }
        return 0;
    default:
        return 1;
    }
}

static int node_eq(const ynode *a, const ynode *b)
{
    if (a == b)
        return 1;
    if (a->kind != YN_SCALAR || b->kind != YN_SCALAR || a->vk != b->vk)
        return 0;
    switch (a->vk) {
    case VK_NULL:
        return 1;
    case VK_BOOL:
        return a->b == b->b;
    case VK_NUM:
        return a->num == b->num;
    case VK_STR:
        return a->strn == b->strn && !memcmp(a->str, b->str, (size_t)a->strn);
    default:
        return 0;
    }
}

static int map_includes(const ynode *m, const ynode *search)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (node_eq(m->pairs[i].key, search))
            return 1;
    return 0;
}

static int has_indent_cn(const cn *t)
{
    return t->type != T_DOCUMENT && t->type != T_ERROR && t->type != T_DOC_END && t->type != T_DIRECTIVE;
}

static ynode *resolve_block_map(comp *C, cn *bm)
{
    ynode *map = new_node(C, YN_MAP);
    long offset = bm->offset, commentEnd = -1;
    int k;
    if (!map)
        return 0;
    if (C->atRoot)
        C->atRoot = 0;
    for (k = 0; k < bm->nitems; k++) {
        citem *ci = &bm->items[k];
        props keyProps, valueProps;
        int implicitKey;
        long keyStart;
        ynode *keyNode, *valueNode;
        const cn *nextk = ci->key ? ci->key : (ci->has_sep && ci->sep.n ? ci->sep.v[0] : 0);
        resolve_props(C, &ci->start, 0, T_EXPL_KEY, nextk, offset, bm->indent, 1, &keyProps);
        implicitKey = !keyProps.found;
        if (implicitKey) {
            if (ci->key) {
                if (ci->key->type == T_BLOCK_SEQ)
                    e_off(C, offset, "A block sequence may not be used as an implicit map key");
                else if (has_indent_cn(ci->key) && ci->key->indent != bm->indent)
                    e_off(C, offset, "All mapping items must start at the same column");
            }
            if (!keyProps.anchor && !keyProps.tag && !ci->has_sep) {
                commentEnd = keyProps.end;
                continue;
            }
            if (keyProps.newlineAfterProp || contains_newline(ci->key)) {
                if (ci->key)
                    e_tok(C, ci->key, "Implicit keys need to be on a single line");
                else if (ci->start.n)
                    e_tok(C, ci->start.v[ci->start.n - 1], "Implicit keys need to be on a single line");
                else
                    e_off(C, offset, "Implicit keys need to be on a single line");
            }
        } else if (keyProps.found->indent != bm->indent)
            e_off(C, offset, "All mapping items must start at the same column");
        C->atKey = 1;
        keyStart = keyProps.end;
        keyNode = ci->key ? compose_node(C, ci->key, &keyProps) : compose_empty_node(C, keyStart, &ci->start, -1, &keyProps);
        C->atKey = 0;
        if (!keyNode)
            return map;
        if (map_includes(map, keyNode))
            e_off(C, keyStart, "Map keys must be unique");
        resolve_props(C, ci->has_sep ? &ci->sep : 0, 0, T_MAP_VAL, ci->value, keyNode->range[2], bm->indent,
                      !ci->key || ci->key->type == T_BLOCK_SCALAR, &valueProps);
        offset = valueProps.end;
        if (valueProps.found) {
            if (implicitKey) {
                if (ci->value && ci->value->type == T_BLOCK_MAP && !valueProps.hasNewline)
                    e_off(C, offset, "Nested mappings are not allowed in compact mappings");
                if (keyProps.start < valueProps.found->offset - 1024)
                    e_rng(C, keyNode->range[0], keyNode->range[1],
                          "The : indicator must be at most 1024 chars after the start of an implicit block mapping key");
            }
            valueNode = ci->value ? compose_node(C, ci->value, &valueProps)
                                  : compose_empty_node(C, offset, ci->has_sep ? &ci->sep : 0, -1, &valueProps);
            if (!valueNode)
                return map;
            offset = valueNode->range[2];
            node_pair(C, map, keyNode, valueNode);
        } else {
            if (implicitKey)
                e_rng(C, keyNode->range[0], keyNode->range[1], "Implicit map keys need to be followed by map values");
            node_pair(C, map, keyNode, 0);
        }
    }
    if (commentEnd > 0 && commentEnd < offset)
        e_off(C, commentEnd, "Map comment with trailing content");
    map->range[0] = bm->offset;
    map->range[1] = offset;
    map->range[2] = commentEnd >= 0 ? commentEnd : offset;
    return map;
}

static ynode *resolve_block_seq(comp *C, cn *bs)
{
    ynode *seq = new_node(C, YN_SEQ);
    long offset = bs->offset, commentEnd = -1;
    int k;
    if (!seq)
        return 0;
    if (C->atRoot)
        C->atRoot = 0;
    if (C->atKey)
        C->atKey = 0;
    for (k = 0; k < bs->nitems; k++) {
        citem *ci = &bs->items[k];
        props pr;
        ynode *node;
        resolve_props(C, &ci->start, 0, T_SEQ_IND, ci->value, offset, bs->indent, 1, &pr);
        if (!pr.found) {
            if (pr.anchor || pr.tag || ci->value) {
                if (ci->value && ci->value->type == T_BLOCK_SEQ)
                    e_off(C, pr.end, "All sequence items must start at the same column");
                else
                    e_off(C, offset, "Sequence item without - indicator");
            } else {
                commentEnd = pr.end;
                continue;
            }
        }
        node = ci->value ? compose_node(C, ci->value, &pr) : compose_empty_node(C, pr.end, &ci->start, -1, &pr);
        if (!node)
            return seq;
        offset = node->range[2];
        node_item(C, seq, node);
    }
    seq->range[0] = bs->offset;
    seq->range[1] = offset;
    seq->range[2] = commentEnd >= 0 ? commentEnd : offset;
    return seq;
}

static int is_block_cn(const cn *t)
{
    return t && (t->type == T_BLOCK_MAP || t->type == T_BLOCK_SEQ);
}

static ynode *resolve_flow_collection(comp *C, cn *fc)
{
    int isMap = fc->fstart->srclen == 1 && fc->fstart->src[0] == '{';
    const char *fcName = isMap ? "flow map" : "flow sequence";
    ynode *coll = new_node(C, isMap ? YN_MAP : YN_SEQ);
    int atRoot = C->atRoot, i;
    long offset = fc->offset + fc->fstart->srclen, cePos;
    char expectedEnd = isMap ? '}' : ']';
    const cn *ce;
    cnl ee;
    if (!coll)
        return 0;
    if (atRoot)
        C->atRoot = 0;
    if (C->atKey)
        C->atKey = 0;
    for (i = 0; i < fc->nitems; ++i) {
        citem *ci = &fc->items[i];
        props pr, valueProps;
        ynode *keyNode, *valueNode;
        long keyStart;
        const cn *nextk = ci->key ? ci->key : (ci->has_sep && ci->sep.n ? ci->sep.v[0] : 0);
        resolve_props(C, &ci->start, fcName, T_EXPL_KEY, nextk, offset, fc->indent, 0, &pr);
        if (!pr.found) {
            if (!pr.anchor && !pr.tag && !ci->has_sep && !ci->value) {
                if (i == 0 && pr.comma)
                    e_tok(C, pr.comma, mk2(C, "Unexpected , in ", fcName));
                else if (i < fc->nitems - 1)
                    e_off(C, pr.start, mk2(C, "Unexpected empty item in ", fcName));
                offset = pr.end;
                continue;
            }
            if (!isMap && contains_newline(ci->key))
                e_tok(C, ci->key, "Implicit keys of flow sequence pairs need to be on a single line");
        }
        if (i == 0) {
            if (pr.comma)
                e_tok(C, pr.comma, mk2(C, "Unexpected , in ", fcName));
        } else {
            if (!pr.comma)
                e_off(C, pr.start, mk3(C, "Missing , between ", fcName, " items"));
            if (pr.commentn) {
                const char *prevc = 0;
                long prevn = 0;
                int j;
                for (j = 0; j < ci->start.n; j++) {
                    const cn *st = ci->start.v[j];
                    if (st->type == T_COMMA || st->type == T_SPACE)
                        continue;
                    if (st->type == T_COMMENT && st->srclen > 1) {
                        prevc = st->src + 1;
                        prevn = st->srclen - 1;
                    }
                    break;
                }
                if (prevc && prevn > 0) {
                    /* props.comment = props.comment.substring(prevItemComment.length + 1) */
                    if (pr.commentn > prevn + 1) {
                        pr.comment += prevn + 1;
                        pr.commentn -= prevn + 1;
                    } else {
                        pr.comment = (char *)"";
                        pr.commentn = 0;
                    }
                }
            }
        }
        if (!isMap && !ci->has_sep && !pr.found) {
            valueNode = ci->value ? compose_node(C, ci->value, &pr) : compose_empty_node(C, pr.end, ci->has_sep ? &ci->sep : 0, -1, &pr);
            if (!valueNode)
                return coll;
            node_item(C, coll, valueNode);
            offset = valueNode->range[2];
            if (is_block_cn(ci->value))
                e_rng(C, valueNode->range[0], valueNode->range[1], "Block collections are not allowed within flow collections");
        } else {
            C->atKey = 1;
            keyStart = pr.end;
            keyNode = ci->key ? compose_node(C, ci->key, &pr) : compose_empty_node(C, keyStart, &ci->start, -1, &pr);
            if (!keyNode)
                return coll;
            if (is_block_cn(ci->key))
                e_rng(C, keyNode->range[0], keyNode->range[1], "Block collections are not allowed within flow collections");
            C->atKey = 0;
            resolve_props(C, ci->has_sep ? &ci->sep : 0, fcName, T_MAP_VAL, ci->value, keyNode->range[2], fc->indent, 0, &valueProps);
            if (valueProps.found) {
                if (!isMap && !pr.found) {
                    if (ci->has_sep) {
                        int j;
                        for (j = 0; j < ci->sep.n; j++) {
                            if (ci->sep.v[j] == valueProps.found)
                                break;
                            if (ci->sep.v[j]->type == T_NEWLINE) {
                                e_tok(C, ci->sep.v[j], "Implicit keys of flow sequence pairs need to be on a single line");
                                break;
                            }
                        }
                    }
                    if (pr.start < valueProps.found->offset - 1024)
                        e_tok(C, valueProps.found,
                              "The : indicator must be at most 1024 chars after the start of an implicit flow sequence key");
                }
            } else if (ci->value) {
                if (ci->value->has_src && ci->value->srclen > 0 && ci->value->src[0] == ':')
                    e_tok(C, ci->value, mk2(C, "Missing space after : in ", fcName));
                else
                    e_off(C, valueProps.start, mk3(C, "Missing , or : between ", fcName, " items"));
            }
            valueNode = ci->value ? compose_node(C, ci->value, &valueProps)
                                  : valueProps.found ? compose_empty_node(C, valueProps.end, ci->has_sep ? &ci->sep : 0, -1, &valueProps) : 0;
            if (valueNode && is_block_cn(ci->value))
                e_rng(C, valueNode->range[0], valueNode->range[1], "Block collections are not allowed within flow collections");
            if (isMap) {
                if (map_includes(coll, keyNode))
                    e_off(C, keyStart, "Map keys must be unique");
                node_pair(C, coll, keyNode, valueNode);
            } else {
                ynode *m = new_node(C, YN_MAP);
                const long *er;
                if (!m)
                    return coll;
                node_pair(C, m, keyNode, valueNode);
                er = (valueNode ? valueNode : keyNode)->range;
                m->range[0] = keyNode->range[0];
                m->range[1] = er[1];
                m->range[2] = er[2];
                node_item(C, coll, m);
            }
            offset = valueNode ? valueNode->range[2] : valueProps.end;
        }
    }
    ce = fc->end.n ? fc->end.v[0] : 0;
    memset(&ee, 0, sizeof(ee));
    {
        int j;
        for (j = 1; j < fc->end.n; j++)
            cnl_push(C->a, &ee, fc->end.v[j]);
    }
    cePos = offset;
    if (ce && ce->srclen == 1 && ce->src[0] == expectedEnd)
        cePos = ce->offset + ce->srclen;
    else {
        char name[16];
        const char *msg;
        strcpy(name, isMap ? "Flow map" : "Flow sequence");
        msg = atRoot ? mk3(C, name, " must end with a ", isMap ? "}" : "]")
                     : mk3(C, name, " in block collection must be sufficiently indented and end with a ", isMap ? "}" : "]");
        e_off(C, offset, msg);
        if (ce && ce->srclen != 1) {
            /* ee.unshift(ce) */
            cnl ee2;
            int j;
            memset(&ee2, 0, sizeof(ee2));
            cnl_push(C->a, &ee2, (cn *)ce);
            for (j = 0; j < ee.n; j++)
                cnl_push(C->a, &ee2, ee.v[j]);
            ee = ee2;
        }
    }
    if (ee.n > 0) {
        long e2 = resolve_end(C, &ee, cePos, 1);
        coll->range[0] = fc->offset;
        coll->range[1] = cePos;
        coll->range[2] = e2;
    } else {
        coll->range[0] = fc->offset;
        coll->range[1] = cePos;
        coll->range[2] = cePos;
    }
    return coll;
}

static ynode *compose_collection(comp *C, cn *token, props *pr)
{
    const cn *tagTok = pr->tag;
    const char *tname = tagTok ? tag_name(C, tagTok) : 0;
    ynode *coll;
    (void)tname;
    if (token->type == T_BLOCK_SEQ) {
        const cn *anchor = pr->anchor, *nl = pr->newlineAfterProp, *lastProp;
        lastProp = anchor && tagTok ? (anchor->offset > tagTok->offset ? anchor : tagTok) : (anchor ? anchor : tagTok);
        if (lastProp && (!nl || nl->offset < lastProp->offset))
            e_tok(C, lastProp, "Missing newline after block sequence props");
    }
    if (token->type == T_BLOCK_MAP)
        coll = resolve_block_map(C, token);
    else if (token->type == T_BLOCK_SEQ)
        coll = resolve_block_seq(C, token);
    else
        coll = resolve_flow_collection(C, token);
    return coll;
}

static ynode *compose_node(comp *C, cn *token, props *pr)
{
    ynode *node = 0;
    switch (token->type) {
    case T_ALIAS:
        node = compose_alias(C, token);
        if (pr->anchor || pr->tag)
            e_tok(C, token, "An alias node must not specify any properties");
        break;
    case T_SCALAR:
    case T_SQ_SCALAR:
    case T_DQ_SCALAR:
    case T_BLOCK_SCALAR:
        node = compose_scalar(C, token, pr->tag);
        if (node && pr->anchor)
            node->anchor = mkn(C, pr->anchor->src + 1, pr->anchor->srclen - 1);
        break;
    case T_BLOCK_MAP:
    case T_BLOCK_SEQ:
    case T_FLOW_COLL:
        if (C->depth >= Y_DEPTH) {
            e_tok(C, token, "Maximum call stack size exceeded");
            break;
        }
        C->depth++;
        node = compose_collection(C, token, pr);
        C->depth--;
        if (node && pr->anchor)
            node->anchor = mkn(C, pr->anchor->src + 1, pr->anchor->srclen - 1);
        break;
    default:
        e_tok(C, token, token->type == T_ERROR ? token->msg : mk3(C, "Unsupported token (type: ", yaml_type_name(token->type), ")"));
    }
    if (!node)
        node = compose_empty_node(C, token->offset, 0, -1, pr);
    if (node && pr->anchor && node->anchor && !node->anchor[0])
        e_tok(C, pr->anchor, "Anchor cannot be an empty string");
    return node;
}

/* ---- documents and the composer ---- */

static ydoc *compose_doc(comp *C, cn *tok)
{
    ydoc *d = (ydoc *)ya_alloc(C->a, sizeof(ydoc));
    props pr;
    const cn *next;
    long contentEnd;
    if (!d) {
        C->oom = 1;
        return 0;
    }
    C->atKey = 0;
    C->atRoot = 1;
    next = tok->value ? tok->value : (tok->has_end && tok->end.n ? tok->end.v[0] : 0);
    resolve_props(C, &tok->start, 0, T_DOC_START, next, tok->offset, 0, 1, &pr);
    if (pr.found) {
        d->docStart = 1;
        if (tok->value && (tok->value->type == T_BLOCK_MAP || tok->value->type == T_BLOCK_SEQ) && !pr.hasNewline)
            e_off(C, pr.end, "Block collection cannot start on same line with directives-end marker");
    }
    d->contents = tok->value ? compose_node(C, tok->value, &pr) : compose_empty_node(C, pr.end, &tok->start, -1, &pr);
    contentEnd = d->contents ? d->contents->range[2] : pr.end;
    {
        long re = resolve_end(C, tok->has_end ? &tok->end : 0, contentEnd, 0);
        d->range[0] = tok->offset;
        d->range[1] = contentEnd;
        d->range[2] = re;
    }
    return d;
}

static void decorate(comp *C, ydoc *d, int afterDoc)
{
    int i;
    if (afterDoc) {
        for (i = 0; i < C->errors.n; i++)
            errs_add(C, &d->errors, C->errors.v[i].p0, C->errors.v[i].p1, C->errors.v[i].ulen, C->errors.v[i].msg);
    } else
        d->errors = C->errors;
    memset(&C->errors, 0, sizeof(C->errors));
}

/* JSON.stringify of a string */
static char *json_str(comp *C, const char *s, long n)
{
    sb b;
    long i;
    char *r;
    static const char hex[] = "0123456789abcdef";
    memset(&b, 0, sizeof(b));
    sb_ch(&b, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"')
            sb_add(&b, "\\\"", 2);
        else if (c == '\\')
            sb_add(&b, "\\\\", 2);
        else if (c == '\b')
            sb_add(&b, "\\b", 2);
        else if (c == '\f')
            sb_add(&b, "\\f", 2);
        else if (c == '\n')
            sb_add(&b, "\\n", 2);
        else if (c == '\r')
            sb_add(&b, "\\r", 2);
        else if (c == '\t')
            sb_add(&b, "\\t", 2);
        else if (c < 0x20) {
            sb_add(&b, "\\u00", 4);
            sb_ch(&b, hex[c >> 4]);
            sb_ch(&b, hex[c & 15]);
        } else
            sb_ch(&b, c);
    }
    sb_ch(&b, '"');
    r = mkn(C, b.p ? b.p : "", b.n);
    free(b.p);
    return r;
}

/* Directives.add */
static void directive_add(comp *C, const cn *tok)
{
    const char *s = tok->src;
    long n = tok->srclen, i = 0;
    char *parts[8];
    int np = 0, k;
    char *copy = mkn(C, s, n);
    char *p = copy;
    /* line.trim().split(/[ \t]+/) */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    {
        long len = (long)strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n' || p[len - 1] == '\r'))
            p[--len] = 0;
    }
    (void)i;
    while (*p && np < 8) {
        parts[np++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!*p)
            break;
        while (*p == ' ' || *p == '\t') {
            *p = 0;
            p++;
        }
    }
    if (np == 0)
        return;
    if (!strcmp(parts[0], "%TAG")) {
        int extra = np - 1;
        if (extra != 2) {
            e_rng(C, tok->offset, tok->offset + tok->srclen, "%TAG directive should contain exactly two parts");
            if (extra < 2)
                return;
        }
        for (k = 0; k < C->ntags; k++)
            if (!strcmp(C->thandle[k], parts[1])) {
                C->tprefix[k] = parts[2];
                return;
            }
        if (C->ntags < 8) {
            C->thandle[C->ntags] = parts[1];
            C->tprefix[C->ntags] = parts[2];
            C->ntags++;
        }
    } else if (!strcmp(parts[0], "%YAML")) {
        if (np - 1 != 1) {
            e_rng(C, tok->offset, tok->offset + tok->srclen, "%YAML directive should contain exactly one part");
            return;
        }
        if (strcmp(parts[1], "1.1") && strcmp(parts[1], "1.2")) {
            /* a warning when the version looks like \d+.\d+, else an error */
            const char *v = parts[1];
            int ok = 0, dots = 0, digits = 0;
            for (; *v; v++) {
                if (is_digit((unsigned char)*v))
                    digits++;
                else if (*v == '.' && digits && !dots) {
                    dots = 1;
                    digits = 0;
                } else {
                    ok = -1;
                    break;
                }
            }
            if (ok == 0 && dots && digits)
                return;
            e_rng(C, tok->offset + 6, tok->offset + tok->srclen, mk2(C, "Unsupported YAML version ", parts[1]));
        }
    }
}

/* the composer over the events: the documents it yields (at most two are kept) */
static void compose_events(comp *C, cn **ev, long nev, long endOffset)
{
    long i;
    for (i = 0; i < nev && !C->oom; i++) {
        cn *tok = ev[i];
        switch (tok->type) {
        case T_DIRECTIVE:
            directive_add(C, tok);
            C->atDirectives = 1;
            break;
        case T_DOCUMENT: {
            ydoc *d = compose_doc(C, tok);
            if (!d)
                break;
            if (C->atDirectives && !d->docStart)
                e_tok(C, tok, "Missing directives-end/doc-start indicator line");
            decorate(C, d, 0);
            if (C->doc && C->nyield < 2)
                C->yielded[C->nyield++] = C->doc;
            C->doc = d;
            C->atDirectives = 0;
            break;
        }
        case T_BOM:
        case T_SPACE:
        case T_COMMENT:
        case T_NEWLINE:
            break;
        case T_ERROR: {
            const char *msg = tok->has_src && tok->srclen > 0
                                  ? mk3(C, tok->msg, ": ", json_str(C, tok->src, tok->srclen))
                                  : tok->msg;
            if (C->atDirectives || !C->doc)
                errs_add(C, &C->errors, tok->offset, tok->offset + (tok->has_src ? tok->srclen : 1),
                         tok->has_src ? u16_len(tok->src, tok->srclen) : 1, msg);
            else
                errs_add(C, &C->doc->errors, tok->offset, tok->offset + (tok->has_src ? tok->srclen : 1),
                         tok->has_src ? u16_len(tok->src, tok->srclen) : 1, msg);
            break;
        }
        case T_DOC_END:
            if (!C->doc) {
                e_tok(C, tok, "Unexpected doc-end without preceding document");
                break;
            }
            resolve_end(C, tok->has_end ? &tok->end : 0, tok->offset + tok->srclen, 1);
            decorate(C, C->doc, 1);
            break;
        default:
            e_tok(C, tok, mk2(C, "Unsupported token ", yaml_type_name(tok->type)));
        }
        if (C->nyield >= 2)
            return;
    }
    if (C->nyield >= 2)
        return;
    if (C->doc) {
        decorate(C, C->doc, 1);
        C->yielded[C->nyield++] = C->doc;
        C->doc = 0;
    } else {
        ydoc *d = (ydoc *)ya_alloc(C->a, sizeof(ydoc));
        if (!d) {
            C->oom = 1;
            return;
        }
        if (C->atDirectives)
            e_off(C, endOffset, "Missing directives-end indicator line");
        d->range[0] = 0;
        d->range[1] = d->range[2] = endOffset;
        decorate(C, d, 0);
        C->yielded[C->nyield++] = d;
    }
}

/* ---- prettifyError ---- */

typedef struct u16s {
    unsigned short *v;
    long n, cap;
} u16s;

static void u16_add(u16s *u, unsigned short c)
{
    if (u->n == u->cap) {
        long nc = u->cap ? u->cap * 2 : 128;
        unsigned short *q = (unsigned short *)realloc(u->v, sizeof(unsigned short) * (size_t)nc);
        if (!q)
            return;
        u->v = q;
        u->cap = nc;
    }
    u->v[u->n++] = c;
}

static void u16_from_utf8(u16s *u, const char *s, long n)
{
    long i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned long cp;
        int len;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if (c >= 0xF0 && i + 3 < n + 0 && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((unsigned long)(c & 7) << 18) | ((unsigned long)(s[i + 1] & 0x3F) << 12) | ((unsigned long)(s[i + 2] & 0x3F) << 6) |
                 (unsigned long)(s[i + 3] & 0x3F);
            len = 4;
        } else if (c >= 0xE0 && c < 0xF0 && i + 2 < n && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((unsigned long)(c & 15) << 12) | ((unsigned long)(s[i + 1] & 0x3F) << 6) | (unsigned long)(s[i + 2] & 0x3F);
            len = 3;
        } else if (c >= 0xC0 && c < 0xE0 && i + 1 < n && (s[i + 1] & 0xC0) == 0x80) {
            cp = ((unsigned long)(c & 31) << 6) | (unsigned long)(s[i + 1] & 0x3F);
            len = 2;
        } else {
            cp = 0xFFFD;
            len = 1;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u16_add(u, (unsigned short)(0xD800 + (cp >> 10)));
            u16_add(u, (unsigned short)(0xDC00 + (cp & 0x3FF)));
        } else
            u16_add(u, (unsigned short)cp);
    }
}

static void u16_to_utf8(sb *b, const unsigned short *v, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        unsigned long cp = v[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && v[i + 1] >= 0xDC00 && v[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (v[i + 1] - 0xDC00);
            i++;
        }
        sb_utf8(b, cp);
    }
}

static void u16_cat(u16s *dst, const unsigned short *v, long n)
{
    long i;
    for (i = 0; i < n; i++)
        u16_add(dst, v[i]);
}

/* the 1-based line and column of the offset off (UTF-16 units), by the line starts ls[0..nls) (units) */
static void line_pos(const long *ls, long nls, long off, long *line, long *col)
{
    long low = 0, high = nls;
    while (low < high) {
        long mid = (low + high) >> 1;
        if (ls[mid] < off)
            low = mid + 1;
        else
            high = mid;
    }
    if (low < nls && ls[low] == off) {
        *line = low + 1;
        *col = 1;
        return;
    }
    if (low == 0) {
        *line = 0;
        *col = off;
        return;
    }
    *line = low;
    *col = off - ls[low - 1] + 1;
}

static char *prettify(comp *C, const yerr *e)
{
    const long *ls = C->ls;
    long nls = C->nls, line, col, line1, col1, ci, cnt;
    sb out;
    u16s ln, tmp;
    char *r;
    const char *src = C->src;
    long srclen = C->srclen;
    {
        long k, u0, u1, *lu = (long *)malloc(sizeof(long) * (size_t)(nls + 1));
        if (!lu)
            return e->msg;
        for (k = 0; k < nls; k++)
            lu[k] = u16_len(src, ls[k] > srclen ? srclen : ls[k]);
        u0 = u16_len(src, e->p0 > srclen ? srclen : e->p0);
        u1 = u0 + e->ulen;
        line_pos(lu, nls, u0, &line, &col);
        line_pos(lu, nls, u1, &line1, &col1);
        free(lu);
    }
    memset(&out, 0, sizeof(out));
    memset(&ln, 0, sizeof(ln));
    sb_add(&out, e->msg, (long)strlen(e->msg));
    {
        char nb[48];
        long v;
        int t = 0, k;
        sb_add(&out, " at line ", 9);
        v = line;
        if (!v)
            nb[t++] = '0';
        while (v > 0) {
            nb[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        for (k = t - 1; k >= 0; k--)
            sb_ch(&out, nb[k]);
        sb_add(&out, ", column ", 9);
        t = 0;
        v = col;
        if (!v)
            nb[t++] = '0';
        while (v > 0) {
            nb[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        for (k = t - 1; k >= 0; k--)
            sb_ch(&out, nb[k]);
    }
    ci = col - 1;
    if (line >= 1) {
        long a = ls[line - 1], b = line < nls ? ls[line] : srclen;
        while (b > a && (src[b - 1] == '\n' || src[b - 1] == '\r'))
            b--;
        u16_from_utf8(&ln, src + a, b - a);
        if (ci >= 60 && ln.n > 80) {
            long trimStart = ci - 39 < ln.n - 79 ? ci - 39 : ln.n - 79;
            memset(&tmp, 0, sizeof(tmp));
            u16_add(&tmp, 0x2026);
            u16_cat(&tmp, ln.v + trimStart, ln.n - trimStart);
            free(ln.v);
            ln = tmp;
            ci -= trimStart - 1;
        }
        if (ln.n > 80) {
            memset(&tmp, 0, sizeof(tmp));
            u16_cat(&tmp, ln.v, 79);
            u16_add(&tmp, 0x2026);
            free(ln.v);
            ln = tmp;
        }
        if (line > 1) {
            long k;
            int blank = 1;
            for (k = 0; k < ci && k < ln.n; k++)
                if (ln.v[k] != ' ')
                    blank = 0;
            if (blank) {
                long pa = ls[line - 2], pb = ls[line - 1];
                u16s prev;
                memset(&prev, 0, sizeof(prev));
                u16_from_utf8(&prev, src + pa, pb - pa);
                if (prev.n > 80) {
                    u16s p2;
                    memset(&p2, 0, sizeof(p2));
                    u16_cat(&p2, prev.v, 79);
                    u16_add(&p2, 0x2026);
                    u16_add(&p2, '\n');
                    free(prev.v);
                    prev = p2;
                }
                memset(&tmp, 0, sizeof(tmp));
                u16_cat(&tmp, prev.v, prev.n);
                u16_cat(&tmp, ln.v, ln.n);
                free(prev.v);
                free(ln.v);
                ln = tmp;
            }
        }
        {
            int nonblank = 0;
            long k;
            for (k = 0; k < ln.n; k++)
                if (ln.v[k] != ' ')
                    nonblank = 1;
            if (nonblank) {
                cnt = 1;
                if (line1 == line && col1 > col) {
                    long d = col1 - col, lim = 80 - ci;
                    cnt = d < lim ? d : lim;
                    if (cnt < 1)
                        cnt = 1;
                }
                sb_add(&out, ":\n\n", 3);
                u16_to_utf8(&out, ln.v, ln.n);
                sb_ch(&out, '\n');
                for (k = 0; k < ci; k++)
                    sb_ch(&out, ' ');
                for (k = 0; k < cnt; k++)
                    sb_ch(&out, '^');
                sb_ch(&out, '\n');
            }
        }
    }
    free(ln.v);
    r = mkn(C, out.p ? out.p : "", out.n);
    free(out.p);
    return r;
}

/* ---- Document.toJS: aliases, and what the top-level keys are ---- */

typedef struct walk {
    ynode **seen;
    long nseen, capseen;
    const char *bad;            /* name of the first alias with no anchor */
    int oom;
    int depth;
} walk;

static void walk_node(comp *C, walk *W, ynode *nd)
{
    int i;
    if (!nd || W->bad || W->oom)
        return;
    if (W->depth > Y_DEPTH)
        return;
    if (nd->kind == YN_ALIAS) {
        long k;
        nd->target = 0;
        for (k = W->nseen - 1; k >= 0; k--)
            if (W->seen[k]->anchor && !strcmp(W->seen[k]->anchor, nd->alias)) {
                nd->target = W->seen[k];
                break;
            }
        if (!nd->target)
            W->bad = nd->alias;
        return;
    }
    if (nd->anchor && nd->anchor[0]) {
        if (W->nseen == W->capseen) {
            long nc = W->capseen ? W->capseen * 2 : 16;
            ynode **q = (ynode **)realloc(W->seen, sizeof(ynode *) * (size_t)nc);
            if (!q) {
                W->oom = 1;
                return;
            }
            W->seen = q;
            W->capseen = nc;
        }
        W->seen[W->nseen++] = nd;
    }
    W->depth++;
    if (nd->kind == YN_MAP)
        for (i = 0; i < nd->n; i++) {
            walk_node(C, W, nd->pairs[i].key);
            walk_node(C, W, nd->pairs[i].val);
        }
    else if (nd->kind == YN_SEQ)
        for (i = 0; i < nd->n; i++)
            walk_node(C, W, nd->items[i]);
    W->depth--;
}

static ynode *deref(ynode *nd)
{
    int guard = 0;
    while (nd && nd->kind == YN_ALIAS && nd->target && guard++ < 64)
        nd = nd->target;
    return nd;
}

/* Run the parse: 0 ok (doc out), -1 oom, -2 error (message in err) */
static int run_parse(const char *text, long n, ya *a, comp *C, ydoc **docp, char *err, long errcap)
{
    lexemes lx;
    cevents ev;
    int rc;
    memset(C, 0, sizeof(*C));
    C->a = a;
    C->src = text;
    C->srclen = n;
    C->thandle[0] = (char *)"!!";
    C->tprefix[0] = (char *)YAML_TAG;
    C->ntags = 1;
    memset(&lx, 0, sizeof(lx));
    memset(&ev, 0, sizeof(ev));
    rc = yaml_lex(a, text, n, &lx);
    if (!rc)
        rc = yaml_cst(a, &lx, &ev);
    if (rc) {
        free(lx.v);
        free(ev.v);
        return -1;
    }
    C->ls = ev.nl;
    C->nls = ev.nnl;
    compose_events(C, ev.v, ev.n, n);
    free(lx.v);
    if (C->oom || a->oom)
        return -1;
    if (C->nyield < 1)
        return -1;
    {
        ydoc *d = C->yielded[0];
        if (C->nyield > 1) {
            ydoc *d2 = C->yielded[1];
            errs_add(C, &d->errors, d2->range[0], d2->range[1], u16_len(C->src + d2->range[0], d2->range[1] - d2->range[0]),
                     "Source contains multiple documents; please use YAML.parseAllDocuments()");
        }
        *docp = d;
        if (d->errors.n > 0) {
            char *m = prettify(C, &d->errors.v[0]);
            if (errcap > 0) {
                long l = (long)strlen(m);
                if (l >= errcap)
                    l = errcap - 1;
                memcpy(err, m, (size_t)l);
                err[l] = 0;
            }
            return -2;
        }
    }
    return 0;
}

int pi_yaml_error(const char *text, long n, char *err, long errcap)
{
    ya a;
    comp C;
    ydoc *d = 0;
    int rc;
    memset(&a, 0, sizeof(a));
    if (errcap > 0)
        err[0] = 0;
    rc = run_parse(text, n, &a, &C, &d, err, errcap);
    if (rc == 0) {
        walk W;
        memset(&W, 0, sizeof(W));
        walk_node(&C, &W, d->contents);
        if (W.bad) {
            char *m = mk2(&C, "Unresolved alias (the anchor must be set before the alias): ", W.bad);
            long l = (long)strlen(m);
            if (errcap > 0) {
                if (l >= errcap)
                    l = errcap - 1;
                memcpy(err, m, (size_t)l);
                err[l] = 0;
            }
            rc = -2;
        }
        free(W.seen);
    }
    ya_free(&a);
    return rc == -2 ? 1 : rc;
}

int pi_yaml_keys(const char *text, long n, const char *const *keys, int nkeys, pi_yv *vals, char *err, long errcap)
{
    ya a;
    comp C;
    ydoc *d = 0;
    int rc, i;
    memset(&a, 0, sizeof(a));
    for (i = 0; i < nkeys; i++) {
        vals[i].kind = YV_NONE;
        vals[i].s = 0;
    }
    if (errcap > 0)
        err[0] = 0;
    rc = run_parse(text, n, &a, &C, &d, err, errcap);
    if (rc == 0) {
        walk W;
        ynode *root;
        memset(&W, 0, sizeof(W));
        walk_node(&C, &W, d->contents);
        if (W.oom)
            rc = -1;
        else if (W.bad) {
            char *m = mk2(&C, "Unresolved alias (the anchor must be set before the alias): ", W.bad);
            long l = (long)strlen(m);
            if (errcap > 0) {
                if (l >= errcap)
                    l = errcap - 1;
                memcpy(err, m, (size_t)l);
                err[l] = 0;
            }
            rc = -2;
        } else {
            root = deref(d->contents);
            if (root && root->kind == YN_MAP) {
                int k;
                for (k = 0; k < root->n && rc == 0; k++) {
                    ynode *kn = deref(root->pairs[k].key), *vn = deref(root->pairs[k].val);
                    if (!kn || kn->kind != YN_SCALAR || kn->vk != VK_STR)
                        continue;
                    for (i = 0; i < nkeys; i++)
                        if ((long)strlen(keys[i]) == kn->strn && !memcmp(keys[i], kn->str, (size_t)kn->strn)) {
                            free(vals[i].s);
                            vals[i].s = 0;
                            if (!vn) {
                                vals[i].kind = YV_OTHER;
                            } else if (vn->kind == YN_SCALAR && vn->vk == VK_STR) {
                                vals[i].s = (char *)malloc((size_t)vn->strn + 1);
                                if (!vals[i].s) {
                                    rc = -1;
                                    break;
                                }
                                memcpy(vals[i].s, vn->str, (size_t)vn->strn);
                                vals[i].s[vn->strn] = 0;
                                vals[i].kind = YV_STR;
                            } else if (vn->kind == YN_SCALAR && vn->vk == VK_BOOL && vn->b)
                                vals[i].kind = YV_TRUE;
                            else
                                vals[i].kind = YV_OTHER;
                        }
                }
            }
        }
        free(W.seen);
    }
    ya_free(&a);
    if (rc) {
        pi_yv_free(vals, nkeys);
        for (i = 0; i < nkeys; i++) {
            vals[i].kind = YV_NONE;
            vals[i].s = 0;
        }
    }
    return rc;
}

void pi_yv_free(pi_yv *vals, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        free(vals[i].s);
        vals[i].s = 0;
    }
}
