/* see show_pi.h */
#include <stdlib.h>
#include <string.h>
#include "show_pi.h"
#include "claude/util.h"
#include "claude/json.h"
#include "claude/show.h"

#define SGR0 "\033[0m"
#define BOLD "\033[1m"
#define DIM  "\033[2m"

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
static void say(pi_show *s, int role, const char *attr, const char *text)
{
    char b[48], st[96];
    cl_copy(st, role >= 0 ? sg(s, role, b) : "", sizeof(st));
    cl_cat(st, attr, sizeof(st));
    show_wrap(&s->batch, " ", " ", st, text, (long)strlen(text), s->t->cols - 2);
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
        raw(s, SGR0 " ");
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
    vo_init(&s->vo, md_sink, s, VW_UTF8, 16, &s->hlt);
    mo.width = s->t->cols - 2;
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
         * side; the logo and its space take five of the first row) */
        static const char hint[] =
            "escape interrupt \302\267 ctrl+c/ctrl+d clear/exit \302\267 / commands \302\267 ! bash \302\267 ctrl+o more";
        long n = (long)sizeof(hint) - 1, cut = 0, i;
        int w = 0, room = s->t->cols - 2 - 5;
        for (i = 0; i < n;) {
            unsigned long cp;
            int cl = vw_char(hint + i, n - i, &cp), cw = vw_cp_width(cp);
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
        raw(s, dim);
        jw_raw(&s->batch, hint, cut);
        raw(s, SGR0 "\n");
        if (cut < n) {
            while (cut < n && hint[cut] == ' ')
                cut++;
            show_wrap(&s->batch, " ", " ", dim, hint + cut, n - cut, s->t->cols - 2);
        }
    }
    out(s);
}

void pshow_user(pi_show *s, const char *text)
{
    spacer(s);
    blank(s);                       /* the message's padding row, above and below */
    say(s, -1, "", text);
    blank(s);
    out(s);
}

void pshow_note(pi_show *s, int role, const char *text)
{
    spacer(s);
    say(s, role, "", text);
    out(s);
}

void pshow_error(pi_show *s, const char *text)
{
    spacer(s);
    say(s, PR_ERROR, "", text);
    out(s);
}

/* ---- tool calls ---- */

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
                jw_raw(o, z, l);
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
    cl_copy(l, " ", sizeof(l));
    if (!strcmp(name, "bash")) {
        jv v;
        arg_str(a, "command", tmp, sizeof(tmp));
        cl_cat(l, BOLD, sizeof(l));
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
        cl_cat(l, BOLD "grep" SGR0 " ", sizeof(l));
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
    blank(s);
    header_line(s, name, a);
    out(s);
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
            show_wrap(&s->batch, " ", " ", sg(s, role, b), text + a, i - a, s->t->cols - 2);
            a = i + 1;
            shown++;
        }
    }
    if (total > max) {
        cl_copy(m, "... (", sizeof(m));
        cl_ltoa(total - max, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " more lines, ctrl+o to expand)", sizeof(m));
        show_wrap(&s->batch, " ", " ", sg(s, PR_MUTED, b), m, (long)strlen(m), s->t->cols - 2);
    }
}

/* bash: the last five lines of the output, "... (N earlier lines, ctrl+o to expand)" above when more */
static void bash_result(pi_show *s, const char *text, unsigned long ms)
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
        show_wrap(&wrapped, "", "", "", text + a, n - a, s->t->cols - 2);
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
            show_wrap(&s->batch, " ", " ", sg(s, PR_MUTED, b), m, (long)strlen(m), s->t->cols - 2);
        }
        for (i = skip; i < nrows; i++) {
            /* each row of wrapped holds its text and SGR0 \n: re-set in the output colour */
            long e = first_row[i + 1] - 1;
            jw_rawz(&s->batch, " ");
            jw_rawz(&s->batch, sg(s, PR_TOOL_OUTPUT, b));
            jw_raw(&s->batch, wrapped.p + first_row[i], e - first_row[i]);
            jw_rawz(&s->batch, "\n");
            rows++;
        }
        jw_free(&wrapped);
    }
    jw_free(&all);
    duration(ms, dur);
    blank(s);
    cl_copy(m, "Took ", sizeof(m));
    cl_cat(m, dur, sizeof(m));
    show_wrap(&s->batch, " ", " ", sg(s, PR_MUTED, b), m, (long)strlen(m), s->t->cols - 2);
    (void)rows;
}

void pshow_tool_end(pi_show *s, const char *result, long rn, int is_error)
{
    jw text;
    unsigned long ms = (s->t->io->ms ? s->t->io->ms(s->t->io->u) : 0) - s->t0;
    jw_init(&text);
    pshow_result_text(result, rn, &text);
    jw_rawz(&text, "");
    if (!strcmp(s->name, "bash"))
        bash_result(s, text.p ? text.p : "", ms);
    else if (is_error && text.n) {
        blank(s);
        say(s, PR_ERROR, "", text.p);
    } else if (!strcmp(s->name, "ls") || !strcmp(s->name, "find"))
        result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 20);
    else if (!strcmp(s->name, "grep"))
        result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 15);
    blank(s);                       /* the block's bottom padding */
    jw_free(&text);
    out(s);
}
