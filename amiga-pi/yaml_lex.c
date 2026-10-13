/* yaml_lex.c -- the lexer of the yaml package (yaml 2.9.0 parse/lexer.js), run over a complete text: setNext() and the
 * incomplete-input branches never happen, so they are not here. Positions are byte offsets; the characters the lexer tells
 * apart are ASCII, so UTF-8 text passes through. See yaml_int.h. */
#include <stdlib.h>
#include <string.h>
#include "yaml_int.h"

/* ---- arena ---- */

void *ya_alloc(ya *a, size_t n)
{
    ya_blk *b = (ya_blk *)calloc(1, sizeof(ya_blk) + n + 8);
    if (!b) {
        a->oom = 1;
        return 0;
    }
    b->next = a->head;
    a->head = b;
    return (char *)b + sizeof(ya_blk);
}

char *ya_dup(ya *a, const char *s, long n)
{
    char *p = (char *)ya_alloc(a, (size_t)n + 1);
    if (p) {
        if (n > 0)
            memcpy(p, s, (size_t)n);
        p[n] = 0;
    }
    return p;
}

void ya_free(ya *a)
{
    while (a->head) {
        ya_blk *b = a->head;
        a->head = b->next;
        free(b);
    }
}

/* ---- the lexer ---- */

enum { S_END = 0, S_STREAM, S_LINE_START, S_BLOCK_START, S_DOC, S_FLOW, S_QUOTED, S_BLOCK_SCALAR, S_PLAIN };

typedef struct lex {
    ya *a;
    const char *buf;
    long blen, pos;
    int flowKey, flowLevel;
    long indentNext, indentValue;
    int blockScalarIndent, blockScalarKeep;
    long lineEndPos;
    int lineEndSet;
    lexemes *out;
    int oom;
} lex;

static const char DOC_MARK[] = "\002";
static const char FLOW_END_MARK[] = "\030";
static const char SCALAR_MARK[] = "\037";

/* buffer[i] as JavaScript reads it: -1 for undefined */
static int at(const lex *L, long i)
{
    return i >= 0 && i < L->blen ? (unsigned char)L->buf[i] : -1;
}

static int is_empty(int ch)
{
    return ch == -1 || ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

static int is_flow_ind(int ch)
{
    return ch == ',' || ch == '[' || ch == ']' || ch == '{' || ch == '}';
}

static int not_anchor(int ch)
{
    return ch == -1 || ch == ' ' || ch == ',' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '\n' || ch == '\r' ||
           ch == '\t';
}

static int block_hdr_end(int ch)
{
    return is_empty(ch) || ch == '#';
}

static int is_hex(int ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
}

static void emit(lex *L, const char *p, long n)
{
    lexemes *o = L->out;
    if (o->n == o->cap) {
        long nc = o->cap ? o->cap * 2 : 256;
        lexeme *q = (lexeme *)realloc(o->v, sizeof(lexeme) * (size_t)nc);
        if (!q) {
            L->oom = 1;
            return;
        }
        o->v = q;
        o->cap = nc;
    }
    o->v[o->n].p = p;
    o->v[o->n].n = n;
    o->n++;
}

/* buffer.indexOf(c, from) */
static long index_of(const lex *L, int c, long from)
{
    long i;
    if (from < 0)
        from = 0;
    for (i = from; i < L->blen; i++)
        if ((unsigned char)L->buf[i] == c)
            return i;
    return -1;
}

static int has_chars(const lex *L, long n)
{
    return L->pos + n <= L->blen;
}

/* getLine(): the rest of the current line, without a CR before the LF; its length (the text starts at pos) */
static long get_line(lex *L)
{
    long end = L->lineEndPos;
    if (!L->lineEndSet || (end != -1 && end < L->pos)) {
        end = index_of(L, '\n', L->pos);
        L->lineEndPos = end;
        L->lineEndSet = 1;
    }
    if (end == -1)
        return L->blen - L->pos;
    if (end >= 1 && L->buf[end - 1] == '\r')
        end -= 1;
    return end - L->pos;
}

static int at_line_end(const lex *L)
{
    long i = L->pos;
    int ch = at(L, i);
    while (ch == ' ' || ch == '\t')
        ch = at(L, ++i);
    if (ch == -1 || ch == '#' || ch == '\n')
        return 1;
    if (ch == '\r')
        return at(L, i + 1) == '\n';
    return 0;
}

static int char_at(const lex *L, long n)
{
    return at(L, L->pos + n);
}

static long continue_scalar(const lex *L, long offset)
{
    int ch = at(L, offset);
    if (L->indentNext > 0) {
        long indent = 0;
        while (ch == ' ')
            ch = at(L, ++indent + offset);
        if (ch == '\r') {
            int next = at(L, indent + offset + 1);
            if (next == '\n')
                return offset + indent + 1;
        }
        return ch == '\n' || indent >= L->indentNext ? offset + indent : -1;
    }
    if (ch == '-' || ch == '.') {
        if (offset + 3 <= L->blen && !memcmp(L->buf + offset, ch == '-' ? "---" : "...", 3) && is_empty(at(L, offset + 3)))
            return -1;
    }
    return offset;
}

static long push_count(lex *L, long n)
{
    if (n > 0) {
        long avail = L->blen - L->pos;
        emit(L, L->buf + L->pos, n < avail ? n : avail);
        L->pos += n;
        return n;
    }
    return 0;
}

static long push_to_index(lex *L, long i, int allow_empty)
{
    long end = i > L->blen ? L->blen : i;
    if (end > L->pos) {
        long n = end - L->pos;
        emit(L, L->buf + L->pos, n);
        L->pos += n;
        return n;
    }
    if (allow_empty)
        emit(L, L->buf + L->pos, 0);
    return 0;
}

static long push_spaces(lex *L, int allow_tabs)
{
    long i = L->pos - 1, n;
    int ch;
    do {
        ch = at(L, ++i);
    } while (ch == ' ' || (allow_tabs && ch == '\t'));
    n = i - L->pos;
    if (n > 0) {
        emit(L, L->buf + L->pos, n);
        L->pos = i;
    }
    return n;
}

static long push_until(lex *L, int (*test)(int))
{
    long i = L->pos;
    int ch = at(L, i);
    while (!test(ch))
        ch = at(L, ++i);
    return push_to_index(L, i, 0);
}

static long push_newline(lex *L)
{
    int ch = at(L, L->pos);
    if (ch == '\n')
        return push_count(L, 1);
    if (ch == '\r' && char_at(L, 1) == '\n')
        return push_count(L, 2);
    return 0;
}

static long push_tag(lex *L)
{
    static const char tagchars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-#;/?:@&=+$_.!~*'()";
    if (char_at(L, 1) == '<') {
        long i = L->pos + 2;
        int ch = at(L, i);
        while (!is_empty(ch) && ch != '>')
            ch = at(L, ++i);
        return push_to_index(L, ch == '>' ? i + 1 : i, 0);
    } else {
        long i = L->pos + 1;
        int ch = at(L, i);
        while (ch != -1) {
            if (ch != 0 && strchr(tagchars, ch))
                ch = at(L, ++i);
            else if (ch == '%' && is_hex(at(L, i + 1)) && is_hex(at(L, i + 2))) {
                i += 3;
                ch = at(L, i);
            } else
                break;
        }
        return push_to_index(L, i, 0);
    }
}

static long push_indicators(lex *L)
{
    long n = 0;
    for (;;) {
        int c = char_at(L, 0);
        if (c == '!') {
            n += push_tag(L);
            n += push_spaces(L, 1);
            continue;
        }
        if (c == '&') {
            n += push_until(L, not_anchor);
            n += push_spaces(L, 1);
            continue;
        }
        if (c == '-' || c == '?' || c == ':') {
            int in_flow = L->flowLevel > 0, ch1 = char_at(L, 1);
            if (is_empty(ch1) || (in_flow && is_flow_ind(ch1))) {
                if (!in_flow)
                    L->indentNext = L->indentValue + 1;
                else if (L->flowKey)
                    L->flowKey = 0;
                n += push_count(L, 1);
                n += push_spaces(L, 1);
                continue;
            }
        }
        break;
    }
    return n;
}

static int parse_block_start(lex *L);
static int parse_block_scalar(lex *L);
static int parse_plain_scalar(lex *L);

static int parse_line_start(lex *L)
{
    int ch = char_at(L, 0);
    if (ch == '-' || ch == '.') {
        if (has_chars(L, 3) && !memcmp(L->buf + L->pos, ch == '-' ? "---" : "...", 3) && is_empty(char_at(L, 3))) {
            push_count(L, 3);
            L->indentValue = 0;
            L->indentNext = 0;
            return ch == '-' ? S_DOC : S_STREAM;
        }
    }
    L->indentValue = push_spaces(L, 0);
    {
        /* charAt(1) is the next character, which may take several bytes of UTF-8 text */
        long k = L->pos + 1;
        int c0 = char_at(L, 0);
        if (c0 >= 0xC0)
            while (k < L->blen && ((unsigned char)L->buf[k] & 0xC0) == 0x80)
                k++;
        if (L->indentNext > L->indentValue && !is_empty(at(L, k)))
            L->indentNext = L->indentValue;
    }
    return parse_block_start(L);
}

static int parse_block_start(lex *L)
{
    int ch0 = char_at(L, 0), ch1 = char_at(L, 1);
    if ((ch0 == '-' || ch0 == '?' || ch0 == ':') && is_empty(ch1)) {
        long n = push_count(L, 1) + push_spaces(L, 1);
        L->indentNext = L->indentValue + 1;
        L->indentValue += n;
        return S_BLOCK_START;
    }
    return S_DOC;
}

static int parse_stream(lex *L)
{
    long ll = get_line(L), skip = 0;
    const char *line = L->buf + L->pos;
    if (ll >= 3 && !memcmp(line, "\357\273\277", 3)) {
        push_count(L, 3);
        skip = 3;
    }
    if (ll - skip > 0 && line[skip] == '%') {
        const char *l2 = line + skip;
        long l2n = ll - skip, dir_end = l2n, k = 0, n;
        for (;;) {
            long cs = -1, j;
            int ch;
            for (j = k; j < l2n; j++)
                if (l2[j] == '#') {
                    cs = j;
                    break;
                }
            if (cs == -1)
                break;
            ch = cs >= 1 ? (unsigned char)l2[cs - 1] : -1;
            if (ch == ' ' || ch == '\t') {
                dir_end = cs - 1;
                break;
            }
            k = cs + 1;
        }
        for (;;) {
            int ch = dir_end >= 1 ? (unsigned char)l2[dir_end - 1] : -1;
            if (ch == ' ' || ch == '\t')
                dir_end -= 1;
            else
                break;
        }
        n = push_count(L, dir_end) + push_spaces(L, 1);
        push_count(L, l2n - n);
        /* (yaml calls pushNewline() here without yield*: nothing is pushed) */
        return S_STREAM;
    }
    if (at_line_end(L)) {
        long sp = push_spaces(L, 1);
        push_count(L, (ll - skip) - sp);
        push_newline(L);
        return S_STREAM;
    }
    emit(L, DOC_MARK, 1);
    return parse_line_start(L);
}

static int parse_quoted_scalar(lex *L)
{
    int quote = char_at(L, 0);
    long end = index_of(L, quote, L->pos + 1), nl;
    if (quote == '\'') {
        while (end != -1 && at(L, end + 1) == '\'')
            end = index_of(L, '\'', end + 2);
    } else {
        while (end != -1) {
            long n = 0;
            while (at(L, end - 1 - n) == '\\')
                n += 1;
            if (n % 2 == 0)
                break;
            end = index_of(L, '"', end + 1);
        }
    }
    /* only the text between the quotes is searched for line breaks (qb = buffer.substring(0, end): empty when there is no closing quote) */
    nl = -1;
    if (end != -1) {
        long k;
        for (k = L->pos; k < end; k++)
            if (L->buf[k] == '\n') {
                nl = k;
                break;
            }
    }
    if (nl != -1) {
        while (nl != -1) {
            long cs = continue_scalar(L, nl + 1), k;
            if (cs == -1)
                break;
            nl = -1;
            for (k = cs < 0 ? 0 : cs; k < end; k++)
                if (L->buf[k] == '\n') {
                    nl = k;
                    break;
                }
        }
        if (nl != -1)
            end = nl - (at(L, nl - 1) == '\r' ? 2 : 1);
    }
    if (end == -1)
        end = L->blen;
    push_to_index(L, end + 1, 0);
    return L->flowLevel ? S_FLOW : S_DOC;
}

static long parse_block_scalar_header(lex *L)
{
    long i = L->pos;
    L->blockScalarIndent = -1;
    L->blockScalarKeep = 0;
    for (;;) {
        int ch = at(L, ++i);
        if (ch == '+')
            L->blockScalarKeep = 1;
        else if (ch >= '1' && ch <= '9')
            L->blockScalarIndent = ch - '0' - 1;
        else if (ch != '-')
            break;
    }
    return push_until(L, block_hdr_end);
}

static int parse_document(lex *L)
{
    long ll, n, line0;
    int c;
    push_spaces(L, 1);
    ll = get_line(L);
    line0 = L->pos;
    n = push_indicators(L);
    c = n < ll ? (unsigned char)L->buf[line0 + n] : -1;
    switch (c) {
    case '#':
        push_count(L, ll - n);
        /* fallthrough */
    case -1:
        push_newline(L);
        return parse_line_start(L);
    case '{':
    case '[':
        push_count(L, 1);
        L->flowKey = 0;
        L->flowLevel = 1;
        return S_FLOW;
    case '}':
    case ']':
        push_count(L, 1);
        return S_DOC;
    case '*':
        push_until(L, not_anchor);
        return S_DOC;
    case '"':
    case '\'':
        return parse_quoted_scalar(L);
    case '|':
    case '>':
        n += parse_block_scalar_header(L);
        n += push_spaces(L, 1);
        push_count(L, ll - n);
        push_newline(L);
        return parse_block_scalar(L);
    default:
        return parse_plain_scalar(L);
    }
}

static int parse_plain_scalar(lex *L)
{
    int in_flow = L->flowLevel > 0, ch;
    long end = L->pos - 1, i = L->pos - 1;
    while ((ch = at(L, ++i)) != -1) {
        if (ch == ':') {
            int next = at(L, i + 1);
            if (is_empty(next) || (in_flow && is_flow_ind(next)))
                break;
            end = i;
        } else if (is_empty(ch)) {
            int next = at(L, i + 1);
            if (ch == '\r') {
                if (next == '\n') {
                    i += 1;
                    ch = '\n';
                    next = at(L, i + 1);
                } else
                    end = i;
            }
            if (next == '#' || (in_flow && is_flow_ind(next)))
                break;
            if (ch == '\n') {
                long cs = continue_scalar(L, i + 1);
                if (cs == -1)
                    break;
                i = i > cs - 2 ? i : cs - 2;
            }
        } else {
            if (in_flow && is_flow_ind(ch))
                break;
            end = i;
        }
    }
    emit(L, SCALAR_MARK, 1);
    push_to_index(L, end + 1, 1);
    return in_flow ? S_FLOW : S_DOC;
}

static int parse_flow_collection(lex *L)
{
    long nl, sp, indent = -1, ll, line0, n;
    int c;
    do {
        nl = push_newline(L);
        if (nl > 0) {
            sp = push_spaces(L, 0);
            L->indentValue = indent = sp;
        } else
            sp = 0;
        sp += push_spaces(L, 1);
    } while (nl + sp > 0);
    ll = get_line(L);
    line0 = L->pos;
    if ((indent != -1 && indent < L->indentNext && !(ll > 0 && L->buf[line0] == '#')) ||
        (indent == 0 && ll >= 3 && (!memcmp(L->buf + line0, "---", 3) || !memcmp(L->buf + line0, "...", 3)) &&
         (ll < 4 ? 1 : is_empty((unsigned char)L->buf[line0 + 3])))) {
        int at_end_marker = indent == L->indentNext - 1 && L->flowLevel == 1 && ll > 0 &&
                            (L->buf[line0] == ']' || L->buf[line0] == '}');
        if (!at_end_marker) {
            L->flowLevel = 0;
            emit(L, FLOW_END_MARK, 1);
            return parse_line_start(L);
        }
    }
    n = 0;
    while (n < ll && L->buf[line0 + n] == ',') {
        n += push_count(L, 1);
        n += push_spaces(L, 1);
        L->flowKey = 0;
    }
    n += push_indicators(L);
    c = n < ll ? (unsigned char)L->buf[line0 + n] : -1;
    switch (c) {
    case -1:
        return S_FLOW;
    case '#':
        push_count(L, ll - n);
        return S_FLOW;
    case '{':
    case '[':
        push_count(L, 1);
        L->flowKey = 0;
        L->flowLevel += 1;
        return S_FLOW;
    case '}':
    case ']':
        push_count(L, 1);
        L->flowKey = 1;
        L->flowLevel -= 1;
        return L->flowLevel ? S_FLOW : S_DOC;
    case '*':
        push_until(L, not_anchor);
        return S_FLOW;
    case '"':
    case '\'':
        L->flowKey = 1;
        return parse_quoted_scalar(L);
    case ':': {
        int next = char_at(L, 1);
        if (L->flowKey || is_empty(next) || next == ',') {
            L->flowKey = 0;
            push_count(L, 1);
            push_spaces(L, 1);
            return S_FLOW;
        }
    }
        /* fallthrough */
    default:
        L->flowKey = 0;
        return parse_plain_scalar(L);
    }
}

static int parse_block_scalar(lex *L)
{
    long nl = L->pos - 1, indent = 0, i;
    int ch;
    for (i = L->pos; (ch = at(L, i)) != -1; ++i) {
        if (ch == ' ')
            indent += 1;
        else if (ch == '\n') {
            nl = i;
            indent = 0;
        } else if (ch == '\r' && at(L, i + 1) == '\n') {
            /* a CR before an LF: skipped */
        } else
            break;
    }
    if (indent >= L->indentNext) {
        if (L->blockScalarIndent == -1)
            L->indentNext = indent;
        else
            L->indentNext = L->blockScalarIndent + (L->indentNext == 0 ? 1 : L->indentNext);
        do {
            long cs = continue_scalar(L, nl + 1);
            if (cs == -1)
                break;
            nl = index_of(L, '\n', cs);
        } while (nl != -1);
        if (nl == -1)
            nl = L->blen;
    }
    /* trailing insufficiently indented tabs are part of the scalar (so that the composer can refuse them) */
    i = nl + 1;
    ch = at(L, i);
    while (ch == ' ')
        ch = at(L, ++i);
    if (ch == '\t') {
        while (ch == '\t' || ch == ' ' || ch == '\r' || ch == '\n')
            ch = at(L, ++i);
        nl = i - 1;
    } else if (!L->blockScalarKeep) {
        for (;;) {
            long j = nl - 1, last;
            int c2 = at(L, j);
            if (c2 == '\r')
                c2 = at(L, --j);
            last = j;
            while (c2 == ' ')
                c2 = at(L, --j);
            if (c2 == '\n' && j >= L->pos && j + 1 + indent > last)
                nl = j;
            else
                break;
        }
    }
    emit(L, SCALAR_MARK, 1);
    push_to_index(L, nl + 1, 1);
    return parse_line_start(L);
}

static int parse_next(lex *L, int next)
{
    switch (next) {
    case S_STREAM:
        return parse_stream(L);
    case S_LINE_START:
        return parse_line_start(L);
    case S_BLOCK_START:
        return parse_block_start(L);
    case S_DOC:
        return parse_document(L);
    case S_FLOW:
        return parse_flow_collection(L);
    case S_QUOTED:
        return parse_quoted_scalar(L);
    case S_BLOCK_SCALAR:
        return parse_block_scalar(L);
    case S_PLAIN:
        return parse_plain_scalar(L);
    default:
        return S_END;
    }
}

int yaml_lex(ya *a, const char *buf, long blen, lexemes *out)
{
    lex L;
    int next = S_STREAM;
    long guard = 0;
    memset(&L, 0, sizeof(L));
    L.a = a;
    L.buf = buf;
    L.blen = blen;
    L.flowKey = 0;
    L.blockScalarIndent = -1;
    L.out = out;
    out->v = 0;
    out->n = out->cap = 0;
    while (next && has_chars(&L, 1) && !L.oom) {
        long before = L.pos;
        next = parse_next(&L, next);
        if (L.pos == before && ++guard > 8)
            break;              /* no progress: yaml's lexer would not stop here either; the text cannot be read */
        if (L.pos != before)
            guard = 0;
    }
    return L.oom ? -1 : 0;
}

/* cst.tokenType */
int yaml_token_type(const char *s, long n)
{
    if (n == 3 && !memcmp(s, "\357\273\277", 3))
        return T_BOM;
    if (n == 1) {
        switch (s[0]) {
        case '\002': return T_DOC_MODE;
        case '\030': return T_FLOW_ERROR_END;
        case '\037': return T_SCALAR;
        case '\n': return T_NEWLINE;
        case '-': return T_SEQ_IND;
        case '?': return T_EXPL_KEY;
        case ':': return T_MAP_VAL;
        case '{': return T_FLOW_MAP_START;
        case '}': return T_FLOW_MAP_END;
        case '[': return T_FLOW_SEQ_START;
        case ']': return T_FLOW_SEQ_END;
        case ',': return T_COMMA;
        default: break;
        }
    }
    if (n == 3 && !memcmp(s, "---", 3))
        return T_DOC_START;
    if (n == 3 && !memcmp(s, "...", 3))
        return T_DOC_END;
    if (n == 0 || (n == 2 && s[0] == '\r' && s[1] == '\n'))
        return T_NEWLINE;
    switch (s[0]) {
    case ' ':
    case '\t': return T_SPACE;
    case '#': return T_COMMENT;
    case '%': return T_DIRECTIVE_LINE;
    case '*': return T_ALIAS;
    case '&': return T_ANCHOR;
    case '!': return T_TAG;
    case '\'': return T_SQ_SCALAR;
    case '"': return T_DQ_SCALAR;
    case '|':
    case '>': return T_BLOCK_HDR;
    default: break;
    }
    return T_NONE;
}

const char *yaml_type_name(int type)
{
    switch (type) {
    case T_BOM: return "byte-order-mark";
    case T_DOC_MODE: return "doc-mode";
    case T_FLOW_ERROR_END: return "flow-error-end";
    case T_SCALAR: return "scalar";
    case T_DOC_START: return "doc-start";
    case T_DOC_END: return "doc-end";
    case T_NEWLINE: return "newline";
    case T_SEQ_IND: return "seq-item-ind";
    case T_EXPL_KEY: return "explicit-key-ind";
    case T_MAP_VAL: return "map-value-ind";
    case T_FLOW_MAP_START: return "flow-map-start";
    case T_FLOW_MAP_END: return "flow-map-end";
    case T_FLOW_SEQ_START: return "flow-seq-start";
    case T_FLOW_SEQ_END: return "flow-seq-end";
    case T_COMMA: return "comma";
    case T_SPACE: return "space";
    case T_COMMENT: return "comment";
    case T_DIRECTIVE_LINE: return "directive-line";
    case T_ALIAS: return "alias";
    case T_ANCHOR: return "anchor";
    case T_TAG: return "tag";
    case T_SQ_SCALAR: return "single-quoted-scalar";
    case T_DQ_SCALAR: return "double-quoted-scalar";
    case T_BLOCK_HDR: return "block-scalar-header";
    case T_DOCUMENT: return "document";
    case T_BLOCK_MAP: return "block-map";
    case T_BLOCK_SEQ: return "block-seq";
    case T_FLOW_COLL: return "flow-collection";
    case T_BLOCK_SCALAR: return "block-scalar";
    case T_ERROR: return "error";
    case T_DIRECTIVE: return "directive";
    default: return "";
    }
}
