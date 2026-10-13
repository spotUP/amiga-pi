/* see show_pi.h */
#include <stdlib.h>
#include <string.h>
#include "show_pi.h"
#include "resreport.h"
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
    s->after_block = 0;
}

/* showStatus and showError add a Spacer(1) of their own: below the loaded-resources block (whose own Spacer ends it) that is two blank rows;
 * the user's message has no Spacer, its box begins with a padding row */
static void spacer_item(pi_show *s)
{
    int after = s->after_block;
    spacer(s);
    if (after)
        blank(s);
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

/* whether an SGR sequence (ESC[...m, as sgr_reset_len found it) sets a foreground colour: 30-37, 38, 90-97 */
static int sgr_sets_color(const char *p, long n)
{
    long i = 2;
    while (i < n - 1) {
        long v = 0;
        int any = 0;
        for (; i < n - 1 && p[i] >= '0' && p[i] <= '9'; i++) {
            v = v * 10 + (p[i] - '0');
            any = 1;
        }
        if (any && ((v >= 30 && v <= 38) || (v >= 90 && v <= 97)))
            return 1;
        if (any && v == 38)
            return 1;
        i++;                        /* the ';' */
    }
    return 0;
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
    s->ins = -1;
    s->t = t;
    s->pad = 1;
    jw_init(&s->line);
    jw_init(&s->batch);
    jw_init(&s->shadow);
    pshow_theme(s, th);
}

/* ---- the chat's items ---- */

static void item_free(ps_item *it)
{
    jw_free(&it->x);
    jw_free(&it->y);
    if (it->kind == PK_TOOL) {
        jw_free(&it->tool.args);
        jw_free(&it->tool.partial);
        jw_free(&it->tool.result);
    }
    free(it);
}

/* a new item at the end of the chat; 0 when memory is out (the item is drawn, not kept) */
static ps_item *item_new(pi_show *s, int kind)
{
    ps_item *it;
    if (s->nitem == s->capitem) {
        long cap = s->capitem ? s->capitem * 2 : 64;
        ps_item **n = (ps_item **)realloc(s->item, (size_t)cap * sizeof(*n));
        if (!n)
            return 0;
        s->item = n;
        s->capitem = cap;
    }
    it = (ps_item *)calloc(1, sizeof(*it));
    if (!it)
        return 0;
    it->kind = kind;
    jw_init(&it->x);
    jw_init(&it->y);
    jw_init(&it->tool.args);
    jw_init(&it->tool.partial);
    jw_init(&it->tool.result);
    if (s->ins >= 0 && s->ins <= s->nitem) {
        /* the loaded-resources block is being written again: its items go where the old ones were */
        memmove(&s->item[s->ins + 1], &s->item[s->ins], (size_t)(s->nitem - s->ins) * sizeof(*s->item));
        s->item[s->ins++] = it;
        s->nitem++;
        return it;
    }
    s->item[s->nitem++] = it;
    return it;
}

/* an item holding two texts (z: zero-terminated; the length of x is xn, y is a string) */
static ps_item *item_text(pi_show *s, int kind, long a, const char *x, long xn, const char *y)
{
    ps_item *it = item_new(s, kind);
    if (!it)
        return 0;
    it->a = a;
    if (x)
        jw_raw(&it->x, x, xn);
    jw_rawz(&it->x, "");
    if (y)
        jw_rawz(&it->y, y);
    return it;
}

static void items_drop(pi_show *s)
{
    long i;
    for (i = 0; i < s->nitem; i++)
        item_free(s->item[i]);
    s->nitem = 0;
    s->cur = 0;
}

void pshow_free(pi_show *s)
{
    if (s->md)
        md_close(s->md);
    s->md = 0;
    jw_free(&s->line);
    jw_free(&s->batch);
    box_forget(s);
    items_drop(s);
    free(s->item);
    s->item = 0;
    s->capitem = 0;
    jw_free(&s->shadow);
}

/* the answer being written dropped (the screen is about to be written again) */
static void md_abandon(pi_show *s)
{
    if (s->md)
        md_close(s->md);
    s->md = 0;
    vo_flush(&s->vo);
    jw_reset(&s->line);
    jw_reset(&s->batch);
}

void pshow_clear(pi_show *s)
{
    md_abandon(s);
    box_forget(s);
    items_drop(s);
    tui_clear(s->t);
    s->any = 0;
    s->after_block = 0;
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

/* the Markdown block of an answer opened */
static void md_begin(pi_show *s)
{
    md_opts mo;
    vo_init(&s->vo, md_sink, s, VW_UTF8, s->th->depth >= 256 ? s->th->depth : 16, &s->hlt);
    mo.width = pwid(s);
    mo.urls = 1;
    mo.look = MD_LOOK_PI;
    s->md = md_open(&mo, &s->vo);
    s->first = 1;
    jw_reset(&s->line);
}

/* an answer begins: its item, and the block */
static void md_start(pi_show *s)
{
    if (s->md)
        return;
    md_begin(s);
    s->cur = item_new(s, PK_ANSWER);
}

static void r_text(void *u, const char *p, long n)
{
    pi_show *s = (pi_show *)u;
    s->n_text++;
    md_start(s);
    if (s->cur)
        jw_raw(&s->cur->x, p, n);       /* kept whole: Ctrl+O draws the answer again */
    if (s->md)
        md_feed(s->md, p, n);
    vo_flush(&s->vo);
    out(s);
}

/* the block closed: what md holds back is written */
static void md_finish(pi_show *s)
{
    md_close(s->md);
    s->md = 0;
    vo_flush(&s->vo);
    if (s->line.n)
        md_sink(s, "\n", 1);
    out(s);
}

static void r_end(void *u)
{
    pi_show *s = (pi_show *)u;
    if (!s->md)
        return;
    md_finish(s);
    s->cur = 0;
}

/* an answer item drawn again; the one still being written stays open for what follows */
static void answer_draw(pi_show *s, ps_item *it)
{
    md_begin(s);
    if (it->x.n)
        md_feed(s->md, it->x.p, it->x.n);
    vo_flush(&s->vo);
    out(s);
    if (it != s->cur)
        md_finish(s);
}

void pshow_render(pi_show *s, cl_render *r)
{
    r->u = s;
    r->text = r_text;
    r->end = r_end;
}

/* ---- the program's own lines ---- */

static void d_header(pi_show *s, const char *name, const char *version)
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

static void box_end(pi_show *s, long from);
static void mdbox_text(pi_show *s, const char *text, long n);

/* a user message's box: padding rows above and below the text */
static void user_box(pi_show *s, const char *text)
{
    long from = s->batch.n;
    blank(s);                       /* the message's padding row, above and below */
    say(s, PR_USER_MESSAGE_TEXT, "", text);
    blank(s);
    panel(s, from, PR_USER_MESSAGE_BG);
    out(s);
}

/* pi's parseSkillBlock (core/agent-session.ts:177-188): /^<skill name="([^"]+)" location="([^"]+)">\n([\s\S]*?)\n<\/skill>
 * (?:\n\n([\s\S]+))?$/ over the text; the user message is trimmed, "" when none. 1 matched, with the pieces as slices of t */
static int skill_block(const char *t, const char **name, long *nn, const char **content, long *cn, const char **user, long *un)
{
    static const char end[] = "\n</skill>";
    long len = (long)strlen(t), i, q, r;
    const char *p;
    if (strncmp(t, "<skill name=\"", 13))
        return 0;
    *name = t + 13;
    for (i = 13; i < len && t[i] != '"'; i++)
        ;
    *nn = i - 13;
    if (!*nn || strncmp(t + i, "\" location=\"", 12))
        return 0;
    q = i + 12;
    for (r = q; r < len && t[r] != '"'; r++)
        ;
    if (r == q || strncmp(t + r, "\">\n", 3))
        return 0;
    *content = t + r + 3;
    /* the lazy match: the first "\n</skill>" after which the text ends or "\n\n" and something follows */
    for (p = strstr(*content, end); p; p = strstr(p + 1, end)) {
        long rest = (long)(p - t) + 9;
        if (rest == len || (rest + 2 < len && t[rest] == '\n' && t[rest + 1] == '\n')) {
            *cn = (long)(p - *content);
            *user = rest == len ? "" : t + rest + 2;
            *un = rest == len ? 0 : len - rest - 2;
            while (*un && (**user == ' ' || **user == '\n' || **user == '\t' || **user == '\r')) {
                (*user)++;
                (*un)--;
            }
            while (*un && ((*user)[*un - 1] == ' ' || (*user)[*un - 1] == '\n' || (*user)[*un - 1] == '\t' || (*user)[*un - 1] == '\r'))
                (*un)--;
            return 1;
        }
    }
    return 0;
}

/* pi 1.1.0 SkillInvocationMessageComponent (components/skill-invocation-message.ts): a Box(outputPad, 1) over customMessageBg;
 * collapsed one row "[skill] name (ctrl+o to expand)", expanded the label and the skill's text as Markdown under its bold name */
static void skill_box(pi_show *s, const char *name, long nn, const char *content, long cn)
{
    char b[48], l[400], nm[200];
    long from;
    cl_copy(nm, name, nn < (long)sizeof(nm) ? nn + 1 : (long)sizeof(nm));
    from = s->batch.n;
    blank(s);                       /* the box's top padding row */
    cl_copy(l, pd(s), sizeof(l));
    cl_cat(l, BOLD, sizeof(l));
    cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_LABEL, b), sizeof(l));
    cl_cat(l, "[skill]", sizeof(l));
    if (s->expanded) {
        jw t;
        cl_cat(l, SGR0, sizeof(l));
        raw(s, l);
        raw(s, "\n");
        jw_init(&t);
        jw_rawz(&t, "**");
        jw_raw(&t, name, nn);
        jw_rawz(&t, "**\n\n");
        jw_raw(&t, content, cn);
        mdbox_text(s, t.p ? t.p : "", t.n);
        jw_free(&t);
    } else {
        cl_cat(l, SGR0, sizeof(l));
        cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_LABEL, b), sizeof(l));
        cl_cat(l, " \033[39m", sizeof(l));
        cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_TEXT, b), sizeof(l));
        cl_cat(l, nm, sizeof(l));
        cl_cat(l, sg(s, PR_DIM, b), sizeof(l));
        cl_cat(l, " (ctrl+o to expand)" SGR0, sizeof(l));
        raw(s, l);
        raw(s, "\n");
    }
    box_end(s, from);
}

static void d_user(pi_show *s, const char *text)
{
    const char *name, *content, *user;
    long nn, cn, un;
    spacer(s);
    if (skill_block(text, &name, &nn, &content, &cn, &user, &un)) {
        /* a /skill:name message: the skill's box, then the arguments as the user's own message */
        skill_box(s, name, nn, content, cn);
        if (un) {
            char *u = (char *)malloc((size_t)un + 1);
            if (u) {
                memcpy(u, user, (size_t)un);
                u[un] = 0;
                blank(s);
                user_box(s, u);
                free(u);
            }
        }
        return;
    }
    user_box(s, text);
}

static void d_note(pi_show *s, int role, const char *text)
{
    spacer_item(s);
    say_pad(s, 1, role, "", text);     /* pi's status lines are Text(.., 1, 0), not outputPad */
    out(s);
}

static void d_error(pi_show *s, const char *text)
{
    spacer_item(s);
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
    s->nbox = 0;
    jw_reset(&s->shadow);
}

static void box_drop(pi_show *s, int i)
{
    for (; i + 1 < s->nbox; i++)
        s->box[i] = s->box[i + 1];
    s->nbox--;
    if (!s->nbox)
        jw_reset(&s->shadow);
}

static long count_lines(const char *p, long n)
{
    long k = 0, i;
    for (i = 0; i < n; i++)
        k += p[i] == '\n';
    return k;
}

static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live);
static void result_lines(pi_show *s, const char *text, int role, int max);
static void read_lines(pi_show *s, const char *text);

/* the rows of a tool's box into the batch: pi's ToolExecutionComponent, a Box(outputPad, 1) holding the call's header
 * and the result renderer's rows, over toolPendingBg until the result is final, then toolSuccessBg / toolErrorBg. With
 * toolOutputExpanded (s->expanded) the result renderers show everything they have (renderers/{read,bash,ls,find,grep}.ts) */
static void box_build(pi_show *s, const ps_box *b)
{
    jw text;
    jv a;
    long from = s->batch.n;
    int replay = s->replay;
    s->replay = b->replay;
    if (json_parse(b->args.p, b->args.n, &a))
        a.p = 0, a.n = 0;
    blank(s);
    header_line(s, b->name, a);
    jw_init(&text);
    if (!b->done) {
        /* running: bash shows its output so far and "Elapsed" (bash.ts:61-111); pi's renderResult runs only once a
         * partial result exists, and the bash tool sends an empty one when it starts (core/tools/bash.ts:318) */
        if (b->elapsed)
            bash_result(s, b->partial.p ? b->partial.p : "", clock_ms(s) - b->t0, -1, 1);
    } else {
        pshow_result_text(b->result.p ? b->result.p : "", b->result.n, &text);
        jw_rawz(&text, "");
        if (!strcmp(b->name, "bash"))
            bash_result(s, text.p ? text.p : "", 0, b->duration_ms, 0);
        else if (b->is_error && text.n) {
            blank(s);
            say(s, PR_ERROR, "", text.p);
        } else if (!strcmp(b->name, "read")) {
            if (s->expanded)        /* renderers/read.ts:80-83: collapsed, the result shows nothing */
                read_lines(s, text.p ? text.p : "");
        } else if (!strcmp(b->name, "ls") || !strcmp(b->name, "find"))
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 20);
        else if (!strcmp(b->name, "grep"))
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 15);
    }
    blank(s);                       /* the box's bottom padding */
    jw_free(&text);
    s->replay = replay;
    panel(s, from, !b->done ? PR_TOOL_PENDING_BG : b->is_error ? PR_TOOL_ERROR_BG : PR_TOOL_SUCCESS_BG);
}

/* box b drawn again where it is: its text in the copy replaced, the transcript from its first line on replaced by the
 * copy from there (the boxes and lines written after it stay as they were). 0, -1 when the transcript is not what the
 * copy says (cleared, other lines written meanwhile): nothing is tracked then */
static int box_redraw(pi_show *s, ps_box *b)
{
    long delta, at;
    int i;
    if (s->t->n_lines != s->base + count_lines(s->shadow.p ? s->shadow.p : "", s->shadow.n)) {
        box_forget(s);
        jw_reset(&s->batch);
        return -1;
    }
    box_build(s, b);
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
        if (s->box[i]->off > b->off)
            s->box[i]->off += delta;
    b->len = s->batch.n;
    jw_reset(&s->batch);
    at = s->base + count_lines(s->shadow.p, b->off);
    tui_replace_tail(s->t, at, s->shadow.p + b->off, s->shadow.n - b->off);
    return 0;
}

/* a tool item drawn: the blank row of its Spacer, then the box. A box that waits for its result is tracked, to be drawn
 * again where it is (box_redraw) */
static void tool_draw(pi_show *s, ps_box *b)
{
    blank(s);
    s->any = 1;
    out(s);
    box_build(s, b);
    if (!b->done && s->nbox < PS_BOXES) {
        if (!s->nbox) {
            jw_reset(&s->shadow);
            s->base = s->t->n_lines;
        }
        s->box[s->nbox++] = b;
        b->off = s->shadow.n;
        b->len = s->batch.n;
    }
    out(s);
}

/* the box of the newest tool call with this id that waits for its result, tracked or not */
static ps_box *item_box(pi_show *s, const char *id)
{
    long i;
    for (i = s->nitem - 1; i >= 0; i--)
        if (s->item[i]->kind == PK_TOOL && !s->item[i]->tool.done && !strcmp(s->item[i]->tool.id, id))
            return &s->item[i]->tool;
    return 0;
}

static int box_tracked(const pi_show *s, const ps_box *b)
{
    int i;
    for (i = 0; i < s->nbox; i++)
        if (s->box[i] == b)
            return i + 1;
    return 0;
}

void pshow_tool_start(pi_show *s, const char *id, const char *name, const char *args, long an)
{
    ps_item *it;
    ps_box *b;
    s->n_tools++;
    /* the block is a blank line below what is above it, with one of its own inside; pi's ToolExecutionComponent opens with
     * its Spacer(1) (tool-execution.ts:85) even at the top of a redrawn chat, where an assistant message with only tool
     * calls (no Spacer of its own, assistant-message.ts:103) is all that is above it */
    it = item_new(s, PK_TOOL);
    if (!it)
        return;
    b = &it->tool;
    cl_copy(b->id, id ? id : "", sizeof(b->id));
    cl_copy(b->name, name, sizeof(b->name));
    jw_raw(&b->args, args, an);
    jw_rawz(&b->args, "");
    b->t0 = clock_ms(s);
    b->elapsed = !s->replay && !strcmp(name, "bash");
    b->replay = s->replay;
    b->duration_ms = -1;
    tool_draw(s, b);        /* pi's box while the tool runs; the end draws it again in its final colour */
}

void pshow_tool_update(pi_show *s, const char *id, const char *result, long rn)
{
    ps_box *b = item_box(s, id ? id : "");
    if (!b || !b->elapsed)
        return;
    jw_reset(&b->partial);
    pshow_result_text(result, rn, &b->partial);
    jw_rawz(&b->partial, "");
    if (box_tracked(s, b))
        box_redraw(s, b);
}

void pshow_tick(pi_show *s)
{
    int i;
    unsigned long now;
    if (!s->nbox)
        return;
    now = clock_ms(s);
    for (i = 0; i < s->nbox; i++) {
        ps_box *b = s->box[i];
        long secs = (long)((now - b->t0) / 1000);
        if (b->elapsed && secs > b->ticks) {
            b->ticks = secs;
            if (box_redraw(s, b))
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

/* the key hint of pi's keyHint (keybinding-hints.ts:42-44): the key in the dim colour, the rest in the muted one, between
 * muted `before` and `after`; one row, outputPad in */
static void hint_row(pi_show *s, const char *before, const char *after)
{
    char d[48], m[48];
    sg(s, PR_MUTED, m);
    sg(s, PR_DIM, d);
    raw(s, pd(s));
    raw(s, m);
    raw(s, before);
    raw(s, d);
    raw(s, "ctrl+o");
    raw(s, m);
    raw(s, after);
    raw(s, SGR0 "\n");
}

/* the lines of text (split at newlines, trailing blank lines cut), each one space in, at most max shown from the
 * start (max < 0: all, toolOutputExpanded); the rest as "... (N more lines, ctrl+o to expand)" */
static void result_lines(pi_show *s, const char *text, int role, int max)
{
    long n = (long)strlen(text), a = 0, shown = 0, total = 0, i;
    char b[48], m[96], num[16];
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' '))
        n--;
    for (i = 0; i <= n; i++)
        if (i == n || text[i] == '\n')
            total++;
    if (s->expanded)
        max = -1;
    blank(s);
    for (i = 0; i <= n && (max < 0 || shown < max); i++) {
        if (i == n || text[i] == '\n') {
            show_wrap(&s->batch, pd(s), pd(s), sg(s, role, b), text + a, i - a, pwid(s));
            a = i + 1;
            shown++;
        }
    }
    if (max >= 0 && total > max) {
        cl_copy(m, "... (", sizeof(m));
        cl_ltoa(total - max, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " more lines, ", sizeof(m));
        hint_row(s, m, " to expand)");
    }
}

/* renderers/read.ts:73-90, expanded: every line of the file as the tool returned it, trailing empty lines cut, tabs three
 * spaces, in the output colour (a file with a known language is highlighted by pi; not here) */
static void read_lines(pi_show *s, const char *text)
{
    long n = (long)strlen(text), a = 0, i;
    char b[48];
    jw t;
    jw_init(&t);
    for (i = 0; i < n; i++) {
        if (text[i] == '\t')
            jw_rawz(&t, "   ");
        else
            jw_raw(&t, text + i, 1);
    }
    jw_rawz(&t, "");
    n = t.n;
    /* the lines are text.split("\n"): a final newline leaves an empty last line, cut with the others */
    while (n > 0 && t.p[n - 1] == '\n')
        n--;
    blank(s);
    for (i = 0; i <= n && n > 0; i++)
        if (i == n || t.p[i] == '\n') {
            show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_TOOL_OUTPUT, b), t.p + a, i - a, pwid(s));
            a = i + 1;
        }
    jw_free(&t);
}

/* bash: the last five lines of the output, "... (N earlier lines, ctrl+o to expand)" above when more (expanded: all of
 * it, renderers/bash.ts:60-77), then how long it took (live: how long it has run so far) */
static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live)
{
    char b[48], dur[24], m[96], num[16];
    long n = (long)strlen(text), a = 0, i, nrows = 0, skip = 0, row = 0, start = 0;
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r'))
        n--;
    while (a < n && (text[a] == '\n' || text[a] == ' ' || text[a] == '\t'))
        a++;
    if (a < n) {
        /* the output wrapped to the screen's width: its rows ("visual lines"), the last five kept */
        jw wrapped;
        jw_init(&wrapped);
        show_wrap(&wrapped, "", "", "", text + a, n - a, pwid(s));
        for (i = 0; i < wrapped.n; i++)
            nrows += wrapped.p[i] == '\n';
        skip = nrows > 5 && !s->expanded ? nrows - 5 : 0;
        blank(s);
        if (skip) {
            cl_copy(m, "... (", sizeof(m));
            cl_ltoa(skip, num);
            cl_cat(m, num, sizeof(m));
            cl_cat(m, " earlier lines, ", sizeof(m));
            hint_row(s, m, " to expand)");
        }
        for (i = 0; i < wrapped.n; i++) {
            if (wrapped.p[i] != '\n')
                continue;
            /* each row of wrapped holds its text and SGR0 \n: re-set in the output colour */
            if (row++ >= skip) {
                jw_rawz(&s->batch, pd(s));
                jw_rawz(&s->batch, sg(s, PR_TOOL_OUTPUT, b));
                jw_raw(&s->batch, wrapped.p + start, i - start);
                jw_rawz(&s->batch, "\n");
            }
            start = i + 1;
        }
        jw_free(&wrapped);
    }
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
}

void pshow_tool_end(pi_show *s, const char *id, const char *result, long rn, int is_error, long duration_ms)
{
    ps_box *b;
    int k;
    out(s);
    b = item_box(s, id ? id : "");
    if (!b)
        return;                     /* not one of ours (the screen was cleared meanwhile) */
    jw_reset(&b->result);
    jw_raw(&b->result, result, rn);
    jw_rawz(&b->result, "");
    b->is_error = is_error;
    /* the recorded duration, else the call's own clock (kept: a later redraw shows the same "Took") */
    b->duration_ms = duration_ms < 0 && !b->replay ? (long)(clock_ms(s) - b->t0) : duration_ms;
    b->done = 1;
    if (!box_tracked(s, b))
        return;                     /* not on the screen as a box that can be drawn again: the next redraw shows it */
    box_redraw(s, b);
    k = box_tracked(s, b);          /* box_redraw may have forgotten them all */
    if (k)
        box_drop(s, k - 1);
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

static void mdbox_text(pi_show *s, const char *text, long n);

/* the expanded body: Markdown(header + summary, ...) in customMessageText (compaction-summary-message.ts:49-54,
 * branch-summary-message.ts:43-48) */
static void box_expanded(pi_show *s, const char *header, const char *summary, long n)
{
    jw t;
    jw_init(&t);
    jw_rawz(&t, header);
    jw_raw(&t, summary, n);
    mdbox_text(s, t.p ? t.p : "", t.n);
    jw_free(&t);
}

/* `summary` is the message's summary text; shown only when pi's toolOutputExpanded is on (ctrl+o, s->expanded) */
static void d_compaction(pi_show *s, long tokens_before, const char *summary, long n)
{
    char num[32], t[80];
    long from;
    grouped(tokens_before, num);
    from = box_begin(s, "compaction");
    if (s->expanded) {
        char h[80];
        cl_copy(h, "**Compacted from ", sizeof(h));
        cl_cat(h, num, sizeof(h));
        cl_cat(h, " tokens**\n\n", sizeof(h));
        box_expanded(s, h, summary ? summary : "", summary ? n : 0);
    } else {
        cl_copy(t, "Compacted from ", sizeof(t));
        cl_cat(t, num, sizeof(t));
        cl_cat(t, " tokens (", sizeof(t));
        box_collapsed(s, t, " to expand)");
    }
    box_end(s, from);
}

static void d_branch(pi_show *s, const char *summary, long n)
{
    long from = box_begin(s, "branch");
    if (s->expanded)
        box_expanded(s, "**Branch Summary**\n\n", summary ? summary : "", summary ? n : 0);
    else
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
                /* a style of its own colour (a heading, a list bullet: pi styles them without the box's text colour)
                 * keeps its look; the plain resets and the styles without a colour (bold, italic) get the text colour back */
                if (!sgr_sets_color(md.p + j, rl))
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

/* pi's loaded-resources block (interactive-mode.ts showLoadedResources): ThemedText and ExpandableText with padding 0, each
 * followed by a Spacer(1). The title is "[name]" in a role's colour. The blank row of the block's end is the chat's first Spacer,
 * so s->any is cleared. */
static void d_rtitle(pi_show *s, int role, const char *title)
{
    char b[48], l[200];
    cl_copy(l, sg(s, role, b), sizeof(l));
    cl_cat(l, "[", sizeof(l));
    cl_cat(l, title, sizeof(l));
    cl_cat(l, "]" SGR0, sizeof(l));
    raw(s, l);
    raw(s, "\n");
}

static void d_rline(pi_show *s, int kind, int mode, const char *text)
{
    char b[48], c[48];
    const char *nl;
    if ((mode == RRM_COLLAPSED && s->expanded) || (mode == RRM_EXPANDED && !s->expanded))
        return;                     /* the block's other form (ExpandableText: Ctrl+O switches between them) */
    if (kind == RRL_COLL_WIN || kind == RRL_COLL_LOSE) {
        /* "    <glyph> <path>" in the dim colour with the glyph in success (winner) or warning (skipped) */
        long glyph = 4;
        if ((long)strlen(text) > glyph + 3 && tui_width(text, (long)strlen(text)) <= s->t->cols) {
            raw(s, sg(s, PR_DIM, b));
            jw_raw(&s->batch, text, glyph);
            raw(s, sg(s, kind == RRL_COLL_WIN ? PR_SUCCESS : PR_WARNING, c));
            jw_raw(&s->batch, text + glyph, 3);
            raw(s, "\033[39m");          /* the dim colour is the terminal's own: the glyph's colour ends */
            raw(s, text + glyph + 3);
            raw(s, SGR0 "\n");
            return;
        }
        say_pad(s, 0, PR_DIM, "", text);
        return;
    }
    if (kind == RRL_SCOPE) {
        /* "  <scope>": the two spaces plain, the scope in the accent colour */
        raw(s, "  ");
        raw(s, sg(s, PR_ACCENT, b));
        raw(s, text + 2);
        raw(s, SGR0 "\n");
        return;
    }
    /* a message with line breaks of its own (a YAML error's context lines): the lines one after the other */
    nl = strchr(text, '\n');
    if (nl) {
        char one[1200];
        long n = (long)(nl - text);
        if (n >= (long)sizeof(one))
            n = (long)sizeof(one) - 1;
        memcpy(one, text, (size_t)n);
        one[n] = 0;
        d_rline(s, kind, RRM_BOTH, one);
        d_rline(s, kind, RRM_BOTH, nl + 1);
        return;
    }
    say_pad(s, 0, kind == RRL_ERROR ? PR_ERROR : kind == RRL_ITEM ? PR_DIM : PR_WARNING, "", text);
}

static void d_rend(pi_show *s)
{
    blank(s);                       /* Spacer(1) */
    s->any = 0;
    s->after_block = 1;
    out(s);
}

static void d_blank(pi_show *s)
{
    blank(s);
    out(s);
}

/* a custom message (a session's custom_message entry) with display true: its customType as the label, its text as
 * Markdown. Extension message renderers (custom-message.ts:80-98) are not run. */
static void d_custom(pi_show *s, const char *type, const char *text, long n)
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
    s->after_block = 0;
    item_new(s, PK_RESET);          /* a redraw of the items begins the conversation the same way */
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
            long tb = json_get(m, "tokensBefore", &v) && json_type(v) == J_NUM ? json_long(v, 0) : 0, sn = 0;
            char *sum = json_get(m, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &sn) : 0;
            pshow_compaction_summary(s, tb, sum, sn);
            free(sum);
        } else if (!strcmp(role, "branchSummary")) {
            long sn = 0;
            char *sum = json_get(m, "summary", &v) && json_type(v) == J_STR ? json_strdup(v, &sn) : 0;
            pshow_branch_summary(s, sum, sn);
            free(sum);
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

static void d_bash(pi_show *s, const char *command, const char *output, int exit_code, int cancelled, int excluded)
{
    char b[48], rule[48], line[700], num[16];
    int role = excluded ? PR_DIM : PR_BASH_MODE, i, w = s->t->cols;
    long n = (long)strlen(output), k, nl = 0, first, hidden = 0;
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
         * twenty logical lines wrapped to the width (bash-execution.ts:19,100-101); expanded, all of them (:130-133) */
        jw body;
        long a = 0;
        jw_init(&body);
        for (k = 0; k <= n; k++)
            nl += k == n || output[k] == '\n';
        first = nl > 20 ? nl - 20 : 0;
        hidden = first;
        if (s->expanded)
            first = 0;
        nl = 0;
        for (k = 0; k <= n; k++)
            if (k == n || output[k] == '\n') {
                if (nl++ >= first)
                    show_wrap(&body, pd(s), pd(s), sg(s, PR_MUTED, b), output + a, k - a, pwid(s));
                a = k + 1;
            }
        blank(s);
        jw_raw(&s->batch, body.p ? body.p : "", body.n);
        jw_free(&body);
    }
    if (hidden > 0 || cancelled || exit_code != 0) {
        char st[200];
        blank(s);
        if (hidden > 0) {
            if (s->expanded) {
                hint_row(s, "(", " to collapse)");
            } else {
                cl_copy(st, "... ", sizeof(st));
                cl_ltoa(hidden, num);
                cl_cat(st, num, sizeof(st));
                cl_cat(st, " more lines (", sizeof(st));
                hint_row(s, st, " to expand)");
            }
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
    }
    raw(s, rule);
    for (i = 0; i < w; i++)
        raw(s, "\342\224\200");
    raw(s, SGR0 "\n");
    out(s);
}

/* ---- the chat as a list: what is written is kept, so it can be written again ---- */

/* one item drawn (the same code as when it is first written) */
static void item_draw(pi_show *s, ps_item *it)
{
    const char *x = it->x.p ? it->x.p : "", *y = it->y.p ? it->y.p : "";
    switch (it->kind) {
    case PK_HEADER:
        d_header(s, x, y);
        break;
    case PK_USER:
        d_user(s, x);
        break;
    case PK_ANSWER:
        answer_draw(s, it);
        break;
    case PK_NOTE:
    case PK_STATUS:
        d_note(s, (int)it->a, x);
        break;
    case PK_ERROR:
        d_error(s, x);
        break;
    case PK_TOOL:
        tool_draw(s, &it->tool);
        break;
    case PK_BASH:
        d_bash(s, x, y, (int)it->a, (int)it->b, (int)it->c);
        break;
    case PK_COMPACT:
        d_compaction(s, it->a, x, it->x.n);
        break;
    case PK_BRANCH:
        d_branch(s, x, it->x.n);
        break;
    case PK_CUSTOM:
        d_custom(s, x, y, it->y.n);
        break;
    case PK_RTITLE:
        d_rtitle(s, (int)it->a, x);
        break;
    case PK_RLINE:
        d_rline(s, (int)it->a, (int)it->b, x);
        break;
    case PK_REND:
        d_rend(s);
        break;
    case PK_BLANK:
        d_blank(s);
        break;
    case PK_RESET:
        s->any = 0;
        s->after_block = 0;
        break;
    }
}

/* the screen drawn again from the items */
static void redraw_items(pi_show *s)
{
    long i, n_text = s->n_text, n_blocks = s->n_blocks, n_tools = s->n_tools;
    md_abandon(s);
    box_forget(s);
    tui_clear(s->t);
    s->any = 0;
    s->after_block = 0;
    s->replay = 0;
    for (i = 0; i < s->nitem; i++)
        item_draw(s, s->item[i]);
    s->n_text = n_text;
    s->n_blocks = n_blocks;
    s->n_tools = n_tools;
}

void pshow_expand(pi_show *s, int expanded, const char *status)
{
    ps_item *last = s->nitem ? s->item[s->nitem - 1] : 0;
    s->expanded = expanded;
    if (last && last->kind == PK_STATUS) {              /* showStatus (interactive-mode.ts:3824-3834) */
        jw_reset(&last->x);
        jw_rawz(&last->x, status);
    } else
        item_text(s, PK_STATUS, PR_DIM, status, (long)strlen(status), 0);
    redraw_items(s);
}

/* pi's reload() empties loadedResourcesContainer and showLoadedResources fills it again (interactive-mode.ts:6489-6496): the old block's
 * items are dropped (the run after the header of RTITLE, RLINE, REND and BLANK items) and what is written until pshow_resources_end
 * takes their place; the end draws the screen again */
void pshow_resources_begin(pi_show *s)
{
    long start = s->nitem && s->item[0]->kind == PK_HEADER ? 1 : 0, end = start, i;
    while (end < s->nitem) {
        int k = s->item[end]->kind;
        if (k != PK_RTITLE && k != PK_RLINE && k != PK_REND && k != PK_BLANK)
            break;
        end++;
    }
    for (i = start; i < end; i++)
        item_free(s->item[i]);
    memmove(&s->item[start], &s->item[end], (size_t)(s->nitem - end) * sizeof(*s->item));
    s->nitem -= end - start;
    s->ins = start;
}

void pshow_resources_end(pi_show *s)
{
    s->ins = -1;
    redraw_items(s);
}

void pshow_header(pi_show *s, const char *name, const char *version)
{
    item_text(s, PK_HEADER, 0, name, (long)strlen(name), version);
    d_header(s, name, version);
}

void pshow_user(pi_show *s, const char *text)
{
    item_text(s, PK_USER, 0, text, (long)strlen(text), 0);
    d_user(s, text);
}

/* a dim note is pi's showStatus: Ctrl+O replaces one that is the last item */
void pshow_note(pi_show *s, int role, const char *text)
{
    item_text(s, role == PR_DIM ? PK_STATUS : PK_NOTE, role, text, (long)strlen(text), 0);
    d_note(s, role, text);
}

void pshow_error(pi_show *s, const char *text)
{
    item_text(s, PK_ERROR, 0, text, (long)strlen(text), 0);
    d_error(s, text);
}

void pshow_compaction_summary(pi_show *s, long tokens_before, const char *summary, long n)
{
    item_text(s, PK_COMPACT, tokens_before, summary ? summary : "", summary ? n : 0, 0);
    d_compaction(s, tokens_before, summary, n);
}

void pshow_branch_summary(pi_show *s, const char *summary, long n)
{
    item_text(s, PK_BRANCH, 0, summary ? summary : "", summary ? n : 0, 0);
    d_branch(s, summary, n);
}

void pshow_custom(pi_show *s, const char *type, const char *text, long n)
{
    ps_item *it = item_text(s, PK_CUSTOM, 0, type, (long)strlen(type), 0);
    if (it)
        jw_raw(&it->y, text, n);
    d_custom(s, type, text, n);
}

void pshow_resource_title(pi_show *s, int role, const char *title)
{
    item_text(s, PK_RTITLE, role, title, (long)strlen(title), 0);
    if (s->ins < 0)
        d_rtitle(s, role, title);
}

void pshow_resource_line(pi_show *s, int kind, int mode, const char *text)
{
    ps_item *it = item_text(s, PK_RLINE, kind, text, (long)strlen(text), 0);
    if (it)
        it->b = mode;
    if (s->ins < 0)
        d_rline(s, kind, mode, text);
}

void pshow_resource_end(pi_show *s)
{
    item_text(s, PK_REND, 0, 0, 0, 0);
    if (s->ins < 0)
        d_rend(s);
}

void pshow_blank(pi_show *s)
{
    item_text(s, PK_BLANK, 0, 0, 0, 0);
    if (s->ins < 0)
        d_blank(s);
}

void pshow_bash(pi_show *s, const char *command, const char *output, int exit_code, int cancelled, int excluded)
{
    ps_item *it = item_text(s, PK_BASH, exit_code, command, (long)strlen(command), output);
    if (it) {
        it->b = cancelled;
        it->c = excluded;
    }
    d_bash(s, command, output, exit_code, cancelled, excluded);
}

void pshow_set_pad(pi_show *s, int pad)
{
    s->pad = pad ? 1 : 0;
}
