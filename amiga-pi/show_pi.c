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

static void box_forget(pi_show *s);

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
        if (s->nbox)
            jw_raw(&s->shadow, s->batch.p, s->batch.n);       /* a box may have to be drawn again with this below it */
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

/* the length of an SGR sequence at p that starts from a reset (ESC[0m, ESC[m, ESC[0;..m: hl_sgr writes a style that way),
 * else 0 */
static long sgr_reset_len(const char *p, long n)
{
    long i = 2;
    if (n < 3 || p[0] != '\033' || p[1] != '[')
        return 0;
    if (p[i] == 'm')
        return 3;
    if (p[i] != '0' || i + 1 >= n || (p[i + 1] != 'm' && p[i + 1] != ';'))
        return 0;
    for (i = 3; i < n && ((p[i] >= '0' && p[i] <= '9') || p[i] == ';'); i++)
        ;
    return i < n && p[i] == 'm' ? i + 1 : 0;
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
            long rl = sgr_reset_len(tmp + j, i - j);
            if (rl) {
                jw_raw(&s->batch, tmp + j, rl);
                jw_rawz(&s->batch, bg);
                j += rl - 1;
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
    jw_init(&s->shadow);
    pshow_theme(s, th);
}

void pshow_free(pi_show *s)
{
    if (s->md)
        md_close(s->md);
    s->md = 0;
    jw_free(&s->line);
    jw_free(&s->batch);
    box_forget(s);
    jw_free(&s->shadow);
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

static unsigned long clock_ms(const pi_show *s)
{
    return s->t->io->ms ? s->t->io->ms(s->t->io->u) : 0;
}

/* the tool boxes no longer tracked (the screen was cleared, a session drawn again, the end) */
static void box_forget(pi_show *s)
{
    int i;
    for (i = 0; i < s->nbox; i++) {
        jw_free(&s->box[i].args);
        jw_free(&s->box[i].partial);
    }
    s->nbox = 0;
    jw_reset(&s->shadow);
}

static void box_drop(pi_show *s, int i)
{
    jw_free(&s->box[i].args);
    jw_free(&s->box[i].partial);
    for (; i + 1 < s->nbox; i++)
        s->box[i] = s->box[i + 1];
    s->nbox--;
    if (!s->nbox)
        jw_reset(&s->shadow);
}

static ps_box *box_find(pi_show *s, const char *id)
{
    int i;
    for (i = 0; i < s->nbox; i++)
        if (!strcmp(s->box[i].id, id))
            return &s->box[i];
    return 0;
}

static long count_lines(const char *p, long n)
{
    long k = 0, i;
    for (i = 0; i < n; i++)
        k += p[i] == '\n';
    return k;
}

/* what the end of a tool puts in its box */
typedef struct box_result {
    const char *json;
    long n;
    int is_error;
    long duration_ms;
} box_result;

static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live);
static void result_lines(pi_show *s, const char *text, int role, int max);

/* the rows of a tool's box into the batch: pi's ToolExecutionComponent, a Box(outputPad, 1) holding the call's header
 * and the result renderer's rows, over toolPendingBg until the result is final, then toolSuccessBg / toolErrorBg */
static void box_build(pi_show *s, const ps_box *b, const box_result *r)
{
    jw text;
    jv a;
    long from = s->batch.n;
    if (json_parse(b->args.p, b->args.n, &a))
        a.p = 0, a.n = 0;
    blank(s);
    header_line(s, b->name, a);
    jw_init(&text);
    if (!r) {
        /* running: bash shows its output so far and "Elapsed" (bash.ts:61-111); pi's renderResult runs only once a
         * partial result exists, and the bash tool sends an empty one when it starts (core/tools/bash.ts:318) */
        if (b->elapsed)
            bash_result(s, b->partial.p ? b->partial.p : "", clock_ms(s) - b->t0, -1, 1);
    } else {
        pshow_result_text(r->json, r->n, &text);
        jw_rawz(&text, "");
        if (!strcmp(b->name, "bash"))
            bash_result(s, text.p ? text.p : "", clock_ms(s) - b->t0, r->duration_ms, 0);
        else if (r->is_error && text.n) {
            blank(s);
            say(s, PR_ERROR, "", text.p);
        } else if (!strcmp(b->name, "ls") || !strcmp(b->name, "find"))
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 20);
        else if (!strcmp(b->name, "grep"))
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 15);
    }
    blank(s);                       /* the box's bottom padding */
    jw_free(&text);
    panel(s, from, !r ? PR_TOOL_PENDING_BG : r->is_error ? PR_TOOL_ERROR_BG : PR_TOOL_SUCCESS_BG);
}

/* box b drawn again where it is: its text in the copy replaced, the transcript from its first line on replaced by the
 * copy from there (the boxes and lines written after it stay as they were). 0, -1 when the transcript is not what the
 * copy says (cleared, other lines written meanwhile): nothing is tracked then */
static int box_redraw(pi_show *s, ps_box *b, const box_result *r)
{
    long delta, at;
    int i;
    if (s->t->n_lines != s->base + count_lines(s->shadow.p ? s->shadow.p : "", s->shadow.n)) {
        box_forget(s);
        jw_reset(&s->batch);
        return -1;
    }
    box_build(s, b, r);
    delta = s->batch.n - b->len;
    {
        jw copy;
        jw_init(&copy);
        jw_raw(&copy, s->shadow.p, b->off);
        jw_raw(&copy, s->batch.p, s->batch.n);
        jw_raw(&copy, s->shadow.p + b->off + b->len, s->shadow.n - b->off - b->len);
        jw_free(&s->shadow);
        s->shadow = copy;
    }
    for (i = 0; i < s->nbox; i++)
        if (s->box[i].off > b->off)
            s->box[i].off += delta;
    b->len = s->batch.n;
    jw_reset(&s->batch);
    at = s->base + count_lines(s->shadow.p, b->off);
    tui_replace_tail(s->t, at, s->shadow.p + b->off, s->shadow.n - b->off);
    return 0;
}

void pshow_tool_start(pi_show *s, const char *id, const char *name, const char *args, long an)
{
    ps_box nb, *b = 0;
    s->n_tools++;
    /* the block is a blank line below what is above it, with one of its own inside; pi's ToolExecutionComponent opens with
     * its Spacer(1) (tool-execution.ts:85) even at the top of a redrawn chat, where an assistant message with only tool
     * calls (no Spacer of its own, assistant-message.ts:103) is all that is above it */
    blank(s);
    s->any = 1;
    out(s);
    memset(&nb, 0, sizeof(nb));
    cl_copy(nb.id, id ? id : "", sizeof(nb.id));
    cl_copy(nb.name, name, sizeof(nb.name));
    jw_init(&nb.args);
    jw_raw(&nb.args, args, an);
    jw_rawz(&nb.args, "");
    jw_init(&nb.partial);
    nb.t0 = clock_ms(s);
    nb.elapsed = !s->replay && !strcmp(name, "bash");
    box_build(s, &nb, 0);           /* pi's box while the tool runs; the end draws it again in its final colour */
    if (s->nbox < PS_BOXES) {
        if (!s->nbox) {
            jw_reset(&s->shadow);
            s->base = s->t->n_lines;
        }
        s->box[s->nbox] = nb;
        b = &s->box[s->nbox++];
        b->off = s->shadow.n;
        b->len = s->batch.n;
    } else
        jw_free(&nb.args), jw_free(&nb.partial);
    out(s);
}

void pshow_tool_update(pi_show *s, const char *id, const char *result, long rn)
{
    ps_box *b = box_find(s, id ? id : "");
    if (!b || !b->elapsed)
        return;
    jw_reset(&b->partial);
    pshow_result_text(result, rn, &b->partial);
    jw_rawz(&b->partial, "");
    box_redraw(s, b, 0);
}

void pshow_tick(pi_show *s)
{
    int i;
    unsigned long now;
    if (!s->nbox)
        return;
    now = clock_ms(s);
    for (i = 0; i < s->nbox; i++) {
        ps_box *b = &s->box[i];
        long secs = (long)((now - b->t0) / 1000);
        if (b->elapsed && secs > b->ticks) {
            b->ticks = secs;
            if (box_redraw(s, b, 0))
                return;
        }
    }
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

/* bash: the last five lines of the output, "... (N earlier lines, ctrl+o to expand)" above when more, then how long it
 * took (live: how long it has run so far) */
static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live)
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
    if (!live && recorded >= 0)
        ms = (unsigned long)recorded;
    else if (!live && s->replay)
        return;
    duration(ms, dur);
    blank(s);
    cl_copy(m, live ? "Elapsed " : "Took ", sizeof(m));
    cl_cat(m, dur, sizeof(m));
    show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_MUTED, b), m, (long)strlen(m), pwid(s));
    (void)rows;
}

void pshow_tool_end(pi_show *s, const char *id, const char *result, long rn, int is_error, long duration_ms)
{
    ps_box *b;
    box_result r;
    out(s);
    b = box_find(s, id ? id : "");
    if (!b)
        return;                     /* not one of ours (the screen was cleared meanwhile) */
    r.json = result;
    r.n = rn;
    r.is_error = is_error;
    r.duration_ms = duration_ms;
    box_redraw(s, b, &r);
    b = box_find(s, id ? id : "");  /* box_redraw may have forgotten them all */
    if (b)
        box_drop(s, (int)(b - s->box));
}

/* ---- custom, compaction-summary and branch-summary messages ---- */

/* pi 1.1.0 components/{custom-message,compaction-summary-message,branch-summary-message}.ts: a Box(outputPad, 1) over
 * customMessageBg holding a bold label in customMessageLabel, a blank row and the text in customMessageText. The blank
 * row above the box is a Spacer(1) that interactive-mode.ts:3903,3914 (summaries) and custom-message.ts:34 (custom)
 * add whether or not the chat has anything above. The box's rows are collected from `from` and drawn by panel(). */
static long box_begin(pi_show *s, const char *label)
{
    char b[48], l[200];
    long from;
    blank(s);
    s->any = 1;
    from = s->batch.n;
    blank(s);                       /* the box's top padding row */
    cl_copy(l, pd(s), sizeof(l));
    cl_cat(l, BOLD, sizeof(l));
    cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_LABEL, b), sizeof(l));
    cl_cat(l, "[", sizeof(l));
    cl_cat(l, label, sizeof(l));
    cl_cat(l, "]" SGR0, sizeof(l));
    raw(s, l);
    raw(s, "\n");
    blank(s);                       /* Spacer(1) under the label */
    return from;
}

static void box_end(pi_show *s, long from)
{
    blank(s);                       /* the bottom padding row */
    panel(s, from, PR_CUSTOM_MESSAGE_BG);
    out(s);
}

/* tokensBefore.toLocaleString(): groups of three digits split by commas */
static void grouped(long v, char *o)
{
    char d[24];
    int n, i, k = 0;
    cl_ltoa(v < 0 ? -v : v, d);
    n = (int)strlen(d);
    if (v < 0)
        o[k++] = '-';
    for (i = 0; i < n; i++) {
        if (i && (n - i) % 3 == 0)
            o[k++] = ',';
        o[k++] = d[i];
    }
    o[k] = 0;
}

/* the collapsed line: customMessageText around the dim key hint */
static void box_collapsed(pi_show *s, const char *before, const char *after)
{
    char b[48], l[300];
    cl_copy(l, pd(s), sizeof(l));
    cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_TEXT, b), sizeof(l));
    cl_cat(l, before, sizeof(l));
    cl_cat(l, sg(s, PR_DIM, b), sizeof(l));
    cl_cat(l, "ctrl+o", sizeof(l));
    cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_TEXT, b), sizeof(l));
    cl_cat(l, after, sizeof(l));
    cl_cat(l, SGR0, sizeof(l));
    raw(s, l);
    raw(s, "\n");
}

void pshow_compaction_summary(pi_show *s, long tokens_before)
{
    char n[32], t[80];
    long from;
    grouped(tokens_before, n);
    cl_copy(t, "Compacted from ", sizeof(t));
    cl_cat(t, n, sizeof(t));
    cl_cat(t, " tokens (", sizeof(t));
    from = box_begin(s, "compaction");
    box_collapsed(s, t, " to expand)");
    box_end(s, from);
}

void pshow_branch_summary(pi_show *s)
{
    long from = box_begin(s, "branch");
    box_collapsed(s, "Branch summary (", " to expand)");
    box_end(s, from);
}

static void mdbox_sink(void *u, const char *p, long n)
{
    jw_raw((jw *)u, p, n);
}

/* Markdown(text, 0, 0, theme, {color: customMessageText}): the Markdown at the box's width, every row in the base
 * colour put back after each reset; the blank rows before and after the text are not kept */
static void mdbox_text(pi_show *s, const char *text, long n)
{
    jw md;
    vw_out vo;
    md_opts mo;
    char base[48];
    long a = 0, i, start = 0, stop = 0;
    jw_init(&md);
    vo_init(&vo, mdbox_sink, &md, VW_UTF8, s->th->depth >= 256 ? s->th->depth : 16, &s->hlt);
    mo.width = pwid(s);
    mo.urls = 1;
    mo.look = MD_LOOK_PI;
    md_render(text, n, &mo, &vo);
    vo_flush(&vo);
    sg(s, PR_CUSTOM_MESSAGE_TEXT, base);
    for (i = 0; i < md.n; i++)      /* the first and last rows with something on them */
        if (md.p[i] == '\n') {
            if (tui_width(md.p + a, i - a)) {
                if (!stop)
                    start = a;
                stop = i + 1;
            }
            a = i + 1;
        }
    for (a = start, i = start; i < stop; i++) {
        long j;
        if (md.p[i] != '\n')
            continue;
        raw(s, pd(s));
        raw(s, base);
        for (j = a; j < i; j++) {
            long rl = sgr_reset_len(md.p + j, i - j);
            if (rl) {
                jw_raw(&s->batch, md.p + j, rl);
                raw(s, base);
                j += rl - 1;
            } else
                jw_raw(&s->batch, md.p + j, 1);
        }
        raw(s, SGR0 "\n");
        a = i + 1;
    }
    jw_free(&md);
}

/* a custom message (a session's custom_message entry) with display true: its customType as the label, its text as
 * Markdown. Extension message renderers (custom-message.ts:80-98) are not run. */
void pshow_custom(pi_show *s, const char *type, const char *text, long n)
{
    long from = box_begin(s, type);
    mdbox_text(s, text, n);
    box_end(s, from);
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
            pshow_tool_start(s, c[i].id, c[i].name, c[i].args.p ? c[i].args.p : "{}", c[i].args.n);
        jw_free(&c[i].args);
    }
    *n = 0;
}

void pshow_replay(pi_show *s, const struct pi_session *ses)
{
    pshow_replay_msgs(s, ses->msg, ses->msg_n, ses->nmsg);
}

void pshow_replay_msgs(pi_show *s, char *const *msg, const long *msg_n, int nmsg)
{
    pcall calls[PI_TOOLS];
    int ncall = 0, i;
    cl_render r;
    box_forget(s);
    s->replay = 1;
    s->any = 0;
    pshow_render(s, &r);
    for (i = 0; i < nmsg; i++) {
        jv m, v, c, b, t, x;
        jit it;
        char role[24];
        if (json_parse(msg[i], msg_n[i], &m) || !json_get(m, "role", &v))
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
        } else if (!strcmp(role, "compactionSummary")) {
            long tb = json_get(m, "tokensBefore", &v) && json_type(v) == J_NUM ? json_long(v, 0) : 0;
            pshow_compaction_summary(s, tb);
        } else if (!strcmp(role, "branchSummary")) {
            pshow_branch_summary(s);
        } else if (!strcmp(role, "custom")) {
            /* interactive-mode.ts:3889: only a message with display true is drawn */
            if (json_get(m, "display", &v) && v.n >= 4 && !memcmp(v.p, "true", 4)) {
                char ty[80];
                jw w;
                ty[0] = 0;
                if (json_get(m, "customType", &v))
                    json_str(v, ty, sizeof(ty));
                jw_init(&w);
                if (json_get(m, "content", &c)) {
                    if (json_type(c) == J_STR) {
                        long n;
                        char *z = json_strdup(c, &n);
                        if (z)
                            jw_raw(&w, z, n);
                        free(z);
                    } else if (json_type(c) == J_ARR) {
                        int any = 0;
                        json_iter(c, &it);
                        while (json_next(&it, 0, &b))
                            if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                                long n;
                                char *z = json_strdup(x, &n);
                                if (z) {
                                    if (any)
                                        jw_raw(&w, "\n", 1);
                                    jw_raw(&w, z, n);
                                    any = 1;
                                }
                                free(z);
                            }
                    }
                }
                pshow_custom(s, ty, w.p ? w.p : "", w.n);
                jw_free(&w);
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
                    pshow_tool_start(s, calls[k].id, calls[k].name, calls[k].args.p ? calls[k].args.p : "{}", calls[k].args.n);
                    if (json_get(m, "content", &c))
                        pshow_tool_end(s, calls[k].id, c.p, c.n, err, dur);
                    else
                        pshow_tool_end(s, calls[k].id, "[]", 2, err, dur);
                    calls[k].shown = 1;
                    break;
                }
        }
    }
    flush_calls(s, calls, &ncall);
    box_forget(s);
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
