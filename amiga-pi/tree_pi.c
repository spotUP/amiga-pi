/* see tree_pi.h */
#include <stdlib.h>
#include <string.h>
#include "tree_pi.h"
#include "claude/util.h"
#include "claude/json.h"
#include "view/vw_text.h"

#define SGR0 "\033[0m"
#define BOLD "\033[1m"
#define DIM  "\033[2m"
#define MAXG 24
#define E_COL "\033[39m"          /* the end of a coloured span (pi: theme.fg) */
#define E_DIM "\033[22;39m"       /* the end of a faint one */

typedef struct tgut {
    int pos, show;
} tgut;

typedef struct tnode {
    int idx;                    /* the entry's index in the session */
    int vparent;                /* its row's parent among the visible rows, -1 */
    int indent, conn, last, virt;
    int ng;
    tgut g[MAXG];
} tnode;

typedef struct tview {
    pi_store *st;
    cl_tui *t;
    tui_pick pk;
    int filter;
    char query[96];
    int *order;                 /* every entry in pi's flatten order */
    int norder;
    char *folded;               /* per entry */
    char *active;               /* on the path from the root to the leaf */
    int leaf;
    tnode *vis;                 /* the visible rows */
    int nvis;
    int multi;                  /* several visible roots */
    int stamps;                 /* label times shown */
    int last_sel;               /* entry of the selection */
} tview;

static pi_s3 *S(tview *v)
{
    return &v->st->s3;
}

/* ---- text of an entry ---- */

static int parse_entry(pi_s3 *s, int i, jv *o)
{
    long n;
    const char *t = s3_text(s, i, &n);
    return t && !json_parse(t, n, o);
}

/* the text blocks of a content value, joined (at most 200 bytes, as pi's extractContent) */
static void content_text(jv c, jw *out)
{
    if (json_type(c) == J_STR) {
        long n;
        char *z = json_strdup(c, &n);
        if (z)
            jw_raw(out, z, n);
        free(z);
    } else if (json_type(c) == J_ARR) {
        jit it;
        jv b, t, x;
        json_iter(c, &it);
        while (json_next(&it, 0, &b))
            if (json_type(b) == J_OBJ && json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x) &&
                json_type(x) == J_STR) {
                long n;
                char *z = json_strdup(x, &n);
                if (z)
                    jw_raw(out, z, n);
                free(z);
            }
    }
}

/* \n and \t as spaces, trimmed, at most max bytes (cut at a character) */
static void normalize(jw *w, char *out, long cap, long max)
{
    long i = 0, o = 0, a = 0;
    while (a < w->n && (w->p[a] == ' ' || w->p[a] == '\n' || w->p[a] == '\t'))
        a++;
    for (i = a; i < w->n && o < cap - 5 && o < max; i++) {
        char c = w->p[i];
        out[o++] = c == '\n' || c == '\t' ? ' ' : c;
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static const char *sgr(const cl_pitheme *th, int role, char *b)
{
    pitheme_sgr(th, role, b);
    return b;
}

static int has_text(jv c)
{
    jw w;
    long i;
    int any = 0;
    jw_init(&w);
    content_text(c, &w);
    for (i = 0; i < w.n; i++)
        if (w.p[i] != ' ' && w.p[i] != '\n' && w.p[i] != '\t' && w.p[i] != '\r')
            any = 1;
    jw_free(&w);
    return any;
}

/* [read: path] and friends, from the call a tool result answers */
static void tool_text(pi_s3 *s, int idx, jv msg, char *out, long cap)
{
    jv v;
    char id[80], name[40];
    int j;
    id[0] = 0;
    name[0] = 0;
    if (json_get(msg, "toolCallId", &v))
        json_str(v, id, sizeof(id));
    if (json_get(msg, "toolName", &v))
        json_str(v, name, sizeof(name));
    cl_copy(out, "[", cap);
    cl_cat(out, name[0] ? name : "tool", cap);
    cl_cat(out, "]", cap);
    for (j = idx; j >= 0 && id[0]; j = s->e[j].parent) {
        jv o, m, c, b, t, i2, a;
        jit it;
        if (s->e[j].type != S3_MESSAGE || s->e[j].role != S3R_ASSISTANT || !parse_entry(s, j, &o) ||
            !json_get(o, "message", &m) || !json_get(m, "content", &c) || json_type(c) != J_ARR)
            continue;
        json_iter(c, &it);
        while (json_next(&it, 0, &b)) {
            char bid[80];
            if (!json_get(b, "type", &t) || !json_streq(t, "toolCall") || !json_get(b, "id", &i2))
                continue;
            json_str(i2, bid, sizeof(bid));
            if (strcmp(bid, id))
                continue;
            {
                char nm[40], p[200], pat[120];
                nm[0] = p[0] = pat[0] = 0;
                if (json_get(b, "name", &t))
                    json_str(t, nm, sizeof(nm));
                if (!json_get(b, "arguments", &a))
                    a.p = 0, a.n = 0;
                if (json_type(a) == J_OBJ) {
                    if (json_get(a, "path", &t) || json_get(a, "file_path", &t))
                        json_str(t, p, sizeof(p));
                    if (json_get(a, "pattern", &t))
                        json_str(t, pat, sizeof(pat));
                }
                if (!strcmp(nm, "bash")) {
                    char cmd[80], raw[400];
                    long k;
                    raw[0] = 0;
                    if (json_type(a) == J_OBJ && json_get(a, "command", &t))
                        json_str(t, raw, sizeof(raw));
                    for (k = 0; raw[k] && k < 50; k++)
                        cmd[k] = raw[k] == '\n' || raw[k] == '\t' ? ' ' : raw[k];
                    cmd[k] = 0;
                    cl_copy(out, "[bash: ", cap);
                    cl_cat(out, cmd, cap);
                    cl_cat(out, strlen(raw) > 50 ? "...]" : "]", cap);
                } else if (!strcmp(nm, "grep")) {
                    cl_copy(out, "[grep: /", cap);
                    cl_cat(out, pat, cap);
                    cl_cat(out, "/ in ", cap);
                    cl_cat(out, p[0] ? p : ".", cap);
                    cl_cat(out, "]", cap);
                } else if (!strcmp(nm, "find")) {
                    cl_copy(out, "[find: ", cap);
                    cl_cat(out, pat, cap);
                    cl_cat(out, " in ", cap);
                    cl_cat(out, p[0] ? p : ".", cap);
                    cl_cat(out, "]", cap);
                } else if (!strcmp(nm, "ls")) {
                    cl_copy(out, "[ls: ", cap);
                    cl_cat(out, p[0] ? p : ".", cap);
                    cl_cat(out, "]", cap);
                } else if (!strcmp(nm, "read") || !strcmp(nm, "write") || !strcmp(nm, "edit")) {
                    cl_copy(out, "[", cap);
                    cl_cat(out, nm, cap);
                    cl_cat(out, ": ", cap);
                    cl_cat(out, p, cap);
                    if (!strcmp(nm, "read") && json_type(a) == J_OBJ && (json_get(a, "offset", &t) || json_get(a, "limit", &t))) {
                        jv o2, l2;
                        long off = json_get(a, "offset", &o2) ? json_long(o2, 1) : 1;
                        char num[16];
                        cl_cat(out, ":", cap);
                        cl_ltoa(off, num);
                        cl_cat(out, num, cap);
                        if (json_get(a, "limit", &l2)) {
                            cl_cat(out, "-", cap);
                            cl_ltoa(off + json_long(l2, 1) - 1, num);
                            cl_cat(out, num, cap);
                        }
                    }
                    cl_cat(out, "]", cap);
                } else {
                    cl_copy(out, "[", cap);
                    cl_cat(out, nm, cap);
                    cl_cat(out, "]", cap);
                }
            }
            return;
        }
    }
}

void pi_tree_entry_text(pi_store *st, int idx, const cl_pitheme *th, int selected, jw *out)
{
    pi_s3 *s = &st->s3;
    jv o, m, v, c;
    jw w;
    char b[48], b2[48], text[420], tmp[420];
    jw_init(&w);
    text[0] = 0;
    if (!parse_entry(s, idx, &o))
        goto done;
    switch (s->e[idx].type) {
    case S3_MESSAGE:
        if (!json_get(o, "message", &m))
            break;
        if (s->e[idx].role == S3R_USER) {
            if (json_get(m, "content", &c))
                content_text(c, &w);
            normalize(&w, tmp, sizeof(tmp), 200);
            cl_copy(text, sgr(th, PR_ACCENT, b), sizeof(text));
            cl_cat(text, "user: " E_COL, sizeof(text));
            cl_cat(text, tmp, sizeof(text));
        } else if (s->e[idx].role == S3R_ASSISTANT) {
            char stop[24];
            stop[0] = 0;
            if (json_get(m, "content", &c))
                content_text(c, &w);
            normalize(&w, tmp, sizeof(tmp), 200);
            cl_copy(text, sgr(th, PR_SUCCESS, b), sizeof(text));
            cl_cat(text, "assistant: " E_COL, sizeof(text));
            if (json_get(m, "stopReason", &v))
                json_str(v, stop, sizeof(stop));
            if (tmp[0])
                cl_cat(text, tmp, sizeof(text));
            else if (!strcmp(stop, "aborted")) {
                cl_cat(text, sgr(th, PR_MUTED, b), sizeof(text));
                cl_cat(text, "(aborted)" E_DIM, sizeof(text));
            } else if (json_get(m, "errorMessage", &v) && json_type(v) == J_STR) {
                char e[200];
                jw ew;
                json_str(v, e, sizeof(e));
                jw_init(&ew);
                jw_rawz(&ew, e);
                normalize(&ew, tmp, 81, 80);
                jw_free(&ew);
                cl_cat(text, sgr(th, PR_ERROR, b), sizeof(text));
                cl_cat(text, tmp, sizeof(text));
                cl_cat(text, E_COL, sizeof(text));
            } else {
                cl_cat(text, sgr(th, PR_MUTED, b), sizeof(text));
                cl_cat(text, "(no content)" E_DIM, sizeof(text));
            }
        } else if (s->e[idx].role == S3R_TOOLRESULT) {
            tool_text(s, idx, m, tmp, sizeof(tmp));
            cl_copy(text, sgr(th, PR_MUTED, b), sizeof(text));
            cl_cat(text, tmp, sizeof(text));
            cl_cat(text, E_DIM, sizeof(text));
        } else if (s->e[idx].role == S3R_BASH) {
            char cmd[300];
            cmd[0] = 0;
            if (json_get(m, "command", &v))
                json_str(v, cmd, sizeof(cmd));
            jw_rawz(&w, cmd);
            normalize(&w, tmp, sizeof(tmp), 300);
            cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
            cl_cat(text, "[bash]: ", sizeof(text));
            cl_cat(text, tmp, sizeof(text));
            cl_cat(text, E_DIM, sizeof(text));
        } else {
            cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
            cl_cat(text, "[other]" E_DIM, sizeof(text));
        }
        break;
    case S3_CUSTOM_MESSAGE: {
        char ct[60];
        ct[0] = 0;
        if (json_get(o, "customType", &v))
            json_str(v, ct, sizeof(ct));
        if (json_get(o, "content", &c))
            content_text(c, &w);
        normalize(&w, tmp, sizeof(tmp), 200);
        cl_copy(text, sgr(th, PR_CUSTOM_MESSAGE_LABEL, b), sizeof(text));
        cl_cat(text, "[", sizeof(text));
        cl_cat(text, ct, sizeof(text));
        cl_cat(text, "]: " E_COL, sizeof(text));
        cl_cat(text, tmp, sizeof(text));
        break;
    }
    case S3_COMPACTION: {
        char num[16];
        long tb = json_get(o, "tokensBefore", &v) ? json_long(v, 0) : 0;
        cl_copy(text, sgr(th, PR_BORDER_ACCENT, b), sizeof(text));
        cl_cat(text, "[compaction: ", sizeof(text));
        cl_ltoa((tb + 500) / 1000, num);
        cl_cat(text, num, sizeof(text));
        cl_cat(text, "k tokens]" E_COL, sizeof(text));
        break;
    }
    case S3_BRANCH_SUMMARY: {
        char sum[300];
        sum[0] = 0;
        if (json_get(o, "summary", &v))
            json_str(v, sum, sizeof(sum));
        jw_rawz(&w, sum);
        normalize(&w, tmp, sizeof(tmp), 300);
        cl_copy(text, sgr(th, PR_WARNING, b), sizeof(text));
        cl_cat(text, "[branch summary]: " E_COL, sizeof(text));
        cl_cat(text, tmp, sizeof(text));
        break;
    }
    case S3_MODEL: {
        char id[100];
        id[0] = 0;
        if (json_get(o, "modelId", &v))
            json_str(v, id, sizeof(id));
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[model: ", sizeof(text));
        cl_cat(text, id, sizeof(text));
        cl_cat(text, "]" E_DIM, sizeof(text));
        break;
    }
    case S3_THINKING: {
        char lv[30];
        lv[0] = 0;
        if (json_get(o, "thinkingLevel", &v))
            json_str(v, lv, sizeof(lv));
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[thinking: ", sizeof(text));
        cl_cat(text, lv, sizeof(text));
        cl_cat(text, "]" E_DIM, sizeof(text));
        break;
    }
    case S3_CUSTOM: {
        char ct[60];
        ct[0] = 0;
        if (json_get(o, "customType", &v))
            json_str(v, ct, sizeof(ct));
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[custom: ", sizeof(text));
        cl_cat(text, ct, sizeof(text));
        cl_cat(text, "]" E_DIM, sizeof(text));
        break;
    }
    case S3_CONTEXT_EDIT:
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[context edit]" E_DIM, sizeof(text));
        break;
    case S3_LABEL: {
        char lb[100];
        lb[0] = 0;
        if (json_get(o, "label", &v) && json_type(v) == J_STR)
            json_str(v, lb, sizeof(lb));
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[label: ", sizeof(text));
        cl_cat(text, lb[0] ? lb : "(cleared)", sizeof(text));
        cl_cat(text, "]" E_DIM, sizeof(text));
        break;
    }
    case S3_SESSION_INFO: {
        char nm[100];
        nm[0] = 0;
        if (json_get(o, "name", &v) && json_type(v) == J_STR)
            json_str(v, nm, sizeof(nm));
        cl_copy(text, sgr(th, PR_DIM, b), sizeof(text));
        cl_cat(text, "[title: ", sizeof(text));
        if (nm[0])
            cl_cat(text, nm, sizeof(text));
        else {
            cl_cat(text, "\033[3m", sizeof(text));
            cl_cat(text, "empty", sizeof(text));
        }
        cl_cat(text, "]" E_DIM, sizeof(text));
        break;
    }
    default:
        break;
    }
done:
    (void)b2;
    if (selected) {
        jw_rawz(out, BOLD);
        jw_rawz(out, text);
        jw_rawz(out, "\033[22m");
    } else
        jw_rawz(out, text);
    jw_free(&w);
}

/* what a search looks in: label, role, content (pi getSearchableText) */
static void searchable(tview *v, int i, jw *w)
{
    pi_s3 *s = S(v);
    jv o, m, c, x;
    const char *lb = s3_label_of(s, i);
    if (lb) {
        jw_rawz(w, lb);
        jw_rawz(w, " ");
    }
    if (!parse_entry(s, i, &o))
        return;
    switch (s->e[i].type) {
    case S3_MESSAGE:
        if (json_get(o, "message", &m)) {
            static const char *const rn[] = { "", "user", "assistant", "toolResult", "system", "custom", "bashExecution", "other" };
            jw_rawz(w, rn[s->e[i].role]);
            jw_rawz(w, " ");
            if (json_get(m, "content", &c))
                content_text(c, w);
            if (s->e[i].role == S3R_BASH && json_get(m, "command", &x) && json_type(x) == J_STR) {
                long n;
                char *z = json_strdup(x, &n);
                jw_rawz(w, " ");
                if (z)
                    jw_raw(w, z, n);
                free(z);
            }
        }
        break;
    case S3_CUSTOM_MESSAGE:
        if (json_get(o, "content", &c))
            content_text(c, w);
        break;
    case S3_COMPACTION:
        jw_rawz(w, "compaction");
        break;
    case S3_BRANCH_SUMMARY:
        jw_rawz(w, "branch summary ");
        if (json_get(o, "summary", &x) && json_type(x) == J_STR) {
            long n;
            char *z = json_strdup(x, &n);
            if (z)
                jw_raw(w, z, n);
            free(z);
        }
        break;
    case S3_MODEL:
        jw_rawz(w, "model");
        break;
    case S3_THINKING:
        jw_rawz(w, "thinking");
        break;
    default:
        break;
    }
}

static int lower_has(const char *hay, long hn, const char *tok, long tn)
{
    long i, k;
    for (i = 0; i + tn <= hn; i++) {
        for (k = 0; k < tn; k++) {
            char a = hay[i + k], b = tok[k];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (k == tn)
            return 1;
    }
    return 0;
}

/* ---- the tree ---- */

static void build_active(tview *v)
{
    pi_s3 *s = S(v);
    int j;
    memset(v->active, 0, (size_t)s->n);
    for (j = v->leaf; j >= 0; j = s->e[j].parent)
        v->active[j] = 1;
}

/* the entries in pi's order: depth first, the branch holding the leaf first among siblings */
static int flatten(tview *v)
{
    pi_s3 *s = S(v);
    int *stack, sp = 0, i, k, n = s->n;
    stack = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    if (!stack)
        return -1;
    v->norder = 0;
    /* roots: those holding the leaf first; pushed in reverse so the first comes out first */
    {
        int *roots = (int *)malloc(sizeof(int) * (size_t)(n + 1)), nr = 0, pass;
        if (!roots) {
            free(stack);
            return -1;
        }
        for (pass = 0; pass < 2; pass++)
            for (i = 0; i < n; i++)
                if (s->e[i].parent < 0 && (v->active[i] ? 1 : 0) == (pass == 0))
                    roots[nr++] = i;
        for (i = nr - 1; i >= 0; i--)
            stack[sp++] = roots[i];
        free(roots);
    }
    while (sp > 0) {
        int node = stack[--sp], nc = 0, *ch;
        v->order[v->norder++] = node;
        ch = (int *)malloc(sizeof(int) * (size_t)(n + 1));
        if (!ch) {
            free(stack);
            return -1;
        }
        {
            int pass;
            for (pass = 0; pass < 2; pass++)
                for (k = node + 1; k < n; k++)
                    if (s->e[k].parent == node && (v->active[k] ? 1 : 0) == (pass == 0))
                        ch[nc++] = k;
        }
        for (k = nc - 1; k >= 0; k--)
            stack[sp++] = ch[k];
        free(ch);
    }
    free(stack);
    return 0;
}

static int is_settings(pi_s3 *s, int i)
{
    int t = s->e[i].type;
    return t == S3_LABEL || t == S3_CONTEXT_EDIT || t == S3_CUSTOM || t == S3_MODEL || t == S3_THINKING || t == S3_SESSION_INFO;
}

static int passes(tview *v, int i)
{
    pi_s3 *s = S(v);
    jv o, m, c, x;
    int is_leaf = i == v->leaf;
    if (s->e[i].type == S3_USAGE)
        return 0;
    if (s->e[i].type == S3_MESSAGE && s->e[i].role == S3R_ASSISTANT && !is_leaf && parse_entry(s, i, &o) &&
        json_get(o, "message", &m)) {
        char stop[24];
        int ok_stop;
        stop[0] = 0;
        c.p = 0;
        c.n = 0;
        json_get(m, "content", &c);
        if (json_get(m, "stopReason", &x))
            json_str(x, stop, sizeof(stop));
        ok_stop = !stop[0] || !strcmp(stop, "stop") || !strcmp(stop, "toolUse");
        if (!has_text(c) && ok_stop)
            return 0;
    }
    switch (v->filter) {
    case TF_USER:
        return s->e[i].type == S3_MESSAGE && s->e[i].role == S3R_USER;
    case TF_NO_TOOLS:
        return !is_settings(s, i) && !(s->e[i].type == S3_MESSAGE && s->e[i].role == S3R_TOOLRESULT);
    case TF_LABELED:
        return s3_label_of(s, i) != 0;
    case TF_ALL:
        return 1;
    default:
        return !is_settings(s, i);
    }
}

static int row_of(tview *v, int idx)
{
    int i;
    for (i = 0; i < v->nvis; i++)
        if (v->vis[i].idx == idx)
            return i;
    return -1;
}

static int foldable(tview *v, int row)
{
    int i, kids = 0, sib = 0, p = v->vis[row].vparent;
    for (i = 0; i < v->nvis; i++) {
        if (v->vis[i].vparent == row)
            kids++;
        if (p >= 0 && v->vis[i].vparent == p)
            sib++;
    }
    return kids > 0 && (p < 0 || sib > 1);
}

/* the visible rows: filter, search, folds, then pi's recalculateVisualStructure */
static int refilter(tview *v)
{
    pi_s3 *s = S(v);
    int i, n = s->n, nv = 0, near;
    char *show = (char *)calloc((size_t)n + 1, 1), *skip = (char *)calloc((size_t)n + 1, 1);
    int *rowof = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    int *stk, sp = 0;
    char toks[8][40];
    int ntok = 0;
    long ql = (long)strlen(v->query), a = 0;
    if (!show || !skip || !rowof)
        goto oom;
    /* search tokens */
    while (a < ql && ntok < 8) {
        long e;
        while (a < ql && v->query[a] == ' ')
            a++;
        e = a;
        while (e < ql && v->query[e] != ' ')
            e++;
        if (e > a) {
            long l = e - a < 39 ? e - a : 39;
            memcpy(toks[ntok], v->query + a, (size_t)l);
            toks[ntok++][l] = 0;
        }
        a = e;
    }
    for (i = 0; i < v->norder; i++) {
        int e = v->order[i];
        if (!passes(v, e))
            continue;
        if (ntok) {
            jw w;
            int k, ok = 1;
            jw_init(&w);
            searchable(v, e, &w);
            for (k = 0; k < ntok; k++)
                if (!lower_has(w.p ? w.p : "", w.n, toks[k], (long)strlen(toks[k])))
                    ok = 0;
            jw_free(&w);
            if (!ok)
                continue;
        }
        show[e] = 1;
    }
    /* descendants of folded entries (in the whole tree) */
    for (i = 0; i < n; i++) {
        int p = s->e[i].parent;
        if (p >= 0 && (v->folded[p] || skip[p]))
            skip[i] = 1;
    }
    for (i = 0; i < v->norder; i++) {
        int e = v->order[i];
        if (show[e] && !skip[e])
            v->vis[nv++].idx = e;
        else
            show[e] = 0;
    }
    v->nvis = nv;
    for (i = 0; i < n; i++)
        rowof[i] = -1;
    for (i = 0; i < nv; i++)
        rowof[v->vis[i].idx] = i;
    /* nearest visible ancestor */
    for (i = 0; i < nv; i++) {
        int p = s->e[v->vis[i].idx].parent;
        while (p >= 0 && rowof[p] < 0)
            p = s->e[p].parent;
        v->vis[i].vparent = p >= 0 ? rowof[p] : -1;
    }
    {
        int roots = 0;
        for (i = 0; i < nv; i++)
            roots += v->vis[i].vparent < 0;
        v->multi = roots > 1;
    }
    /* indent, connectors, gutters */
    stk = (int *)malloc(sizeof(int) * (size_t)(nv + 1) * 8);
    if (!stk && nv)
        goto oom;
    {
        /* (row, indent, justBranched, showConnector, isLast, gutterCount, virtualRootChild) packed in 7 ints */
        int nroots = 0, rr;
        for (i = 0; i < nv; i++)
            nroots += v->vis[i].vparent < 0;
        {
            int seen = 0;
            for (rr = nv - 1; rr >= 0; rr--)
                if (v->vis[rr].vparent < 0) {
                    int isl = (seen == 0);
                    seen++;
                    stk[sp * 8 + 0] = rr;
                    stk[sp * 8 + 1] = v->multi ? 1 : 0;
                    stk[sp * 8 + 2] = v->multi;
                    stk[sp * 8 + 3] = v->multi;
                    stk[sp * 8 + 4] = isl;
                    stk[sp * 8 + 5] = 0;
                    stk[sp * 8 + 6] = v->multi;
                    sp++;
                }
            (void)nroots;
        }
        while (sp > 0) {
            int row, indent, jb, sc, isl, ng, virt, nch = 0, k, childindent, shown, dispindent, cpos;
            tgut gut[MAXG];
            tnode *nd;
            sp--;
            row = stk[sp * 8 + 0];
            indent = stk[sp * 8 + 1];
            jb = stk[sp * 8 + 2];
            sc = stk[sp * 8 + 3];
            isl = stk[sp * 8 + 4];
            ng = stk[sp * 8 + 5];
            virt = stk[sp * 8 + 6];
            nd = &v->vis[row];
            /* the gutters this node inherits were stored at its parent: recompute from the parent chain */
            nd->ng = 0;
            if (nd->vparent >= 0) {
                tnode *pn = &v->vis[nd->vparent];
                int q;
                for (q = 0; q < pn->ng; q++)
                    nd->g[nd->ng++] = pn->g[q];
                /* the parent's own connector gutter */
                if (pn->conn && !pn->virt) {
                    int pd = v->multi ? (pn->indent > 0 ? pn->indent - 1 : 0) : pn->indent;
                    int cp = pd > 0 ? pd - 1 : 0;
                    if (nd->ng < MAXG) {
                        nd->g[nd->ng].pos = cp;
                        nd->g[nd->ng].show = !pn->last;
                        nd->ng++;
                    }
                }
            }
            (void)ng;
            (void)gut;
            nd->indent = indent;
            nd->conn = sc;
            nd->last = isl;
            nd->virt = virt;
            /* children */
            {
                int cnt = 0;
                for (k = 0; k < nv; k++)
                    if (v->vis[k].vparent == row)
                        cnt++;
                nch = cnt;
            }
            if (nch > 1)
                childindent = indent + 1;
            else if (jb && indent > 0)
                childindent = indent + 1;
            else
                childindent = indent;
            shown = sc && !virt;
            dispindent = v->multi ? (indent > 0 ? indent - 1 : 0) : indent;
            cpos = dispindent > 0 ? dispindent - 1 : 0;
            (void)shown;
            (void)cpos;
            {
                int kids[512], kc = 0;
                for (k = 0; k < nv && kc < 512; k++)
                    if (v->vis[k].vparent == row)
                        kids[kc++] = k;
                for (k = kc - 1; k >= 0; k--) {
                    stk[sp * 8 + 0] = kids[k];
                    stk[sp * 8 + 1] = childindent;
                    stk[sp * 8 + 2] = nch > 1;
                    stk[sp * 8 + 3] = nch > 1;
                    stk[sp * 8 + 4] = k == kc - 1;
                    stk[sp * 8 + 5] = 0;
                    stk[sp * 8 + 6] = 0;
                    sp++;
                    if (sp >= nv)
                        break;
                }
            }
        }
    }
    free(stk);
    /* the cursor stays on the same entry, else its nearest visible ancestor, else the last row */
    near = -1;
    if (v->last_sel >= 0) {
        int j = v->last_sel;
        while (j >= 0 && (near = row_of(v, j)) < 0)
            j = s->e[j].parent;
    }
    if (near < 0)
        near = nv ? nv - 1 : 0;
    v->pk.sel = near;
    v->pk.n = nv;
    if (nv)
        v->last_sel = v->vis[near].idx;
    free(show);
    free(skip);
    free(rowof);
    return 0;
oom:
    free(show);
    free(skip);
    free(rowof);
    return -1;
}

/* ---- rows ---- */

static void row_text(void *u, int i, int selected, char *out, long cap)
{
    tview *v = (tview *)u;
    tnode *nd = &v->vis[i];
    pi_s3 *s = S(v);
    const cl_pitheme *th = v->t->pith;
    char b[48], prefix[260];
    jw w;
    int disp = v->multi ? (nd->indent > 0 ? nd->indent - 1 : 0) : nd->indent;
    int total = disp * 3, k, cpos = nd->conn && !nd->virt ? disp - 1 : -1, folded = v->folded[nd->idx];
    const char *lb;
    prefix[0] = 0;
    for (k = 0; k < total && k < 60; k++) {
        int level = k / 3, pos = k % 3, g, found = -1;
        for (g = 0; g < nd->ng; g++)
            if (nd->g[g].pos == level)
                found = g;
        if (found >= 0)
            cl_cat(prefix, pos == 0 && nd->g[found].show ? "\342\224\202" : " ", sizeof(prefix));
        else if (cpos >= 0 && level == cpos) {
            if (pos == 0)
                cl_cat(prefix, nd->last ? "\342\224\224" : "\342\224\234", sizeof(prefix));
            else if (pos == 1)
                cl_cat(prefix, folded ? "\342\212\236" : foldable(v, i) ? "\342\212\237" : "\342\224\200", sizeof(prefix));
            else
                cl_cat(prefix, " ", sizeof(prefix));
        } else
            cl_cat(prefix, " ", sizeof(prefix));
    }
    if (selected) {
        cl_copy(out, sgr(th, PR_ACCENT, b), cap);
        cl_cat(out, "\342\200\272 " SGR0, cap);
    } else
        cl_copy(out, "  ", cap);
    cl_cat(out, DIM, cap);
    cl_cat(out, prefix, cap);
    cl_cat(out, SGR0, cap);
    if (folded && !(nd->conn && !nd->virt)) {
        cl_cat(out, sgr(th, PR_ACCENT, b), cap);
        cl_cat(out, "\342\212\236 " SGR0, cap);
    }
    if (v->active[nd->idx]) {
        cl_cat(out, sgr(th, PR_ACCENT, b), cap);
        cl_cat(out, "\342\200\242 " SGR0, cap);
    }
    lb = s3_label_of(s, nd->idx);
    if (lb) {
        cl_cat(out, sgr(th, PR_WARNING, b), cap);
        cl_cat(out, "[", cap);
        cl_cat(out, lb, cap);
        cl_cat(out, "] " SGR0, cap);
    }
    jw_init(&w);
    pi_tree_entry_text(v->st, nd->idx, th, selected, &w);
    jw_rawz(&w, "");
    cl_cat(out, w.p, cap);
    jw_free(&w);
}

static void status_text(tview *v)
{
    static const char *const names[] = { "", " [no-tools]", " [user]", " [labeled]", " [all]" };
    cl_copy(v->pk.status, names[v->filter], sizeof(v->pk.status));
    if (v->stamps)
        cl_cat(v->pk.status, " [+label time]", sizeof(v->pk.status));
}

static void refold(tview *v)
{
    memset(v->folded, 0, (size_t)S(v)->n);
}

/* the next branch segment start above / below the selection (pi findBranchSegmentStart) */
static int segment_start(tview *v, int down)
{
    int cur = v->pk.sel, row = cur, guard = 0;
    if (down) {
        for (;;) {
            int k, first = -1, cnt = 0;
            for (k = 0; k < v->nvis; k++)
                if (v->vis[k].vparent == row) {
                    if (first < 0)
                        first = k;
                    cnt++;
                }
            if (cnt == 0)
                return row;
            if (cnt > 1)
                return first;
            row = first;
            if (++guard > v->nvis)
                return cur;
        }
    }
    for (;;) {
        int p = v->vis[row].vparent, k, cnt = 0;
        if (p < 0)
            return row;
        for (k = 0; k < v->nvis; k++)
            cnt += v->vis[k].vparent == p;
        if (cnt > 1 && row < cur)
            return row;
        row = p;
        if (++guard > v->nvis)
            return cur;
    }
}

static void changed(tview *v)
{
    refold(v);
    refilter(v);
    status_text(v);
}

/* the keys of the tree: filters, folding, labels, search; 1 when taken */
static int tree_key(void *u, const cl_key *k, int act)
{
    tview *v = (tview *)u;
    int sel = v->pk.sel, qn = (int)strlen(v->pk.query);
    switch (act) {
    case KA_TREE_F_DEFAULT:
        v->filter = TF_DEFAULT;
        changed(v);
        return 1;
    case KA_TREE_F_NOTOOLS:
    case KA_TREE_F_USER:
    case KA_TREE_F_LABELED:
    case KA_TREE_F_ALL: {
        int m = act == KA_TREE_F_NOTOOLS ? TF_NO_TOOLS : act == KA_TREE_F_USER ? TF_USER : act == KA_TREE_F_LABELED ? TF_LABELED : TF_ALL;
        v->filter = v->filter == m ? TF_DEFAULT : m;
        changed(v);
        return 1;
    }
    case KA_TREE_F_NEXT:
        v->filter = (v->filter + 1) % TF_COUNT;
        changed(v);
        return 1;
    case KA_TREE_F_PREV:
        v->filter = (v->filter + TF_COUNT - 1) % TF_COUNT;
        changed(v);
        return 1;
    case KA_TREE_FOLD:
        if (v->nvis && foldable(v, sel) && !v->folded[v->vis[sel].idx]) {
            v->folded[v->vis[sel].idx] = 1;
            v->last_sel = v->vis[sel].idx;
            refilter(v);
        } else if (v->nvis)
            v->pk.sel = segment_start(v, 0);
        return 1;
    case KA_TREE_UNFOLD:
        if (v->nvis && v->folded[v->vis[sel].idx]) {
            v->folded[v->vis[sel].idx] = 0;
            v->last_sel = v->vis[sel].idx;
            refilter(v);
        } else if (v->nvis)
            v->pk.sel = segment_start(v, 1);
        return 1;
    case KA_TREE_STAMP:
        v->stamps = !v->stamps;
        status_text(v);
        return 1;
    case KA_TREE_LABEL:
        if (v->nvis) {
            char text[100];
            int idx = v->vis[sel].idx;
            if (tui_line_input(v->t, "  Label (empty to remove):", text, sizeof(text)) == 1) {
                s3_label(S(v), idx, text[0] ? text : 0);
                v->last_sel = idx;
                refilter(v);
            }
        }
        return 1;
    case KA_SEL_CANCEL:
        if (qn) {
            v->pk.query[0] = 0;
            changed(v);
            return 1;
        }
        return 0;
    default:
        break;
    }
    if (act == KA_NONE && k->k == K_BS) {
        if (qn) {
            do
                qn--;
            while (qn > 0 && ((unsigned char)v->pk.query[qn] & 0xc0) == 0x80);
            v->pk.query[qn] = 0;
            changed(v);
        }
        return 1;
    }
    if (act == KA_NONE && k->k == K_CHAR && k->ch >= 0x20 && qn + 5 < (int)sizeof(v->pk.query)) {
        v->pk.query[qn + vw_put_utf8(v->pk.query + qn, k->ch)] = 0;
        changed(v);
        return 1;
    }
    if (v->nvis)
        v->last_sel = v->vis[v->pk.sel].idx;
    return 0;
}

static void help_text(tview *v, char *out, long cap)
{
    cl_keymap *m = &v->t->km;
    char a[40], b[40], c[40], d[40];
    out[0] = 0;
    cl_cat(out, "  \342\206\221/\342\206\223 move \302\267 \342\206\220/\342\206\222 page \302\267 ", cap);
    km_key_text(m, KC_TREE, KA_TREE_FOLD, a, sizeof(a));
    km_key_text(m, KC_TREE, KA_TREE_UNFOLD, b, sizeof(b));
    cl_cat(out, a, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, b, cap);
    cl_cat(out, " branch \302\267 ", cap);
    km_key_text(m, KC_TREE, KA_TREE_LABEL, a, sizeof(a));
    cl_cat(out, a, cap);
    cl_cat(out, " label \302\267 ", cap);
    km_key_text(m, KC_TREE, KA_TREE_STAMP, a, sizeof(a));
    cl_cat(out, a, cap);
    cl_cat(out, " label time \302\267 filters ", cap);
    km_key_text(m, KC_TREE, KA_TREE_F_DEFAULT, a, sizeof(a));
    km_key_text(m, KC_TREE, KA_TREE_F_NOTOOLS, b, sizeof(b));
    km_key_text(m, KC_TREE, KA_TREE_F_USER, c, sizeof(c));
    km_key_text(m, KC_TREE, KA_TREE_F_LABELED, d, sizeof(d));
    cl_cat(out, a, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, b + 5, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, c + 5, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, d + 5, cap);
    km_key_text(m, KC_TREE, KA_TREE_F_ALL, a, sizeof(a));
    cl_cat(out, "/", cap);
    cl_cat(out, a + 5, cap);
    cl_cat(out, " \302\267 cycle ", cap);
    km_key_text(m, KC_TREE, KA_TREE_F_NEXT, a, sizeof(a));
    km_key_text(m, KC_TREE, KA_TREE_F_PREV, b, sizeof(b));
    cl_cat(out, a, cap);
    cl_cat(out, "/", cap);
    cl_cat(out, b, cap);
}

int pi_tree_pick(pi_store *st, cl_tui *t, int select, int *filter)
{
    pi_s3 *s = &st->s3;
    tview v;
    char help[400];
    int res = -1, n = s->n;
    if (n <= 0)
        return -1;
    memset(&v, 0, sizeof(v));
    v.st = st;
    v.t = t;
    v.leaf = s->leaf;
    v.filter = filter ? *filter : TF_DEFAULT;
    v.order = (int *)malloc(sizeof(int) * (size_t)n);
    v.folded = (char *)calloc((size_t)n, 1);
    v.active = (char *)calloc((size_t)n, 1);
    v.vis = (tnode *)calloc((size_t)n, sizeof(tnode));
    if (!v.order || !v.folded || !v.active || !v.vis)
        goto out;
    build_active(&v);
    if (flatten(&v))
        goto out;
    v.last_sel = select >= 0 ? select : s->leaf;
    help_text(&v, help, sizeof(help));
    v.pk.title = "Session Tree";
    v.pk.help = help;
    v.pk.maxrows = t->rows / 2 < 5 ? 5 : t->rows / 2;
    v.pk.row = row_text;
    v.pk.key = tree_key;
    v.pk.u = &v;
    status_text(&v);
    if (refilter(&v))
        goto out;
    res = tui_pick_run(t, &v.pk);
    if (res >= 0 && res < v.nvis)
        res = v.vis[res].idx;
    else
        res = -1;
    if (filter)
        *filter = v.filter;
out:
    free(v.order);
    free(v.folded);
    free(v.active);
    free(v.vis);
    return res;
}
