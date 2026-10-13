/* see settings_diag.h */
#include <stdlib.h>
#include <string.h>
#include "settings_diag.h"
#include "claude/util.h"

typedef struct sd {
    unsigned short *u;          /* the text as UTF-16 code units */
    long n;
    long pos;
    int err;                    /* the error is in msg */
    jw *msg;
} sd;

/* V8's text for a position: " in JSON at position N (line L column C)" */
static void at(sd *p, long pos, int in_json)
{
    long line = 1, last = 0, i;
    char b[24];
    for (i = 0; i < pos && i < p->n; i++) {
        unsigned short c = p->u[i];
        if (c == '\r' && i + 1 < p->n && p->u[i + 1] == '\n')
            continue;               /* \r\n is one line break, counted at the \n */
        if (c == '\n' || c == '\r') {
            line++;
            last = i + 1;
        }
    }
    jw_rawz(p->msg, in_json ? " in JSON at position " : " at position ");
    cl_ltoa(pos, b);
    jw_rawz(p->msg, b);
    jw_rawz(p->msg, " (line ");
    cl_ltoa(line, b);
    jw_rawz(p->msg, b);
    jw_rawz(p->msg, " column ");
    cl_ltoa(pos - last + 1, b);
    jw_rawz(p->msg, b);
    jw_rawz(p->msg, ")");
}

/* units [from, to) as UTF-8 */
static void put_units(jw *o, const unsigned short *u, long from, long to)
{
    long i;
    for (i = from; i < to; i++) {
        unsigned long cp = u[i];
        char b[4];
        int k;
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < to && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000) {
            cp = 0x10000UL + ((cp - 0xD800) << 10) + (unsigned long)(u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            b[0] = (char)cp;
            k = 1;
        } else if (cp < 0x800) {
            b[0] = (char)(0xC0 | (cp >> 6));
            b[1] = (char)(0x80 | (cp & 0x3F));
            k = 2;
        } else if (cp < 0x10000UL) {
            b[0] = (char)(0xE0 | (cp >> 12));
            b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
            b[2] = (char)(0x80 | (cp & 0x3F));
            k = 3;
        } else {
            b[0] = (char)(0xF0 | (cp >> 18));
            b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
            b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
            b[3] = (char)(0x80 | (cp & 0x3F));
            k = 4;
        }
        jw_raw(o, b, k);
    }
}

/* a message with a position: "<what> in JSON at position ..." */
static int fail_at(sd *p, const char *what, long pos)
{
    if (!p->err) {
        jw_rawz(p->msg, what);
        at(p, pos, !strstr(what, "after JSON"));         /* "... after JSON at position", the others "... in JSON at position" */
        p->err = 1;
    }
    return 0;
}

/* the character at pos is not what the grammar wants here and no message was given for the place: V8's LookUpErrorMessageForJsonToken */
static int unexpected(sd *p)
{
    long pos = p->pos, len = p->n;
    unsigned short c;
    if (p->err)
        return 0;
    if (pos >= len) {
        jw_rawz(p->msg, "Unexpected end of JSON input");
        p->err = 1;
        return 0;
    }
    c = p->u[pos];
    if (c == '"') {
        jw_rawz(p->msg, "Unexpected string");
        at(p, pos, 1);
        p->err = 1;
        return 0;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        jw_rawz(p->msg, "Unexpected number");
        at(p, pos, 1);
        p->err = 1;
        return 0;
    }
    /* the sources V8 names whole: "undefined" is not valid JSON */
    {
        static const char *const special[] = { "NaN", "Infinity", "undefined", "[object Object]" };
        int k;
        for (k = 0; k < 4; k++) {
            long sl = (long)strlen(special[k]), i;
            if (sl != len)
                continue;
            for (i = 0; i < sl && p->u[i] == (unsigned char)special[k][i]; i++)
                ;
            if (i == sl) {
                jw_rawz(p->msg, "\"");
                jw_rawz(p->msg, special[k]);
                jw_rawz(p->msg, "\" is not valid JSON");
                p->err = 1;
                return 0;
            }
        }
    }
    jw_rawz(p->msg, "Unexpected token '");
    put_units(p->msg, p->u, pos, pos + ((c >= 0xD800 && c < 0xDC00 && pos + 1 < len) ? 2 : 1));
    jw_rawz(p->msg, "', ");
    if (len < 21) {                                 /* kMinOriginalSourceLengthForContext: the whole source */
        jw_rawz(p->msg, "\"");
        put_units(p->msg, p->u, 0, len);
        jw_rawz(p->msg, "\"");
    } else if (pos < 10) {                          /* the start of it, ten units on */
        jw_rawz(p->msg, "\"");
        put_units(p->msg, p->u, 0, pos + 10);
        jw_rawz(p->msg, "\"...");
    } else if (pos < len - 10) {                    /* ten units either side */
        jw_rawz(p->msg, "...\"");
        put_units(p->msg, p->u, pos - 10, pos + 10);
        jw_rawz(p->msg, "\"...");
    } else {                                        /* the end of it, from ten units before */
        jw_rawz(p->msg, "...\"");
        put_units(p->msg, p->u, pos - 10, len);
        jw_rawz(p->msg, "\"");
    }
    jw_rawz(p->msg, " is not valid JSON");
    p->err = 1;
    return 0;
}

static void ws(sd *p)
{
    while (p->pos < p->n && (p->u[p->pos] == ' ' || p->u[p->pos] == '\t' || p->u[p->pos] == '\n' || p->u[p->pos] == '\r'))
        p->pos++;
}

static int is_digit(unsigned short c)
{
    return c >= '0' && c <= '9';
}

/* the string at pos (a '"'): to after its closing quote */
static int string(sd *p)
{
    p->pos++;
    for (;;) {
        unsigned short c;
        if (p->pos >= p->n)
            return fail_at(p, "Unterminated string", p->n);
        c = p->u[p->pos];
        if (c == '"') {
            p->pos++;
            return 1;
        }
        if (c < 0x20)
            return fail_at(p, "Bad control character in string literal", p->pos);
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->n)
                return unexpected(p);           /* a backslash that ends the text: "Unexpected end of JSON input" */
            c = p->u[p->pos];
            if (c == 'u') {
                int k;
                for (k = 1; k <= 4; k++) {
                    unsigned short h = p->pos + k < p->n ? p->u[p->pos + k] : 0;
                    if (!(is_digit(h) || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
                        return fail_at(p, "Bad Unicode escape", p->pos + k);
                }
                p->pos += 5;
                continue;
            }
            if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' || c == 'n' || c == 'r' || c == 't') {
                p->pos++;
                continue;
            }
            return fail_at(p, "Bad escaped character", p->pos);
        }
        p->pos++;
    }
}

static int number(sd *p)
{
    if (p->u[p->pos] == '-') {
        p->pos++;
        if (p->pos >= p->n || !is_digit(p->u[p->pos]))
            return fail_at(p, "No number after minus sign", p->pos);
    }
    if (p->u[p->pos] == '0') {
        p->pos++;
        if (p->pos < p->n && is_digit(p->u[p->pos]))
            return unexpected(p);
    } else
        while (p->pos < p->n && is_digit(p->u[p->pos]))
            p->pos++;
    if (p->pos < p->n && p->u[p->pos] == '.') {
        p->pos++;
        if (p->pos >= p->n || !is_digit(p->u[p->pos]))
            return fail_at(p, "Unterminated fractional number", p->pos);
        while (p->pos < p->n && is_digit(p->u[p->pos]))
            p->pos++;
    }
    if (p->pos < p->n && (p->u[p->pos] == 'e' || p->u[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->n && (p->u[p->pos] == '+' || p->u[p->pos] == '-'))
            p->pos++;
        if (p->pos >= p->n || !is_digit(p->u[p->pos]))
            return fail_at(p, "Exponent part is missing a number", p->pos);
        while (p->pos < p->n && is_digit(p->u[p->pos]))
            p->pos++;
    }
    return 1;
}

static int literal(sd *p, const char *word)
{
    for (; *word; word++, p->pos++)
        if (p->pos >= p->n || p->u[p->pos] != (unsigned char)*word)
            return unexpected(p);
    return 1;
}

static int value(sd *p, int depth)
{
    unsigned short c;
    if (p->pos >= p->n)
        return unexpected(p);
    c = p->u[p->pos];
    if (c == '"')
        return string(p);
    if (c == '-' || is_digit(c))
        return number(p);
    if (c == 't')
        return literal(p, "true");
    if (c == 'f')
        return literal(p, "false");
    if (c == 'n')
        return literal(p, "null");
    if (c == '{') {
        p->pos++;
        ws(p);
        if (p->pos < p->n && p->u[p->pos] == '}') {
            p->pos++;
            return 1;
        }
        if (p->pos >= p->n || p->u[p->pos] != '"')
            return fail_at(p, "Expected property name or '}'", p->pos);
        for (;;) {
            if (!string(p))
                return 0;
            ws(p);
            if (p->pos >= p->n || p->u[p->pos] != ':')
                return fail_at(p, "Expected ':' after property name", p->pos);
            p->pos++;
            ws(p);
            if (!value(p, depth + 1))
                return 0;
            ws(p);
            if (p->pos < p->n && p->u[p->pos] == '}') {
                p->pos++;
                return 1;
            }
            if (p->pos >= p->n || p->u[p->pos] != ',')
                return fail_at(p, "Expected ',' or '}' after property value", p->pos);
            p->pos++;
            ws(p);
            if (p->pos >= p->n || p->u[p->pos] != '"')
                return fail_at(p, "Expected double-quoted property name", p->pos);
        }
    }
    if (c == '[') {
        p->pos++;
        ws(p);
        if (p->pos < p->n && p->u[p->pos] == ']') {
            p->pos++;
            return 1;
        }
        for (;;) {
            if (!value(p, depth + 1))
                return 0;
            ws(p);
            if (p->pos < p->n && p->u[p->pos] == ']') {
                p->pos++;
                return 1;
            }
            if (p->pos >= p->n || p->u[p->pos] != ',')
                return fail_at(p, "Expected ',' or ']' after array element", p->pos);
            p->pos++;
            ws(p);
        }
    }
    return unexpected(p);
}

/* UTF-8 to UTF-16 code units (an invalid byte is U+FFFD, as Node's decoder makes it) */
static long to_units(const char *s, long n, unsigned short *u)
{
    long i = 0, k = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned long cp = 0xFFFD;
        int more = 0, j;
        if (c < 0x80)
            cp = c;
        else if (c >= 0xC2 && c < 0xE0) {
            cp = c & 0x1F;
            more = 1;
        } else if (c >= 0xE0 && c < 0xF0) {
            cp = c & 0x0F;
            more = 2;
        } else if (c >= 0xF0 && c < 0xF5) {
            cp = c & 0x07;
            more = 3;
        }
        for (j = 1; j <= more; j++) {
            if (i + j >= n || ((unsigned char)s[i + j] & 0xC0) != 0x80) {
                cp = 0xFFFD;
                more = j - 1;
                break;
            }
            cp = (cp << 6) | ((unsigned char)s[i + j] & 0x3F);
        }
        i += 1 + more;
        if (cp >= 0x10000UL && cp <= 0x10FFFFUL) {
            u[k++] = (unsigned short)(0xD800 + ((cp - 0x10000UL) >> 10));
            u[k++] = (unsigned short)(0xDC00 + ((cp - 0x10000UL) & 0x3FF));
        } else
            u[k++] = (unsigned short)(cp > 0xFFFF ? 0xFFFD : cp);
    }
    return k;
}

int pi_settings_error(const char *text, long n, jw *msg)
{
    sd p;
    int rc = 0;
    long first;
    if (n <= 0)
        return 0;                               /* `if (!content) return {}` */
    if (n >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;                              /* stripBom */
        n -= 3;
    }
    memset(&p, 0, sizeof(p));
    p.u = (unsigned short *)malloc((size_t)n * 2 + 2);
    if (!p.u)
        return 0;
    p.n = to_units(text, n, p.u);
    p.msg = msg;
    first = msg->n;
    ws(&p);
    if (value(&p, 0)) {
        ws(&p);
        if (p.pos < p.n)
            fail_at(&p, "Unexpected non-whitespace character after JSON", p.pos);
        else {
            /* JSON.parse took it; migrateSettings asks `"queueMode" in settings`, a TypeError for a primitive */
            unsigned short c = p.u[0];
            long i = 0;
            while (i < p.n && (p.u[i] == ' ' || p.u[i] == '\t' || p.u[i] == '\n' || p.u[i] == '\r'))
                i++;
            c = p.u[i];
            if (c != '{' && c != '[') {
                long e = p.n;
                while (e > i && (p.u[e - 1] == ' ' || p.u[e - 1] == '\t' || p.u[e - 1] == '\n' || p.u[e - 1] == '\r'))
                    e--;
                jw_rawz(msg, "Cannot use 'in' operator to search for 'queueMode' in ");
                if (c == '"')
                    put_units(msg, p.u, i + 1, e - 1);  /* a string is shown without its quotes (its escapes are not undone here) */
                else
                    put_units(msg, p.u, i, e);
                p.err = 1;
            }
        }
    }
    rc = p.err;
    if (!rc)
        msg->n = first;
    free(p.u);
    return rc;
}

void pi_settings_warning(const char *path, const char *message, jw *out)
{
    jw_rawz(out, "Invalid settings file ");
    jw_rawz(out, path);
    jw_rawz(out, ": ");
    jw_rawz(out, message);
}
