/* see show_pi.h */
#include <stdlib.h>
#include <string.h>
#include "show_pi.h"
#include "ansi.h"
#include "claude/util.h"
#include "claude/json.h"
#include "claude/show.h"
#include "agent.h"

#define SGR0 "\033[0m"
#define BOLD "\033[1m"
#define DIM  "\033[2m"

/* outputPad (settings.json, pi 1.1.0 settings-manager.ts:173,1511): the horizontal padding of messages, tool boxes and
 * `!` output, 0 or 1 column each side. The ONE place the padding is read: pd is the padding string, pwid the width
 * left for text */
static const char *pd(const pi_show *s)
{
    return s->pad ? " " : "";
}

static long pwid(const pi_show *s)
{
    return s->t->cols - 2 * (long)s->pad;
}

/* the SGR of a role into buf */
static const char *sg(const pi_show *s, int role, char *buf)
{
    pitheme_sgr(s->th, role, buf);
    return buf;
}

static void raw(pi_show *s, const char *z)
{
    jw_rawz(&s->batch, z);
}

static void out(pi_show *s)
{
    if (s->batch.n) {
        tui_lines(s->t, s->batch.p, s->batch.n);
        jw_reset(&s->batch);
    }
}

/* a blank line */
static void blank(pi_show *s)
{
    raw(s, "\n");
}

/* the blank line between two blocks of the transcript (not above the first one) */
static void spacer(pi_show *s)
{
    if (s->any)
        blank(s);
    s->any = 1;
}

/* text set in a role's colour (plus extra attributes), wrapped to the screen's width less pi's one column of
 * padding each side, each line one space in */
static void say_pad(pi_show *s, int pad, int role, const char *attr, const char *text)
{
    char b[48], st[96];
    cl_copy(st, role >= 0 ? sg(s, role, b) : "", sizeof(st));
    cl_cat(st, attr, sizeof(st));
    show_wrap(&s->batch, pad ? " " : "", pad ? " " : "", st, text, (long)strlen(text), s->t->cols - 2 * (long)pad);
}

/* a block that follows outputPad (messages, tool boxes) */
static void say(pi_show *s, int role, const char *attr, const char *text)
{
    say_pad(s, s->pad, role, attr, text);
}

/* the rows of the batch from `from` on become a panel, pi's Box: the background of role bg behind every row over
 * the whole width, put back after each reset inside the row. Nothing changes when the theme draws no background
 * there (pi's system theme). The ONE place the panels' backgrounds are drawn; the colours are the theme's tokens. */
static void panel(pi_show *s, long from, int bgrole)
{
    char bg[48], *tmp;
    long n, a = 0, i;
    pitheme_sgr(s->th, bgrole, bg);
    if (!bg[0] || s->batch.n <= from)
        return;
    n = s->batch.n - from;
    tmp = (char *)malloc((size_t)n + 1);
    if (!tmp)
        return;
    memcpy(tmp, s->batch.p + from, (size_t)n);
    s->batch.n = from;
    s->batch.p[from] = 0;
    for (i = 0; i < n; i++) {
        long j, w, k;
        if (tmp[i] != '\n')
            continue;
        w = tui_width(tmp + a, i - a);
        jw_rawz(&s->batch, bg);
        for (j = a; j < i; j++) {
            if (tmp[j] == '\033' && j + 3 < i && tmp[j + 1] == '[' && tmp[j + 2] == '0' && tmp[j + 3] == 'm') {
                jw_rawz(&s->batch, SGR0);
                jw_rawz(&s->batch, bg);
                j += 3;
            } else
                jw_raw(&s->batch, tmp + j, 1);
        }
        for (k = w; k < s->t->cols; k++)
            jw_rawz(&s->batch, " ");
        jw_rawz(&s->batch, SGR0 "\n");
        a = i + 1;
    }
    free(tmp);
}

void pshow_theme(pi_show *s, const cl_pitheme *th)
{
    s->th = th;
    pitheme_hl(th, &s->hlt);
}

void pshow_init(pi_show *s, cl_tui *t, const cl_pitheme *th)
{
    memset(s, 0, sizeof(*s));
    s->t = t;
    s->pad = 1;
    jw_init(&s->line);
    jw_init(&s->batch);
    jw_init(&s->args);
    pshow_theme(s, th);
}

void pshow_free(pi_show *s)
{
    if (s->md)
        md_close(s->md);
    s->md = 0;
    jw_free(&s->line);
    jw_free(&s->batch);
    jw_free(&s->args);
}

/* ---- the answer ---- */

static void md_sink(void *u, const char *p, long n)
{
    pi_show *s = (pi_show *)u;
    long a = 0, i;
    for (i = 0; i < n; i++) {
        if (p[i] != '\n')
            continue;
        jw_raw(&s->line, p + a, i - a);
        if (s->first) {
            if (!tui_width(s->line.p ? s->line.p : "", s->line.n)) {
                jw_reset(&s->line);
                a = i + 1;
                continue;
            }
            spacer(s);              /* the answer starts a blank line below what is above it */
            s->first = 0;
            s->n_blocks++;
        }
        raw(s, SGR0);
        raw(s, pd(s));
        jw_raw(&s->batch, s->line.p ? s->line.p : "", s->line.n);
        raw(s, SGR0 "\n");
        jw_reset(&s->line);
        a = i + 1;
    }
    jw_raw(&s->line, p + a, n - a);
}

static void md_start(pi_show *s)
{
    md_opts mo;
    if (s->md)
        return;
    vo_init(&s->vo, md_sink, s, VW_UTF8, s->th->depth >= 256 ? s->th->depth : 16, &s->hlt);
    mo.width = pwid(s);
    mo.urls = 1;
    mo.look = MD_LOOK_PI;
    s->md = md_open(&mo, &s->vo);
    s->first = 1;
    jw_reset(&s->line);
}

static void r_text(void *u, const char *p, long n)
{
    pi_show *s = (pi_show *)u;
    s->n_text++;
    md_start(s);
    if (s->md)
        md_feed(s->md, p, n);
    vo_flush(&s->vo);
    out(s);
}

static void r_end(void *u)
{
    pi_show *s = (pi_show *)u;
    if (!s->md)
        return;
    md_close(s->md);
    s->md = 0;
    vo_flush(&s->vo);
    if (s->line.n)
        md_sink(s, "\n", 1);
    out(s);
}

void pshow_render(pi_show *s, cl_render *r)
{
    r->u = s;
    r->text = r_text;
    r->end = r_end;
}

/* ---- the program's own lines ---- */

void pshow_header(pi_show *s, const char *name, const char *version)
{
    char c174[48], c67[48], b67[48], c179[48], dim[48], l[400];
    cl_pitheme logo = *s->th;
    logo.depth = 256;               /* pi's logo names three colours of the 256 palette: sent as they are */
    pitheme_color(&logo, 174, 0, c174);
    pitheme_color(&logo, 67, 0, c67);
    pitheme_color(&logo, 67, 1, b67);
    pitheme_color(&logo, 179, 0, c179);
    sg(s, PR_DIM, dim);
    blank(s);
    s->any = 1;
    /* pi's logo, two rows of half blocks, and beside it what the program is and the keys */
    cl_copy(l, " ", sizeof(l));
    cl_cat(l, c174, sizeof(l));
    cl_cat(l, b67, sizeof(l));
    cl_cat(l, "\342\226\200\033[49m\342\226\200\342\226\210\033[39m  ", sizeof(l));
    cl_cat(l, dim, sizeof(l));
    cl_cat(l, name, sizeof(l));
    cl_cat(l, " ", sizeof(l));
    cl_cat(l, version, sizeof(l));
    cl_cat(l, SGR0, sizeof(l));
    raw(s, l);
    raw(s, "\n");
    cl_copy(l, " ", sizeof(l));
    cl_cat(l, c67, sizeof(l));
    cl_cat(l, "\342\226\210\342\226\200\033[39m ", sizeof(l));
    cl_cat(l, c179, sizeof(l));
    cl_cat(l, "\342\226\210\033[39m ", sizeof(l));
    raw(s, l);
    {
        /* the hint continues the logo's row and wraps in the width as pi's text does (one column of padding each
         * side; the logo and its space take five of the first row). The keys are in the dim colour, what they do and
         * the dots in the muted one (pi's keyHint): \001 starts a key, \002 what it does */
        static const char hint[] =
            "\001escape\002 interrupt \302\267 \001ctrl+c/ctrl+d\002 clear/exit \302\267 \001/\002 commands \302\267 "
            "\001!\002 bash \302\267 \001ctrl+o\002 more";
        char muted[48], key[48];
        long n = (long)sizeof(hint) - 1, cut = 0, i, k;
        int w = 0, room = s->t->cols - 2 - 5, row;
        sg(s, PR_MUTED, muted);
        sg(s, PR_DIM, key);
        for (i = 0; i < n;) {
            unsigned long cp;
            int cl, cw;
            if (hint[i] == 1 || hint[i] == 2) {
                i++;
                continue;
            }
            cl = vw_char(hint + i, n - i, &cp);
            cw = vw_cp_width(cp);
            if (w + cw > room)
                break;
            if (cp == ' ')
                cut = i;
            w += cw;
            i += cl;
        }
        if (i >= n)
            cut = n;
        else if (!cut)
            cut = i;
        for (row = 0; row < 2; row++) {
            long from = row ? cut : 0, to = row ? n : cut;
            if (row && cut >= n)
                break;
            if (row) {
                while (from < to && hint[from] == ' ')
                    from++;
                raw(s, " ");
                for (k = from - 1; k >= 0; k--)
                    if (hint[k] == 1 || hint[k] == 2) {
                        raw(s, hint[k] == 1 ? key : muted);
                        break;
                    }
            }
            for (k = from; k < to; k++) {
                if (hint[k] == 1)
                    raw(s, key);
                else if (hint[k] == 2)
                    raw(s, muted);
                else
                    jw_raw(&s->batch, hint + k, 1);
            }
            raw(s, SGR0 "\n");
        }
    }
    out(s);
}

void pshow_user(pi_show *s, const char *text)
{
    long from;
    spacer(s);
    from = s->batch.n;
    blank(s);                       /* the message's padding row, above and below */
    say(s, PR_USER_MESSAGE_TEXT, "", text);
    blank(s);
    panel(s, from, PR_USER_MESSAGE_BG);
    out(s);
}

void pshow_note(pi_show *s, int role, const char *text)
{
    spacer(s);
    say_pad(s, 1, role, "", text);     /* pi's status lines are Text(.., 1, 0), not outputPad */
    out(s);
}

void pshow_error(pi_show *s, const char *text)
{
    spacer(s);
    say(s, PR_ERROR, "", text);
    out(s);
}

/* ---- tool calls ---- */

/* pi's getTextOutput (core/tools/render-utils.ts:39-48): the text blocks of a result, each cleaned of its escape sequences, binary
 * garbage and carriage returns (stripAnsi, sanitizeBinaryOutput; ansi.c), joined by newlines. The bash, read, ls, find and
 * grep renderers all show this, and executeBash (!cmd) keeps the same text. */
long pshow_result_text(const char *json, long n, jw *o)
{
    jv c, b, t, x;
    jit it;
    long start = o->n;
    int any = 0;
    if (json_parse(json, n, &c) || json_type(c) != J_ARR)
        return 0;
    json_iter(c, &it);
    while (json_next(&it, 0, &b))
        if (json_type(b) == J_OBJ && json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x) &&
            json_type(x) == J_STR) {
            long l;
            char *z = json_strdup(x, &l);
            if (z) {
                if (any)
                    jw_raw(o, "\n", 1);
                pi_ansi_clean(z, l, o);
                any = 1;
            }
            free(z);
        }
    return o->n - start;
}

static int arg_str(jv a, const char *key, char *out, long cap)
{
    jv v;
    out[0] = 0;
    if (!json_get(a, key, &v) || json_type(v) != J_STR)
        return 0;
    json_str(v, out, cap);
    return 1;
}

static void header_line(pi_show *s, const char *name, jv a)
{
    char b[48], b2[48], path[300], tmp[300], l[700];
    cl_copy(l, pd(s), sizeof(l));
    if (!strcmp(name, "bash")) {
        jv v;
        arg_str(a, "command", tmp, sizeof(tmp));
        cl_cat(l, BOLD, sizeof(l));
        cl_cat(l, sg(s, PR_TOOL_TITLE, b2), sizeof(l));      /* pi: bold, then toolTitle */
        cl_cat(l, "$ ", sizeof(l));
        cl_cat(l, tmp[0] ? tmp : "...", sizeof(l));
        cl_cat(l, SGR0, sizeof(l));
        if (json_get(a, "timeout", &v) && json_type(v) == J_NUM && json_long(v, 0) > 0) {
            char n[16];
            cl_cat(l, sg(s, PR_MUTED, b), sizeof(l));
            cl_cat(l, " (timeout ", sizeof(l));
            cl_ltoa(json_long(v, 0), n);
            cl_cat(l, n, sizeof(l));
            cl_cat(l, "s)", sizeof(l));
            cl_cat(l, SGR0, sizeof(l));
        }
    } else if (!strcmp(name, "grep")) {
        char pat[200], glob[100];
        jv v;
        arg_str(a, "pattern", pat, sizeof(pat));
        if (!arg_str(a, "path", path, sizeof(path)) || !path[0])
            cl_copy(path, ".", sizeof(path));
        arg_str(a, "glob", glob, sizeof(glob));
        cl_cat(l, BOLD, sizeof(l));
        cl_cat(l, sg(s, PR_TOOL_TITLE, b2), sizeof(l));
        cl_cat(l, "grep" SGR0 " ", sizeof(l));
        cl_cat(l, sg(s, PR_ACCENT, b), sizeof(l));
        cl_cat(l, "/", sizeof(l));
        cl_cat(l, pat, sizeof(l));
        cl_cat(l, "/" SGR0, sizeof(l));
        cl_cat(l, sg(s, PR_TOOL_OUTPUT, b), sizeof(l));
        cl_cat(l, " in ", sizeof(l));
        cl_cat(l, path, sizeof(l));
        if (glob[0]) {
            cl_cat(l, " (", sizeof(l));
            cl_cat(l, glob, sizeof(l));
            cl_cat(l, ")", sizeof(l));
        }
        if (json_get(a, "limit", &v) && json_type(v) == J_NUM) {
            char n[16];
            cl_cat(l, " limit ", sizeof(l));
            cl_ltoa(json_long(v, 0), n);
            cl_cat(l, n, sizeof(l));
        }
        cl_cat(l, SGR0, sizeof(l));
    } else if (!strcmp(name, "find") || !strcmp(name, "ls")) {
        char pat[200];
        jv v;
        int is_find = name[0] == 'f';
        arg_str(a, "pattern", pat, sizeof(pat));
        if (!arg_str(a, "path", path, sizeof(path)) || !path[0])
            cl_copy(path, ".", sizeof(path));
        cl_cat(l, BOLD, sizeof(l));
        cl_cat(l, sg(s, PR_TOOL_TITLE, b2), sizeof(l));      /* pi: bold, then toolTitle */
        cl_cat(l, name, sizeof(l));
        cl_cat(l, SGR0 " ", sizeof(l));
        if (is_find) {
            cl_cat(l, sg(s, PR_ACCENT, b), sizeof(l));
            cl_cat(l, pat, sizeof(l));
            cl_cat(l, SGR0, sizeof(l));
            cl_cat(l, sg(s, PR_TOOL_OUTPUT, b), sizeof(l));
            cl_cat(l, " in ", sizeof(l));
            cl_cat(l, path, sizeof(l));
        } else {
            cl_cat(l, sg(s, PR_ACCENT, b), sizeof(l));
            cl_cat(l, path, sizeof(l));
            cl_cat(l, SGR0, sizeof(l));
            cl_cat(l, sg(s, PR_TOOL_OUTPUT, b), sizeof(l));
        }
        if (json_get(a, "limit", &v) && json_type(v) == J_NUM) {
            char n[16];
            cl_cat(l, " (limit ", sizeof(l));
            cl_ltoa(json_long(v, 0), n);
            cl_cat(l, n, sizeof(l));
            cl_cat(l, ")", sizeof(l));
        }
        cl_cat(l, SGR0, sizeof(l));
    } else {
        /* read, write, edit and anything else: the name in bold, the path in the accent colour */
        jv v;
        if (!arg_str(a, "path", path, sizeof(path)))
            arg_str(a, "file_path", path, sizeof(path));
        cl_cat(l, BOLD, sizeof(l));
        cl_cat(l, sg(s, PR_TOOL_TITLE, b2), sizeof(l));      /* pi: bold, then toolTitle */
        cl_cat(l, name, sizeof(l));
        cl_cat(l, SGR0 " ", sizeof(l));
        if (path[0]) {
            cl_cat(l, sg(s, PR_ACCENT, b), sizeof(l));
            cl_cat(l, path, sizeof(l));
            cl_cat(l, SGR0, sizeof(l));
        } else {
            cl_cat(l, sg(s, PR_TOOL_OUTPUT, b2), sizeof(l));
            cl_cat(l, "...", sizeof(l));
            cl_cat(l, SGR0, sizeof(l));
        }
        if (!strcmp(name, "read") && (json_get(a, "offset", &v) || json_get(a, "limit", &v))) {
            jv o, li;
            long off = json_get(a, "offset", &o) && json_type(o) == J_NUM ? json_long(o, 1) : 1;
            char n[16];
            cl_cat(l, sg(s, PR_WARNING, b), sizeof(l));
            cl_cat(l, ":", sizeof(l));
            cl_ltoa(off, n);
            cl_cat(l, n, sizeof(l));
            if (json_get(a, "limit", &li) && json_type(li) == J_NUM) {
                cl_cat(l, "-", sizeof(l));
                cl_ltoa(off + json_long(li, 1) - 1, n);
                cl_cat(l, n, sizeof(l));
            }
            cl_cat(l, SGR0, sizeof(l));
        }
    }
    raw(s, l);
    raw(s, "\n");
}

void pshow_tool_start(pi_show *s, const char *name, const char *args, long an)
{
    jv a;
    s->n_tools++;
    cl_copy(s->name, name, sizeof(s->name));
    jw_reset(&s->args);
    jw_raw(&s->args, args, an);
    jw_rawz(&s->args, "");
    s->t0 = s->t->io->ms ? s->t->io->ms(s->t->io->u) : 0;
    if (json_parse(s->args.p, s->args.n, &a))
        a.p = 0, a.n = 0;
    spacer(s);                      /* the block is a blank line below what is above it, with one of its own inside */
    out(s);
    s->box_mark = s->t->n_lines;
    blank(s);
    header_line(s, name, a);
    blank(s);
    panel(s, 0, PR_TOOL_PENDING_BG);    /* pi's box while the tool runs; the end draws it again in its final colour */
    out(s);
    s->box_end = s->t->n_lines;
}

/* the duration as pi writes it */
static void duration(unsigned long ms, char *out)
{
    unsigned long tenths = (ms + 50) / 100;
    char n[16];
    if (tenths < 600) {
        cl_ltoa((long)(tenths / 10), out);
        cl_cat(out, ".", 24);
        cl_ltoa((long)(tenths % 10), n);
        cl_cat(out, n, 24);
        cl_cat(out, "s", 24);
        return;
    } else {
        unsigned long total = ms / 1000, min = total / 60, rem = total % 60;
        out[0] = 0;
        if (min >= 60) {
            cl_ltoa((long)(min / 60), n);
            cl_cat(out, n, 24);
            cl_cat(out, "h ", 24);
            min %= 60;
        }
        cl_ltoa((long)min, n);
        cl_cat(out, n, 24);
        cl_cat(out, "m ", 24);
        cl_ltoa((long)rem, n);
        cl_cat(out, n, 24);
        cl_cat(out, "s", 24);
    }
}

/* the lines of text (split at newlines, trailing blank lines cut), each one space in, at most max shown from the
 * start; the rest as "... (N more lines, ctrl+o to expand)" */
static void result_lines(pi_show *s, const char *text, int role, int max)
{
    long n = (long)strlen(text), a = 0, shown = 0, total = 0, i;
    char b[48], m[96], num[16];
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' '))
        n--;
    for (i = 0; i <= n; i++)
        if (i == n || text[i] == '\n')
            total++;
    blank(s);
    for (i = 0; i <= n && shown < max; i++) {
        if (i == n || text[i] == '\n') {
            show_wrap(&s->batch, pd(s), pd(s), sg(s, role, b), text + a, i - a, pwid(s));
            a = i + 1;
            shown++;
        }
    }
    if (total > max) {
        cl_copy(m, "... (", sizeof(m));
        cl_ltoa(total - max, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " more lines, ctrl+o to expand)", sizeof(m));
        show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_MUTED, b), m, (long)strlen(m), pwid(s));
    }
}

/* bash: the last five lines of the output, "... (N earlier lines, ctrl+o to expand)" above when more */
static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded)
{
    char b[48], dur[24], m[96], num[16];
    jw all;
    long n = (long)strlen(text), a = 0, i, rows = 0;
    long first_row[256];
    int nrows = 0, skip;
    jw_init(&all);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r'))
        n--;
    while (a < n && (text[a] == '\n' || text[a] == ' ' || text[a] == '\t'))
        a++;
    if (a < n) {
        /* the output wrapped to the screen's width: its rows ("visual lines"), the last five kept */
        jw wrapped;
        long p = 0;
        jw_init(&wrapped);
        show_wrap(&wrapped, "", "", "", text + a, n - a, pwid(s));
        for (i = 0; i < wrapped.n && nrows < 255; i++)
            if (i == 0 || wrapped.p[i - 1] == '\n') {
                first_row[nrows++] = i;
                (void)p;
            }
        first_row[nrows] = wrapped.n;
        skip = nrows > 5 ? nrows - 5 : 0;
        blank(s);
        if (skip) {
            cl_copy(m, "... (", sizeof(m));
            cl_ltoa(skip, num);
            cl_cat(m, num, sizeof(m));
            cl_cat(m, " earlier lines, ctrl+o to expand)", sizeof(m));
            show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_MUTED, b), m, (long)strlen(m), pwid(s));
        }
        for (i = skip; i < nrows; i++) {
            /* each row of wrapped holds its text and SGR0 \n: re-set in the output colour */
            long e = first_row[i + 1] - 1;
            jw_rawz(&s->batch, pd(s));
            jw_rawz(&s->batch, sg(s, PR_TOOL_OUTPUT, b));
            jw_raw(&s->batch, wrapped.p + first_row[i], e - first_row[i]);
            jw_rawz(&s->batch, "\n");
            rows++;
        }
        jw_free(&wrapped);
    }
    jw_free(&all);
    /* pi 1.1.0 renderers/bash.ts:104-111: a final result's recorded durationMs wins (it survives a reload); the clock of
     * the call is the fallback while the tool runs and for results recorded without one, and a replayed result with
     * none shows no Took */
    if (recorded >= 0)
        ms = (unsigned long)recorded;
    else if (s->replay)
        return;
    duration(ms, dur);
    blank(s);
    cl_copy(m, "Took ", sizeof(m));
    cl_cat(m, dur, sizeof(m));
    show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_MUTED, b), m, (long)strlen(m), pwid(s));
    (void)rows;
}

void pshow_tool_end(pi_show *s, const char *result, long rn, int is_error, long duration_ms)
{
    jw text;
    jv a;
    long hdr_end, hdr_rows;
    unsigned long ms = (s->t->io->ms ? s->t->io->ms(s->t->io->u) : 0) - s->t0;
    out(s);
    if (json_parse(s->args.p, s->args.n, &a))
        a.p = 0, a.n = 0;
    blank(s);                       /* the box again: its top padding and the header ... */
    header_line(s, s->name, a);
    for (hdr_rows = 0, hdr_end = 0; hdr_end < s->batch.n; hdr_end++)
        hdr_rows += s->batch.p[hdr_end] == '\n';
    jw_init(&text);
    pshow_result_text(result, rn, &text);
    jw_rawz(&text, "");
    if (!strcmp(s->name, "bash"))
        bash_result(s, text.p ? text.p : "", ms, duration_ms);
    else if (is_error && text.n) {
        blank(s);
        say(s, PR_ERROR, "", text.p);
    } else if (!strcmp(s->name, "ls") || !strcmp(s->name, "find"))
        result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 20);
    else if (!strcmp(s->name, "grep"))
        result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 15);
    blank(s);                       /* the block's bottom padding */
    jw_free(&text);
    panel(s, 0, is_error ? PR_TOOL_ERROR_BG : PR_TOOL_SUCCESS_BG);
    if (s->box_end == s->t->n_lines && s->box_end > s->box_mark) {
        /* nothing came after the pending box: it is replaced by the finished one */
        tui_replace_tail(s->t, s->box_mark, s->batch.p, s->batch.n);
        jw_reset(&s->batch);
    } else {
        /* something is under it (a second tool): the result goes below, the header keeps its colour */
        long skip = 0, at = 0;
        while (skip < hdr_rows && at < s->batch.n) {
            if (s->batch.p[at] == '\n')
                skip++;
            at++;
        }
        tui_lines(s->t, s->batch.p + at, s->batch.n - at);
        jw_reset(&s->batch);
    }
    s->box_end = s->box_mark = 0;
}

/* ---- the conversation again ---- */

typedef struct pcall {
    char id[80], name[24];
    jw args;
    int shown;
} pcall;

static void flush_calls(pi_show *s, pcall *c, int *n)
{
    int i;
    for (i = 0; i < *n; i++) {
        if (!c[i].shown)
            pshow_tool_start(s, c[i].name, c[i].args.p ? c[i].args.p : "{}", c[i].args.n);
        jw_free(&c[i].args);
    }
    *n = 0;
}

void pshow_replay(pi_show *s, const struct pi_session *ses)
{
    pcall calls[PI_TOOLS];
    int ncall = 0, i;
    cl_render r;
    s->replay = 1;
    s->any = 0;
    pshow_render(s, &r);
    for (i = 0; i < ses->nmsg; i++) {
        jv m, v, c, b, t, x;
        jit it;
        char role[24];
        if (json_parse(ses->msg[i], ses->msg_n[i], &m) || !json_get(m, "role", &v))
            continue;
        json_str(v, role, sizeof(role));
        if (strcmp(role, "toolResult"))
            flush_calls(s, calls, &ncall);
        if (!strcmp(role, "user")) {
            jw w;
            jw_init(&w);
            if (json_get(m, "content", &c)) {
                if (json_type(c) == J_STR) {
                    long n;
                    char *z = json_strdup(c, &n);
                    if (z)
                        jw_raw(&w, z, n);
                    free(z);
                } else if (json_type(c) == J_ARR) {
                    json_iter(c, &it);
                    while (json_next(&it, 0, &b))
                        if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                            long n;
                            char *z = json_strdup(x, &n);
                            if (z)
                                jw_raw(&w, z, n);
                            free(z);
                        }
                }
            }
            jw_rawz(&w, "");
            pshow_user(s, w.p);
            jw_free(&w);
        } else if (!strcmp(role, "assistant") && json_get(m, "content", &c) && json_type(c) == J_ARR) {
            json_iter(c, &it);
            while (json_next(&it, 0, &b)) {
                if (!json_get(b, "type", &t))
                    continue;
                if (json_streq(t, "text") && json_get(b, "text", &x) && json_type(x) == J_STR) {
                    long n;
                    char *z = json_strdup(x, &n);
                    if (z) {
                        r.text(r.u, z, n);
                        r.end(r.u);
                    }
                    free(z);
                } else if (json_streq(t, "toolCall") && ncall < PI_TOOLS) {
                    pcall *pc = &calls[ncall];
                    jv id, nm, ar;
                    pc->id[0] = 0;
                    pc->name[0] = 0;
                    pc->shown = 0;
                    jw_init(&pc->args);
                    if (json_get(b, "id", &id))
                        json_str(id, pc->id, sizeof(pc->id));
                    if (json_get(b, "name", &nm))
                        json_str(nm, pc->name, sizeof(pc->name));
                    if (json_get(b, "arguments", &ar))
                        jw_raw(&pc->args, ar.p, ar.n);
                    jw_rawz(&pc->args, "");
                    ncall++;
                }
            }
        } else if (!strcmp(role, "toolResult")) {
            char id[80];
            int k;
            id[0] = 0;
            if (json_get(m, "toolCallId", &v))
                json_str(v, id, sizeof(id));
            for (k = 0; k < ncall; k++)
                if (!calls[k].shown && !strcmp(calls[k].id, id)) {
                    int err = json_get(m, "isError", &v) && v.n >= 4 && !memcmp(v.p, "true", 4);
                    long dur = -1;          /* the toolResult's durationMs (pi 1.1.0 ToolResultMessage), -1 none recorded */
                    if (json_get(m, "durationMs", &v) && json_type(v) == J_NUM)
                        dur = json_long(v, -1);
                    pshow_tool_start(s, calls[k].name, calls[k].args.p ? calls[k].args.p : "{}", calls[k].args.n);
                    if (json_get(m, "content", &c))
                        pshow_tool_end(s, c.p, c.n, err, dur);
                    else
                        pshow_tool_end(s, "[]", 2, err, dur);
                    calls[k].shown = 1;
                    break;
                }
        }
    }
    flush_calls(s, calls, &ncall);
    s->replay = 0;
}

void pshow_bash(pi_show *s, const char *command, const char *output, int exit_code, int cancelled, int excluded)
{
    char b[48], rule[48], line[700], num[16];
    int role = excluded ? PR_DIM : PR_BASH_MODE, i, w = s->t->cols, hidden = 0, nrows = 0;
    long n = (long)strlen(output), a = 0, k;
    long rowstart[64];
    jw body;
    blank(s);                       /* pi's component starts with its own spacer, even first in the chat */
    s->any = 1;
    sg(s, role, rule);
    raw(s, rule);
    for (i = 0; i < w; i++)
        raw(s, "\342\224\200");
    raw(s, SGR0 "\n");
    cl_copy(line, pd(s), sizeof(line));
    cl_cat(line, sg(s, role, b), sizeof(line));     /* pi 1.1.0 bash-execution.ts:54,139-141: colorKey, dim for !! */
    cl_cat(line, BOLD "$ ", sizeof(line));
    cl_cat(line, command, sizeof(line));
    cl_cat(line, SGR0, sizeof(line));
    raw(s, line);
    raw(s, "\n");
    if (n > 0) {
        /* the output split into lines (a final newline leaves an empty last line, as pi's split does), the last
         * twenty logical lines wrapped to the width, the last twenty rows of those kept */
        long lines_start[2048], nl = 0;
        long first;
        jw_init(&body);
        lines_start[nl++] = 0;
        for (k = 0; k < n && nl < 2047; k++)
            if (output[k] == '\n')
                lines_start[nl++] = k + 1;
        lines_start[nl] = n + 1;
        first = nl > 20 ? nl - 20 : 0;
        hidden = (int)first;
        for (k = first; k < nl; k++) {
            long e = lines_start[k + 1] - 1;
            show_wrap(&body, pd(s), pd(s), sg(s, PR_MUTED, b), output + lines_start[k], e - lines_start[k], pwid(s));
        }
        (void)a;
        (void)rowstart;
        (void)nrows;
        blank(s);
        jw_raw(&s->batch, body.p ? body.p : "", body.n);
        jw_free(&body);
    }
    if (hidden > 0 || cancelled || exit_code != 0) {
        char st[200];
        int any = 0;
        blank(s);
        if (hidden > 0) {
            cl_copy(st, "... ", sizeof(st));
            cl_ltoa(hidden, num);
            cl_cat(st, num, sizeof(st));
            cl_cat(st, " more lines (ctrl+o to expand)", sizeof(st));
            show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_MUTED, b), st, (long)strlen(st), pwid(s));
            any = 1;
        }
        if (cancelled)
            show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_WARNING, b), "(cancelled)", 11, pwid(s));
        else if (exit_code != 0) {
            cl_copy(st, "(exit ", sizeof(st));
            cl_ltoa(exit_code, num);
            cl_cat(st, num, sizeof(st));
            cl_cat(st, ")", sizeof(st));
            show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_ERROR, b), st, (long)strlen(st), pwid(s));
        }
        (void)any;
    }
    raw(s, rule);
    for (i = 0; i < w; i++)
        raw(s, "\342\224\200");
    raw(s, SGR0 "\n");
    out(s);
}

void pshow_set_pad(pi_show *s, int pad)
{
    s->pad = pad ? 1 : 0;
}
