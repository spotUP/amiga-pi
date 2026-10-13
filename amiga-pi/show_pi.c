/* see show_pi.h */
#include <stdlib.h>
#include <string.h>
#include "show_pi.h"
#include "ext.h"
#include "resreport.h"
#include "ansi.h"
#include "claude/util.h"
#include "claude/json.h"
#include "claude/show.h"
#include "agent.h"
#include "jsdiff.h"
#include "truncate.h"
#include "view/hl_lex.h"

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

/* showStatus, showWarning and showError add a Spacer(1) of their own: below the loaded-resources block (whose own Spacer ends it) that is two blank rows;
 * the user's message has no Spacer, its box begins with a padding row */
static void spacer_item(pi_show *s)
{
    int after = s->after_block;
    if (s->any || !after)           /* a Spacer(1) is a child like any other: the first one of an empty chat has its row too (the */
        blank(s);                   /* loaded-resources block clears `any`: its own Spacer is the row there) */
    s->any = 1;
    s->after_block = 0;
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
    s->details = 1;
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
    jw_free(&it->z);
    if (it->kind == PK_TOOL) {
        jw_free(&it->tool.det);
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
    jw_init(&it->z);
    jw_init(&it->tool.args);
    jw_init(&it->tool.partial);
    jw_init(&it->tool.result);
    jw_init(&it->tool.det);
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
        /* pi's compactInstructions / expandedInstructions (interactive-mode.ts:1036-1057), one Text of paddingX 1 with the logo's two
         * rows before it: the hint continues the logo's second row and wraps in the width as pi's text does (one column of padding
         * each side; the logo and its space take five of the first row), a "\n" ends a row for good. The keys are in the dim colour,
         * what they do and the dots in the muted one (pi's keyHint): \001 starts a key, \002 what it does */
        static const char compact[] =
            "\001escape\002 interrupt \302\267 \001ctrl+c/ctrl+d\002 clear/exit \302\267 \001/\002 commands \302\267 "
            "\001!\002 bash \302\267 \001ctrl+o\002 more";
        static const char expanded1[] =
            "\001escape\002 to interrupt\n\001ctrl+c\002 to clear\n\001ctrl+c twice\002 to exit\n\001ctrl+d\002 to exit (empty)\n"
            "\001ctrl+z\002 to suspend\n\001ctrl+k\002 to delete to end\n\001shift+tab\002 to cycle thinking level\n"
            "\001ctrl+p/shift+ctrl+p\002 to cycle models\n\001ctrl+l\002 to select model\n\001ctrl+o\002 to expand tools\n"
            "\001ctrl+t\002 to expand thinking\n\001ctrl+g\002 for external editor\n\001/\002 for commands\n\001!\002 to run bash\n";
        static const char expanded2[] =
            "\001!!\002 to run bash (no context)\n\001alt+enter\002 to queue follow-up\n"
            "\001alt+up\002 to edit all queued messages\n\001ctrl+v\002 to paste files on macOS, images, or text\n"
            "\001drop files\002 to attach";
        char muted[48], key[48], cur[48], both[1024];
        const char *hint = compact;
        long n, a = 0, cut, i, k;
        int first = 1;
        if (s->expanded) {
            cl_copy(both, expanded1, sizeof(both));         /* (a C90 literal is 509 characters at most) */
            cl_cat(both, expanded2, sizeof(both));
            hint = both;
        }
        n = (long)strlen(hint);
        sg(s, PR_MUTED, muted);
        sg(s, PR_DIM, key);
        cl_copy(cur, key, sizeof(cur));
        while (a <= n) {
            long e = a;
            while (e < n && hint[e] != '\n')
                e++;
            for (i = a; i < e;) {                 /* a paragraph in rows */
                int w = 0, room = s->t->cols - 2 - (first ? 5 : 0);
                cut = 0;
                for (k = i; k < e;) {
                    unsigned long cp;
                    int cl, cw;
                    if (hint[k] == 1 || hint[k] == 2) {
                        k++;
                        continue;
                    }
                    cl = vw_char(hint + k, e - k, &cp);
                    cw = vw_cp_width(cp);
                    if (w + cw > room)
                        break;
                    if (cp == ' ')
                        cut = k;
                    w += cw;
                    k += cl;
                }
                if (k >= e)
                    cut = e;
                else if (cut <= i)
                    cut = k;
                if (!first) {
                    raw(s, " ");
                    raw(s, cur);
                }
                for (k = i; k < cut; k++) {
                    if (hint[k] == 1) {
                        raw(s, key);
                        cl_copy(cur, key, sizeof(cur));
                    } else if (hint[k] == 2) {
                        raw(s, muted);
                        cl_copy(cur, muted, sizeof(cur));
                    } else
                        jw_raw(&s->batch, hint + k, 1);
                }
                raw(s, SGR0 "\n");
                first = 0;
                i = cut;
                while (i < e && hint[i] == ' ')
                    i++;
            }
            if (e >= n)
                break;
            a = e + 1;
            cl_copy(cur, key, sizeof(cur));      /* each hint starts with its key */
            if (a > n)
                break;
        }
    }
    if (!s->expanded) {
        char t[200];
        cl_copy(t, "Press ctrl+o to show full startup help", sizeof(t));
        if (s->details)
            cl_cat(t, " and loaded resources", sizeof(t));
        cl_cat(t, ".", sizeof(t));
        say_pad(s, 1, PR_DIM, "", t);
        blank(s);
    } else
        blank(s);
    say_pad(s, 1, PR_DIM, "", "Pi can explain its own features and look up its docs. Ask it how to use or extend Pi.");
    blank(s);                       /* the header container's closing Spacer(1) (interactive-mode.ts:1075): the chat's first block follows it */
    s->any = 0;
    s->after_block = 1;
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

static void d_note_rows(pi_show *s, int role, const char *text)
{
    spacer_item(s);
    say_pad(s, 1, role, "", text);     /* pi's status lines are Text(.., 1, 0), not outputPad */
}

static void d_note(pi_show *s, int role, const char *text)
{
    d_note_rows(s, role, text);
    out(s);
}

/* a dim status drawn, its place in the transcript kept for showStatus to put the next one there */
static void status_draw(pi_show *s, ps_item *it)
{
    it->sany = s->any;
    it->safter = s->after_block;
    it->mark = s->t->n_lines;
    d_note(s, (int)it->a, it->x.p ? it->x.p : "");
    it->end = s->t->n_lines;
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

/* renderers/read.ts:28-34 formatReadLineRange: ":<start>-<end>" in the warning colour when offset or limit is there (a null is none) */
static void line_range(const pi_show *s, jv a, char *l, long cap)
{
    jv o, li;
    int has_o = json_get(a, "offset", &o) && json_type(o) == J_NUM, has_l = json_get(a, "limit", &li) && json_type(li) == J_NUM;
    char b[48], n[16];
    long off;
    if (!has_o && !has_l)
        return;
    off = has_o ? json_long(o, 1) : 1;
    cl_cat(l, sg(s, PR_WARNING, b), cap);
    cl_cat(l, ":", cap);
    cl_ltoa(off, n);
    cl_cat(l, n, cap);
    if (has_l) {
        cl_cat(l, "-", cap);
        cl_ltoa(off + json_long(li, 1) - 1, n);
        cl_cat(l, n, cap);
    }
    cl_cat(l, SGR0, cap);
}

/* the path of a call made absolute against the working directory, "." and ".." resolved (pi's resolveToCwd) */
static void abs_path(const pi_show *s, const char *raw_path, char *out, long cap)
{
    char tmp[800];
    long i, k = 0, n;
    int rooted = raw_path[0] == '/' || (strchr(raw_path, ':') && (!strchr(raw_path, '/') || strchr(raw_path, ':') < strchr(raw_path, '/')));
    tmp[0] = 0;
    if (!rooted) {
        cl_copy(tmp, s->root, sizeof(tmp));
        n = (long)strlen(tmp);
        if (n && tmp[n - 1] != '/' && tmp[n - 1] != ':')
            cl_cat(tmp, "/", sizeof(tmp));
    }
    cl_cat(tmp, raw_path, sizeof(tmp));
    /* segments: "." dropped, ".." takes the one before it */
    n = (long)strlen(tmp);
    i = 0;
    out[0] = 0;
    {
        long start = 0;
        /* keep a leading "/" or "Volume:" */
        if (tmp[0] == '/')
            start = 1;
        else {
            const char *c = strchr(tmp, ':');
            if (c && (!strchr(tmp, '/') || c < strchr(tmp, '/')))
                start = (long)(c - tmp) + 1;
        }
        if (start >= cap)
            start = cap - 1;
        memcpy(out, tmp, (size_t)start);
        k = start;
        i = start;
        while (i < n) {
            long e = i, seglen;
            while (e < n && tmp[e] != '/')
                e++;
            seglen = e - i;
            if (seglen == 0 || (seglen == 1 && tmp[i] == '.'))
                ;
            else if (seglen == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
                if (k > start) {
                    k--;
                    while (k > start && out[k - 1] != '/')
                        k--;
                    if (k > start)
                        k--;            /* the "/" before it */
                }
            } else {
                if (k > start && k < cap - 1)
                    out[k++] = '/';
                else if (k == start && start > 0 && out[start - 1] != '/' && out[start - 1] != ':' && k < cap - 1)
                    out[k++] = '/';
                if (k + seglen < cap) {
                    memcpy(out + k, tmp + i, (size_t)seglen);
                    k += seglen;
                }
            }
            i = e + 1;
        }
    }
    out[k] = 0;
}

/* renderers/read.ts:67-111: a skill's SKILL.md or a context file (AGENTS.md, CLAUDE.md ...) read while the box is collapsed is one short
 * line. Appended to l; 0 when the path is neither. (pi's third form, a file of pi's own docs, has no counterpart here: this program
 * ships none.) */
static int read_compact(const pi_show *s, jv a, char *l, long cap)
{
    char raw_path[400], ap[800], label[800], b[48], b2[48];
    const char *base, *slash;
    long rl;
    if (!arg_str(a, "file_path", raw_path, sizeof(raw_path)) || !raw_path[0])
        if (!arg_str(a, "path", raw_path, sizeof(raw_path)) || !raw_path[0])
            return 0;
    abs_path(s, raw_path, ap, sizeof(ap));
    slash = strrchr(ap, '/');
    if (!slash)
        slash = strrchr(ap, ':');
    base = slash ? slash + 1 : ap;
    if (!strcmp(base, "SKILL.md")) {
        /* basename(dirname(path)) */
        const char *end = slash ? slash : ap, *st = end;
        while (st > ap && st[-1] != '/' && st[-1] != ':')
            st--;
        cl_copy(label, "", sizeof(label));
        if (end > st) {
            long n = (long)(end - st);
            if (n >= (long)sizeof(label))
                n = (long)sizeof(label) - 1;
            memcpy(label, st, (size_t)n);
            label[n] = 0;
        } else
            cl_copy(label, base, sizeof(label));
        cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_LABEL, b), cap);
        cl_cat(l, "\033[1m[skill]\033[22m ", cap);
        cl_cat(l, SGR0, cap);
        cl_cat(l, sg(s, PR_CUSTOM_MESSAGE_TEXT, b2), cap);
        cl_cat(l, label, cap);
        cl_cat(l, SGR0, cap);
    } else if (!strcmp(base, "AGENTS.override.md") || !strcmp(base, "AGENTS.md") || !strcmp(base, "AGENTS.MD") ||
               !strcmp(base, "CLAUDE.md") || !strcmp(base, "CLAUDE.MD")) {
        rl = (long)strlen(s->root);
        if (rl && !strncmp(ap, s->root, (size_t)rl) && ap[rl] == '/' && ap[rl + 1])
            cl_copy(label, ap + rl + 1, sizeof(label));     /* formatPathRelativeToCwdOrAbsolute */
        else
            cl_copy(label, ap, sizeof(label));
        cl_cat(l, BOLD, cap);
        cl_cat(l, sg(s, PR_TOOL_TITLE, b), cap);
        cl_cat(l, "read resource", cap);
        cl_cat(l, SGR0 " ", cap);
        cl_cat(l, sg(s, PR_ACCENT, b2), cap);
        cl_cat(l, label, cap);
        cl_cat(l, SGR0, cap);
    } else
        return 0;
    line_range(s, a, l, cap);
    cl_cat(l, sg(s, PR_DIM, b), cap);
    cl_cat(l, " (ctrl+o to expand)", cap);
    cl_cat(l, SGR0, cap);
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
        if (!strcmp(name, "read") && !s->expanded && read_compact(s, a, l, sizeof(l))) {
            raw(s, l);
            raw(s, "\n");
            return;
        }
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
        if (!strcmp(name, "read"))
            line_range(s, a, l, sizeof(l));
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

/* ---- text with colours in it: highlighted code and the edit diff ---- */

typedef struct sgr_ev {
    long at, src, len;          /* an SGR sequence of the text: before plain byte `at`, at z[src ..) */
} sgr_ev;

/* one line of text that holds SGR sequences (ESC [ ... m) in rows of at most width columns, as show_wrap breaks the plain text; each row
 * after pre. What is in force at the end of a row is set again at the start of the next, as pi-tui's wrapTextWithAnsi does */
static void wrap_sgr(pi_show *s, const char *pre, const char *z, long n, int width)
{
    jw pl;
    sgr_ev *ev = 0;
    long nev = 0, cap = 0, i = 0, a = 0, e, ei = 0;
    char act[512];
    jw_init(&pl);
    while (i < n) {
        if (z[i] == '\033' && i + 1 < n && z[i + 1] == '[') {
            long j = i + 2;
            while (j < n && ((z[j] >= '0' && z[j] <= '9') || z[j] == ';'))
                j++;
            if (j < n && z[j] == 'm') {
                if (nev == cap) {
                    sgr_ev *x = (sgr_ev *)realloc(ev, (size_t)(cap ? cap * 2 : 32) * sizeof(*ev));
                    if (!x)
                        break;
                    ev = x;
                    cap = cap ? cap * 2 : 32;
                }
                ev[nev].at = pl.n;
                ev[nev].src = i;
                ev[nev].len = j + 1 - i;
                nev++;
                i = j + 1;
                continue;
            }
        }
        jw_raw(&pl, z + i, 1);
        i++;
    }
    if (width < 8)
        width = 8;
    e = pl.n;
    act[0] = 0;
    if (!e) {
        raw(s, pre);
        raw(s, "\n");
    }
    while (a < e) {
        long k = show_row_end(pl.p, a, e, width), b;
        while (ei < nev && ev[ei].at < a) {         /* before the row (in the spaces that were skipped): only the style changes */
            long l = sgr_reset_len(z + ev[ei].src, ev[ei].len);
            if (l)
                act[0] = 0;
            if (strlen(act) + (size_t)ev[ei].len < sizeof(act))
                strncat(act, z + ev[ei].src, (size_t)ev[ei].len);
            ei++;
        }
        raw(s, pre);
        raw(s, act);
        for (b = a; b < k; b++) {
            while (ei < nev && ev[ei].at == b) {
                long l = sgr_reset_len(z + ev[ei].src, ev[ei].len);
                jw_raw(&s->batch, z + ev[ei].src, ev[ei].len);
                if (l)
                    act[0] = 0;
                if (strlen(act) + (size_t)ev[ei].len < sizeof(act))
                    strncat(act, z + ev[ei].src, (size_t)ev[ei].len);
                ei++;
            }
            jw_raw(&s->batch, pl.p + b, 1);
        }
        raw(s, SGR0 "\n");
        a = k;
        while (a < e && pl.p[a] == ' ')
            a++;
    }
    free(ev);
    jw_free(&pl);
}

/* theme.ts getLanguageFromPath: the language of the last dot's extension (or of the whole name), 0 none */
static const char *lang_of_path(const char *path)
{
    static const char *const tab[] = {
        "ts", "typescript", "tsx", "typescript", "js", "javascript", "jsx", "javascript", "mjs", "javascript", "cjs", "javascript",
        "py", "python", "rb", "ruby", "rs", "rust", "go", "go", "java", "java", "kt", "kotlin", "swift", "swift", "c", "c", "h", "c",
        "cpp", "cpp", "cc", "cpp", "cxx", "cpp", "hpp", "cpp", "cs", "csharp", "php", "php", "sh", "bash", "bash", "bash", "zsh", "bash",
        "fish", "fish", "ps1", "powershell", "sql", "sql", "html", "html", "htm", "html", "css", "css", "scss", "scss", "sass", "sass",
        "less", "less", "json", "json", "yaml", "yaml", "yml", "yaml", "toml", "toml", "xml", "xml", "md", "markdown", "markdown", "markdown",
        "dockerfile", "dockerfile", "makefile", "makefile", "cmake", "cmake", "lua", "lua", "perl", "perl", "r", "r", "scala", "scala",
        "clj", "clojure", "ex", "elixir", "exs", "elixir", "erl", "erlang", "hs", "haskell", "ml", "ocaml", "vim", "vim", "graphql", "graphql",
        "proto", "protobuf", "tf", "hcl", "hcl", "hcl", 0
    };
    const char *dot = strrchr(path, '.'), *e = dot ? dot + 1 : path;
    char lo[24];
    size_t n = strlen(e), i;
    int k;
    if (!n || n >= sizeof(lo))
        return 0;
    for (i = 0; i < n; i++)
        lo[i] = e[i] >= 'A' && e[i] <= 'Z' ? (char)(e[i] + 32) : e[i];
    lo[n] = 0;
    for (k = 0; tab[k]; k += 2)
        if (!strcmp(tab[k], lo))
            return tab[k + 1];
    return 0;
}

typedef struct hl_out {
    pi_show *s;
    jw *o;
    int depth, known;
} hl_out;

static void hl_token(void *u, int cls, const char *p, long n)
{
    hl_out *h = (hl_out *)u;
    char sgr[64];
    int l = hl_sgr(sgr, &h->s->hlt.s[cls == HL_PLAIN && !h->known ? MD_CODEBLOCK : cls], h->depth);
    jw_raw(h->o, sgr, l);
    jw_raw(h->o, p, n);
}

/* the lines of text as renderers/read.ts:121-136 and write.ts:96-127 show a file's text: tabs three spaces, CRs gone, the trailing empty
 * lines cut, at most max lines (max < 0: all; toolOutputExpanded) written as rows after pre: with a language (theme.ts
 * getLanguageFromPath) highlighted (the Markdown code block colour when the lexer has no such language), else in the output colour.
 * *total is the number of lines before max cut them. */
static void code_rows(pi_show *s, const char *text, long n, const char *lang, int max, long *total)
{
    jw t, line;
    long i, a = 0, end, nl = 0, shown = 0;
    char b[48];
    const hl_lang *L = lang ? hl_find(lang, (long)strlen(lang)) : 0;
    hl_state st;
    hl_out ho;
    jw_init(&t);
    for (i = 0; i < n; i++) {
        if (text[i] == '\r')
            continue;
        if (text[i] == '\t')
            jw_rawz(&t, "   ");
        else
            jw_raw(&t, text + i, 1);
    }
    end = t.n;
    /* the lines are text.split("\n"); the empty ones at the end go */
    while (end > 0 && t.p[end - 1] == '\n')
        end--;
    for (i = 0; i <= end && end > 0; i++)
        if (i == end || t.p[i] == '\n')
            nl++;
    *total = nl;
    ho.s = s;
    ho.depth = s->th->depth >= 256 ? s->th->depth : 16;
    ho.known = L != 0;
    hl_begin(&st, L);
    jw_init(&line);
    for (i = 0; i <= end && end > 0 && (max < 0 || shown < max); i++) {
        if (i != end && t.p[i] != '\n')
            continue;
        jw_reset(&line);
        if (lang) {
            ho.o = &line;
            hl_line(&st, t.p + a, i - a, hl_token, &ho);
            jw_rawz(&line, SGR0);
        } else {
            jw_rawz(&line, sg(s, PR_TOOL_OUTPUT, b));
            jw_raw(&line, t.p + a, i - a);
            jw_rawz(&line, SGR0);
        }
        wrap_sgr(s, pd(s), line.p ? line.p : "", line.n, (int)pwid(s));
        a = i + 1;
        shown++;
    }
    jw_free(&line);
    jw_free(&t);
}

/* the warning rows of a result's details (renderers/read.ts:138-147): one row in the warning colour */
static void warn_row(pi_show *s, const char *msg)
{
    char b[48];
    show_wrap(&s->batch, pd(s), pd(s), sg(s, PR_WARNING, b), msg, (long)strlen(msg), pwid(s));
}

/* a number member of a details object, or def */
static long det_num(jv o, const char *key, long def)
{
    jv v;
    return json_type(o) == J_OBJ && json_get(o, key, &v) && json_type(v) == J_NUM ? json_long(v, def) : def;
}

static int det_true(jv o, const char *key)
{
    jv v;
    return json_type(o) == J_OBJ && json_get(o, key, &v) && json_type(v) == J_TRUE;
}

/* details.truncation of a result: the object when it says truncated, else a null jv */
static int det_trunc(jv det, jv *tr)
{
    jv v;
    if (json_type(det) == J_OBJ && json_get(det, "truncation", &v) && det_true(v, "truncated")) {
        *tr = v;
        return 1;
    }
    return 0;
}

/* renderers/read.ts:138-147 */
static void read_warnings(pi_show *s, jv det)
{
    jv tr, v;
    char m[200], num[16], sz[32];
    if (!det_trunc(det, &tr))
        return;
    pi_size(det_num(tr, "maxBytes", PI_MAX_BYTES), sz, sizeof(sz));
    if (det_true(tr, "firstLineExceedsLimit")) {
        cl_copy(m, "[First line exceeds ", sizeof(m));
        cl_cat(m, sz, sizeof(m));
        cl_cat(m, " limit]", sizeof(m));
    } else if (json_get(tr, "truncatedBy", &v) && json_streq(v, "lines")) {
        cl_copy(m, "[Truncated: showing ", sizeof(m));
        cl_ltoa(det_num(tr, "outputLines", 0), num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " of ", sizeof(m));
        cl_ltoa(det_num(tr, "totalLines", 0), num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " lines (", sizeof(m));
        cl_ltoa(det_num(tr, "maxLines", PI_MAX_LINES), num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " line limit)]", sizeof(m));
    } else {
        cl_copy(m, "[Truncated: ", sizeof(m));
        cl_ltoa(det_num(tr, "outputLines", 0), num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " lines shown (", sizeof(m));
        cl_cat(m, sz, sizeof(m));
        cl_cat(m, " limit)]", sizeof(m));
    }
    warn_row(s, m);
}

/* renderers/ls.ts:48-55, find.ts:55-61, grep.ts:58-67: "[Truncated: <limit>, <50.0KB limit>[, some lines truncated]]" */
static void list_warnings(pi_show *s, const char *name, jv det)
{
    jv tr;
    char m[200], num[16], sz[32];
    const char *lkey = !strcmp(name, "ls") ? "entryLimitReached" : !strcmp(name, "find") ? "resultLimitReached" : "matchLimitReached";
    const char *lword = !strcmp(name, "ls") ? " entries limit" : !strcmp(name, "find") ? " results limit" : " matches limit";
    long limit = det_num(det, lkey, 0);
    int k = 0, has_tr = det_trunc(det, &tr), lines_cut = !strcmp(name, "grep") && det_true(det, "linesTruncated");
    if (!limit && !has_tr && !lines_cut)
        return;
    cl_copy(m, "[Truncated: ", sizeof(m));
    if (limit) {
        cl_ltoa(limit, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, lword, sizeof(m));
        k = 1;
    }
    if (has_tr) {
        pi_size(det_num(tr, "maxBytes", PI_MAX_BYTES), sz, sizeof(sz));
        cl_cat(m, k ? ", " : "", sizeof(m));
        cl_cat(m, sz, sizeof(m));
        cl_cat(m, " limit", sizeof(m));
        k = 1;
    }
    if (lines_cut) {
        cl_cat(m, k ? ", " : "", sizeof(m));
        cl_cat(m, "some lines truncated", sizeof(m));
    }
    cl_cat(m, "]", sizeof(m));
    warn_row(s, m);
}

/* ---- renderDiff (components/diff.ts) ---- */

/* parseDiffLine: /^([+-\s])(\s*\d*)\s(.*)$/ -- the line number's span [ns, ne) and where the content starts, 0 when it does not match */
static int diff_parse(const char *l, long n, long *ns, long *ne, long *cs)
{
    long i = 1, j;
    if (n < 1 || !(l[0] == '+' || l[0] == '-' || l[0] == ' ' || l[0] == '\t'))
        return 0;
    while (i < n && (l[i] == ' ' || l[i] == '\t'))
        i++;
    while (i < n && l[i] >= '0' && l[i] <= '9')
        i++;
    /* the greedy \s*\d* gives back until a whitespace follows */
    for (j = i; j >= 1; j--)
        if (j < n && (l[j] == ' ' || l[j] == '\t')) {
            *ns = 1;
            *ne = j;
            *cs = j + 1;
            return 1;
        }
    return 0;
}

/* replaceTabs into o */
static void tabs3(jw *o, const char *p, long n)
{
    long i;
    for (i = 0; i < n; i++)
        if (p[i] == '\t')
            jw_rawz(o, "   ");
        else
            jw_raw(o, p + i, 1);
}

/* renderIntraLineDiff: the changed words in inverse, the first changed part's leading whitespace left out of it */
static void intra_line(const char *a, long an, const char *b, long bn, jw *ra, jw *rb)
{
    jd_res r;
    long i;
    int first_a = 1, first_b = 1;
    if (jd_words(a, an, b, bn, &r)) {
        jw_raw(ra, a, an);
        jw_raw(rb, b, bn);
        return;
    }
    for (i = 0; i < r.n; i++) {
        const jd_part *p = &r.p[i];
        if (p->removed || p->added) {
            jw *o = p->removed ? ra : rb;
            int *first = p->removed ? &first_a : &first_b;
            const char *v = p->v;
            long n = p->n;
            if (*first) {
                long ws = jd_leading_ws(v, n);
                jw_raw(o, v, ws);
                v += ws;
                n -= ws;
                *first = 0;
            }
            if (n) {
                jw_rawz(o, "\033[7m");
                jw_raw(o, v, n);
                jw_rawz(o, "\033[27m");
            }
        } else {
            jw_raw(ra, p->v, p->n);
            jw_raw(rb, p->v, p->n);
        }
    }
    jd_free(&r);
}

/* a diff line in a role's colour, wrapped */
static void diff_row(pi_show *s, int role, const char *sign_num, long snn, const char *content, long cn)
{
    jw l;
    char b[48];
    jw_init(&l);
    jw_rawz(&l, sg(s, role, b));
    jw_raw(&l, sign_num, snn);
    jw_rawz(&l, " ");
    jw_raw(&l, content, cn);
    wrap_sgr(s, pd(s), l.p ? l.p : "", l.n, (int)pwid(s));
    jw_free(&l);
}

/* the end of the line at p when it is a diff line with this sign, else -1 */
static long diff_line(const char *d, long n, long p, int sign)
{
    long e = p, a, b, c;
    if (p > n)
        return -1;
    while (e < n && d[e] != '\n')
        e++;
    if (!diff_parse(d + p, e - p, &a, &b, &c) || d[p] != sign)
        return -1;
    return e;
}

/* one diff line (sign, number, content) in a role's colour, the tabs in the content three spaces */
static void diff_plain(pi_show *s, int role, const char *l, long n)
{
    jw t;
    long ns, ne, cs;
    jw_init(&t);
    diff_parse(l, n, &ns, &ne, &cs);
    tabs3(&t, l + cs, n - cs);
    diff_row(s, role, l, ne, t.p ? t.p : "", t.n);
    jw_free(&t);
}

/* renderDiff: context lines dim, removed red, added green; a single removed line followed by a single added one with the changed words
 * marked */
static void render_diff(pi_show *s, const char *d, long n)
{
    long i = 0;
    while (i <= n) {
        long e = i, ns, ne, cs;
        while (e < n && d[e] != '\n')
            e++;
        if (!diff_parse(d + i, e - i, &ns, &ne, &cs)) {
            jw l;
            char b[48];
            jw_init(&l);
            jw_rawz(&l, sg(s, PR_TOOL_DIFF_CONTEXT, b));
            jw_raw(&l, d + i, e - i);
            wrap_sgr(s, pd(s), l.p ? l.p : "", l.n, (int)pwid(s));
            jw_free(&l);
        } else if (d[i] == '-') {
            long rs[64], re[64], as[64], ae[64], nr = 0, na = 0, p = i, k, f;
            while (nr < 64 && (f = diff_line(d, n, p, '-')) >= 0) {
                rs[nr] = p;
                re[nr++] = f;
                p = f + 1;
            }
            while (na < 64 && (f = diff_line(d, n, p, '+')) >= 0) {
                as[na] = p;
                ae[na++] = f;
                p = f + 1;
            }
            if (nr == 1 && na == 1) {
                jw ta, tb, ra, rb;
                long ns2, ne2, cs2, ns3, ne3, cs3;
                jw_init(&ta);
                jw_init(&tb);
                jw_init(&ra);
                jw_init(&rb);
                diff_parse(d + rs[0], re[0] - rs[0], &ns2, &ne2, &cs2);
                diff_parse(d + as[0], ae[0] - as[0], &ns3, &ne3, &cs3);
                tabs3(&ta, d + rs[0] + cs2, re[0] - rs[0] - cs2);
                tabs3(&tb, d + as[0] + cs3, ae[0] - as[0] - cs3);
                intra_line(ta.p ? ta.p : "", ta.n, tb.p ? tb.p : "", tb.n, &ra, &rb);
                diff_row(s, PR_TOOL_DIFF_REMOVED, d + rs[0], ne2, ra.p ? ra.p : "", ra.n);
                diff_row(s, PR_TOOL_DIFF_ADDED, d + as[0], ne3, rb.p ? rb.p : "", rb.n);
                jw_free(&ta);
                jw_free(&tb);
                jw_free(&ra);
                jw_free(&rb);
            } else {
                for (k = 0; k < nr; k++)
                    diff_plain(s, PR_TOOL_DIFF_REMOVED, d + rs[k], re[k] - rs[k]);
                for (k = 0; k < na; k++)
                    diff_plain(s, PR_TOOL_DIFF_ADDED, d + as[k], ae[k] - as[k]);
            }
            i = p;
            continue;
        } else
            diff_plain(s, d[i] == '+' ? PR_TOOL_DIFF_ADDED : PR_TOOL_DIFF_CONTEXT, d + i, e - i);
        i = e + 1;
    }
}
static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live, jv det);
static void result_lines(pi_show *s, const char *text, int role, int max);
static void read_rows(pi_show *s, jv a, const char *text);
static void write_body(pi_show *s, jv a);

/* the rows of a tool's box into the batch: pi's ToolExecutionComponent, a Box(outputPad, 1) holding the call's header
 * and the result renderer's rows, over toolPendingBg until the result is final, then toolSuccessBg / toolErrorBg. With
 * toolOutputExpanded (s->expanded) the result renderers show everything they have (renderers/{read,bash,ls,find,grep}.ts) */
static void box_build(pi_show *s, const ps_box *b)
{
    jw text;
    jv a, det;
    long from = s->batch.n;
    int replay = s->replay, bg, has_diff = 0;
    jv dv;
    s->replay = b->replay;
    if (json_parse(b->args.p, b->args.n, &a))
        a.p = 0, a.n = 0;
    det.p = 0;
    det.n = 0;
    if (b->det.n && json_parse(b->det.p, b->det.n, &det))
        det.p = 0, det.n = 0;
    blank(s);
    header_line(s, b->name, a);
    if (!strcmp(b->name, "write"))
        write_body(s, a);
    jw_init(&text);
    if (!b->done) {
        /* running: bash shows its output so far and "Elapsed" (bash.ts:61-111); pi's renderResult runs only once a
         * partial result exists, and the bash tool sends an empty one when it starts (core/tools/bash.ts:318) */
        if (b->elapsed) {
            det.p = 0;
            bash_result(s, b->partial.p ? b->partial.p : "", clock_ms(s) - b->t0, -1, 1, det);
        }
    } else {
        pshow_result_text(b->result.p ? b->result.p : "", b->result.n, &text);
        jw_rawz(&text, "");
        if (!strcmp(b->name, "bash"))
            bash_result(s, text.p ? text.p : "", 0, b->duration_ms, 0, det);
        else if (b->is_error && text.n) {
            blank(s);
            say(s, PR_ERROR, "", text.p);
        } else if (!strcmp(b->name, "read")) {
            if (s->expanded) {          /* renderers/read.ts:121-124: collapsed, the result shows nothing */
                read_rows(s, a, text.p ? text.p : "");
                read_warnings(s, det);
            }
        } else if (!strcmp(b->name, "ls") || !strcmp(b->name, "find")) {
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 20);
            list_warnings(s, b->name, det);
        } else if (!strcmp(b->name, "grep")) {
            result_lines(s, text.p ? text.p : "", PR_TOOL_OUTPUT, 15);
            list_warnings(s, b->name, det);
        } else if (!strcmp(b->name, "edit") && json_type(det) == J_OBJ && json_get(det, "diff", &dv) && json_type(dv) == J_STR) {
            /* renderers/edit.ts:143-150: the call component holds the diff under the call, after a blank row */
            long dn;
            char *d = json_strdup(dv, &dn);
            has_diff = 1;
            if (d) {
                blank(s);
                render_diff(s, d, dn);
            }
            free(d);
        }
    }
    blank(s);                       /* the box's bottom padding */
    jw_free(&text);
    s->replay = replay;
    /* renderers/edit.ts:115-130: the edit box is coloured by its diff: without one, a result that did not fail leaves it pending */
    bg = !b->done ? PR_TOOL_PENDING_BG : b->is_error ? PR_TOOL_ERROR_BG : !strcmp(b->name, "edit") && !has_diff ? PR_TOOL_PENDING_BG : PR_TOOL_SUCCESS_BG;
    panel(s, from, bg);
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

/* renderers/read.ts:121-136, expanded: every line of the file as the tool returned it, trailing empty lines cut, tabs three spaces, a file
 * with a known language highlighted (theme.ts getLanguageFromPath), else in the output colour */
static void read_rows(pi_show *s, jv a, const char *text)
{
    char path[400];
    const char *lang = 0;
    long total;
    if ((arg_str(a, "file_path", path, sizeof(path)) && path[0]) || (arg_str(a, "path", path, sizeof(path)) && path[0]))
        lang = lang_of_path(path);
    blank(s);
    code_rows(s, text, (long)strlen(text), lang, -1, &total);
}

/* renderers/write.ts:96-127: the content under the call, after a blank row: ten lines (all when expanded) highlighted for a known
 * language, "... (N more lines, T total, ctrl+o to expand)" below when there are more */
static void write_body(pi_show *s, jv a)
{
    char path[400], b[48];
    jv v;
    const char *lang = 0;
    char *content;
    long cn, total, rest;
    if (!json_get(a, "content", &v) || json_type(v) == J_NULL)
        return;
    if (json_type(v) != J_STR) {
        blank(s);
        raw(s, pd(s));
        raw(s, sg(s, PR_ERROR, b));
        raw(s, "[invalid content arg - expected string]" SGR0 "\n");
        return;
    }
    content = json_strdup(v, &cn);
    if (!content || !cn) {
        free(content);
        return;
    }
    if ((arg_str(a, "file_path", path, sizeof(path)) && path[0]) || (arg_str(a, "path", path, sizeof(path)) && path[0]))
        lang = lang_of_path(path);
    blank(s);
    code_rows(s, content, cn, lang, s->expanded ? -1 : 10, &total);
    rest = total - 10;
    if (!s->expanded && rest > 0) {
        char m[96], num[16];
        cl_copy(m, "... (", sizeof(m));
        cl_ltoa(rest, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " more lines, ", sizeof(m));
        cl_ltoa(total, num);
        cl_cat(m, num, sizeof(m));
        cl_cat(m, " total, ", sizeof(m));
        hint_row(s, m, " to expand)");
    }
    free(content);
}

/* bash: the last five lines of the output, "... (N earlier lines, ctrl+o to expand)" above when more (expanded: all of
 * it, renderers/bash.ts:60-77), then how long it took (live: how long it has run so far) */
static void bash_result(pi_show *s, const char *text, unsigned long ms, long recorded, int live, jv det)
{
    char b[48], dur[24], m[96], num[16];
    long n = (long)strlen(text), a = 0, i, nrows = 0, skip = 0, row = 0, start = 0;
    jv tr, pv;
    char *full = 0;
    int cut = !live && det_trunc(det, &tr);
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r'))
        n--;
    while (a < n && (text[a] == '\n' || text[a] == ' ' || text[a] == '\t'))
        a++;
    if (!live && json_type(det) == J_OBJ && json_get(det, "fullOutputPath", &pv) && json_type(pv) == J_STR)
        full = json_strdup(pv, 0);
    /* renderers/bash.ts:53-60: the tool's own "[... Full output: <path>]" footer is left out of the output, the warning row below says it */
    if (cut && full && n > 0 && text[n - 1] == ']') {
        long f;
        for (f = n - 3; f >= 0; f--)
            if (text[f] == '\n' && text[f + 1] == '\n' && text[f + 2] == '[')
                break;
        if (f >= 0) {
            char *tail = (char *)malloc((size_t)(n - f) + 1);
            if (tail) {
                memcpy(tail, text + f, (size_t)(n - f));
                tail[n - f] = 0;
                if (strstr(tail, full)) {
                    n = f;
                    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r'))
                        n--;
                }
                free(tail);
            }
        }
    }
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
    /* renderers/bash.ts:85-100 */
    if (cut || full) {
        char w[400], sz[32], num[16];
        w[0] = 0;
        cl_copy(w, "[", sizeof(w));
        if (full) {
            cl_cat(w, "Full output: ", sizeof(w));
            cl_cat(w, full, sizeof(w));
        }
        if (cut) {
            jv v;
            if (full)
                cl_cat(w, ". ", sizeof(w));
            if (json_get(tr, "truncatedBy", &v) && json_streq(v, "lines")) {
                cl_cat(w, "Truncated: showing ", sizeof(w));
                cl_ltoa(det_num(tr, "outputLines", 0), num);
                cl_cat(w, num, sizeof(w));
                cl_cat(w, " of ", sizeof(w));
                cl_ltoa(det_num(tr, "totalLines", 0), num);
                cl_cat(w, num, sizeof(w));
                cl_cat(w, " lines", sizeof(w));
            } else {
                pi_size(det_num(tr, "maxBytes", PI_MAX_BYTES), sz, sizeof(sz));
                cl_cat(w, "Truncated: ", sizeof(w));
                cl_ltoa(det_num(tr, "outputLines", 0), num);
                cl_cat(w, num, sizeof(w));
                cl_cat(w, " lines shown (", sizeof(w));
                cl_cat(w, sz, sizeof(w));
                cl_cat(w, " limit)", sizeof(w));
            }
        }
        cl_cat(w, "]", sizeof(w));
        blank(s);
        warn_row(s, w);
    }
    free(full);
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

void pshow_tool_end(pi_show *s, const char *id, const char *result, long rn, int is_error, long duration_ms, const char *details, long dn)
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
    jw_reset(&b->det);
    if (details && dn > 0)
        jw_raw(&b->det, details, dn);
    jw_rawz(&b->det, "");
    b->det.n = details && dn > 0 ? dn : 0;
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

/* the SGR sequence of a theme colour by its name (pi_role_names: "accent", "customMessageLabel", ...), for an extension's renderer */
static int role_sgr(void *u, const char *name, int bg, char *buf, long cap)
{
    const pi_show *s = (const pi_show *)u;
    char b[48];
    int i;
    (void)bg;
    for (i = 0; i < PR_COUNT; i++)
        if (!strcmp(pi_role_names[i], name)) {
            cl_copy(buf, sg(s, i, b), cap);
            return 1;
        }
    return 0;
}

/* a custom message (a session's custom_message entry) with display true. An extension's renderer for its customType draws it
 * (custom-message.ts:80-98: a Spacer(1), then the component the renderer made, which does its own styling); else its customType is
 * the label and its text Markdown over customMessageBg */
static void d_custom(pi_show *s, const char *type, const char *text, long n, const char *msg, long msg_n)
{
    long from;
    if (s->render && msg && msg_n) {
        pi_ext_render r;
        jw lines;
        int made;
        jw_init(&lines);
        r.width = s->t->cols;
        r.expanded = s->expanded;
        r.pad = s->pad;
        r.sgr = role_sgr;
        r.u = s;
        made = s->render(s->render_u, type, msg, msg_n, &r, &lines);
        if (made && !lines.oom) {
            blank(s);                       /* the component's Spacer(1), as box_begin's */
            s->any = 1;
            jw_raw(&s->batch, lines.p, lines.n);
            out(s);
            jw_free(&lines);
            return;
        }
        jw_free(&lines);
    }
    from = box_begin(s, type);
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
                pshow_custom(s, ty, w.p ? w.p : "", w.n, msg[i], msg_n[i]);
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
                    {
                        jv dt;
                        int has_det = json_get(m, "details", &dt) && json_type(dt) == J_OBJ;
                        if (json_get(m, "content", &c))
                            pshow_tool_end(s, calls[k].id, c.p, c.n, err, dur, has_det ? dt.p : 0, has_det ? dt.n : 0);
                        else
                            pshow_tool_end(s, calls[k].id, "[]", 2, err, dur, has_det ? dt.p : 0, has_det ? dt.n : 0);
                    }
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
        d_note(s, (int)it->a, x);
        break;
    case PK_STATUS:
        status_draw(s, it);
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
        d_custom(s, x, y, it->y.n, it->z.p, it->z.n);
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
    ps_item *last = s->nitem ? s->item[s->nitem - 1] : 0, *it;
    if (role == PR_DIM && last && last->kind == PK_STATUS && last->end && s->ins < 0 && !s->nbox && s->t->n_lines == last->end) {
        /* showStatus (interactive-mode.ts:3824-3834): the last child of the chat is the last status, so its text is replaced */
        long mark = last->mark;
        jw_reset(&last->x);
        jw_rawz(&last->x, text);
        s->any = last->sany;
        s->after_block = last->safter;
        jw_reset(&s->batch);
        d_note_rows(s, role, text);
        if (tui_replace_tail(s->t, mark, s->batch.p, s->batch.n) == 0) {
            jw_reset(&s->batch);
            last->end = s->t->n_lines;
            return;
        }
        jw_reset(&s->batch);
        last->end = 0;
        redraw_items(s);
        return;
    }
    it = item_text(s, role == PR_DIM ? PK_STATUS : PK_NOTE, role, text, (long)strlen(text), 0);
    if (it && it->kind == PK_STATUS)
        status_draw(s, it);
    else
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

void pshow_custom(pi_show *s, const char *type, const char *text, long n, const char *msg, long msg_n)
{
    ps_item *it = item_text(s, PK_CUSTOM, 0, type, (long)strlen(type), 0);
    if (it) {
        jw_raw(&it->y, text, n);
        if (msg)
            jw_raw(&it->z, msg, msg_n);
    }
    d_custom(s, type, text, n, msg, msg_n);
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

void pshow_set_root(pi_show *s, const char *root)
{
    cl_copy(s->root, root ? root : "", sizeof(s->root));
    if (s->root[0] && strlen(s->root) > 1 && s->root[strlen(s->root) - 1] == '/')
        s->root[strlen(s->root) - 1] = 0;
}

void pshow_set_pad(pi_show *s, int pad)
{
    s->pad = pad ? 1 : 0;
}
