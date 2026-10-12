/* cmds -- the session's slash commands (plan E5, E6; pi f10993bc agent-session.ts navigateTree, agent-session-runtime.ts
 * fork, tree-selector in the interactive mode):
 *   /tree                the session tree, numbered, the active branch marked
 *   /tree N              the leaf moves to entry N: a user message leaves the leaf at its parent and its text comes
 *                        back to the input; anything else becomes the leaf
 *   /tree N summarize [focus]   the same, with a summary of the branch left attached at the new leaf
 *   /tree N summarize-with PROMPT   the summary asked for with PROMPT instead of the default one
 *   /tree N ... -l LABEL   the summary (else the entry moved to) gets the label
 *   /fork                the user messages of the branch, numbered
 *   /fork N              a new session holding the branch up to just before user message N; its text comes back
 *   /clone               a new session holding the whole branch
 *   /label N [text]      a label on tree entry N (no text clears it)
 *   /name [text]         the session's display name
 *   /session             where the session is
 * pi's selector is a full-screen list; this is the same operations as commands (COMPATIBILITY.md). */
#include <stdlib.h>
#include <string.h>
#include "store.h"
#include "resources.h"
#include "compact.h"
#include "export.h"
#include "models.h"
#include "claude/util.h"

#define ROW_TEXT 60
#define TREE_SHOWN 40

typedef struct row {
    int idx;                    /* tree index of the entry */
    int depth;
    int parent;                 /* the displayed parent's row, -1 */
} row;

/* the text of a message entry's content: a string, or the text blocks joined */
static void content_line(jv c, jw *out)
{
    long n;
    if (json_type(c) == J_STR) {
        char *z = json_strdup(c, &n);
        if (z)
            jw_raw(out, z, n);
        free(z);
    } else if (json_type(c) == J_ARR) {
        jit it;
        jv b, t, x;
        json_iter(c, &it);
        while (json_next(&it, 0, &b))
            if (json_type(b) == J_OBJ && json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                char *z = json_type(x) == J_STR ? json_strdup(x, &n) : 0;
                if (z)
                    jw_raw(out, z, n);
                free(z);
            }
    }
}

/* whether the tree list shows entry i: the conversation, not the bookkeeping (tool results, model and
 * thinking changes, labels, names, usage) */
static int shown(pi_s3 *st, int i)
{
    s3_ent *e = &st->e[i];
    if (e->type == S3_MESSAGE)
        return e->role == S3R_USER || e->role == S3R_ASSISTANT || e->role == S3R_CUSTOM || e->role == S3R_BASH;
    return e->type == S3_COMPACTION || e->type == S3_BRANCH_SUMMARY || e->type == S3_CUSTOM_MESSAGE;
}

/* an entry in one line, at most ROW_TEXT characters */
static void entry_text(pi_s3 *st, int i, jw *out)
{
    long n;
    const char *t = s3_text(st, i, &n);
    jv o, m, v;
    jw w;
    int type = st->e[i].type;
    jw_init(&w);
    if (!t || json_parse(t, n, &o)) {
        jw_rawz(out, "(unreadable)");
        jw_free(&w);
        return;
    }
    if (type == S3_MESSAGE && json_get(o, "message", &m)) {
        int role = st->e[i].role;
        if (role == S3R_USER) {
            jw_rawz(out, "user: ");
            if (json_get(m, "content", &v))
                content_line(v, &w);
        } else if (role == S3R_ASSISTANT) {
            jw_rawz(out, "assistant: ");
            if (json_get(m, "content", &v))
                content_line(v, &w);
            if (!w.n && json_get(m, "content", &v) && json_type(v) == J_ARR) {
                jit it;
                jv b, nm, ty;
                int any = 0;
                json_iter(v, &it);
                while (json_next(&it, 0, &b))
                    if (json_get(b, "type", &ty) && json_streq(ty, "toolCall") && json_get(b, "name", &nm)) {
                        char *z = json_strdup(nm, 0);
                        jw_rawz(&w, any++ ? ", " : "[calls ");
                        if (z)
                            jw_rawz(&w, z);
                        free(z);
                    }
                if (any)
                    jw_rawz(&w, "]");
            }
        } else if (role == S3R_BASH) {
            jw_rawz(out, "bash: ");
            if (json_get(m, "command", &v))
                content_line(v, &w);
        } else {
            jw_rawz(out, "custom: ");
            if (json_get(m, "content", &v))
                content_line(v, &w);
        }
    } else if (type == S3_COMPACTION) {
        jw_rawz(out, "compaction");
        if (json_get(o, "tokensBefore", &v)) {
            jw_rawz(&w, "(");
            jw_raw(&w, v.p, v.n);
            jw_rawz(&w, " tokens summarized)");
        }
    } else if (type == S3_BRANCH_SUMMARY) {
        jw_rawz(out, "branch summary");
    } else if (type == S3_CUSTOM_MESSAGE) {
        jw_rawz(out, "custom: ");
        if (json_get(o, "content", &v))
            content_line(v, &w);
    }
    if (w.n) {
        long k, take = 0, cols = 0;
        for (k = 0; k < w.n && cols < ROW_TEXT; k++) {
            if (w.p[k] == '\n' || w.p[k] == '\r')
                break;
            if (((unsigned char)w.p[k] & 0xc0) != 0x80)
                cols++;
            take = k + 1;
        }
        if (cols >= ROW_TEXT) {
            /* the cut must not fall inside a UTF-8 character */
            while (take < w.n && ((unsigned char)w.p[take] & 0xc0) == 0x80)
                take++;
        }
        jw_raw(out, w.p, take);
        if (take < w.n && w.p[take] != '\n')
            jw_rawz(out, "...");
    }
    jw_free(&w);
}

/* the displayed rows in depth-first order, children in file order; a level of indentation only where the
 * tree forks. NULL out of memory */
static row *build_rows(pi_s3 *st, int *nrows)
{
    row *rows;
    int *disp_parent, *kids, i, n = 0, *stack, sp, *order;
    int *rowof;
    *nrows = 0;
    rows = (row *)malloc((size_t)(st->n + 1) * sizeof(row));
    disp_parent = (int *)malloc((size_t)(st->n + 1) * sizeof(int));
    kids = (int *)calloc((size_t)(st->n + 1), sizeof(int));
    stack = (int *)malloc((size_t)(st->n + 1) * sizeof(int));
    order = (int *)malloc((size_t)(st->n + 1) * sizeof(int));
    rowof = (int *)malloc((size_t)(st->n + 1) * sizeof(int));
    if (!rows || !disp_parent || !kids || !stack || !order || !rowof) {
        free(rows);
        free(disp_parent);
        free(kids);
        free(stack);
        free(order);
        free(rowof);
        return 0;
    }
    /* the nearest displayed ancestor of every displayed entry */
    for (i = 0; i < st->n; i++) {
        int p = st->e[i].parent, hops = 0;
        disp_parent[i] = -1;
        while (p >= 0 && !shown(st, p) && hops++ <= st->n)
            p = st->e[p].parent;
        disp_parent[i] = p;
        if (shown(st, i) && p >= 0)
            kids[p]++;
    }
    /* depth-first: roots (no displayed parent) in file order; a stack holding the siblings still to visit */
    sp = 0;
    for (i = st->n - 1; i >= 0; i--)
        if (shown(st, i) && disp_parent[i] < 0)
            stack[sp++] = i;
    while (sp > 0) {
        int cur = stack[--sp], c;
        order[n] = cur;
        rowof[cur] = n;
        rows[n].idx = cur;
        rows[n].parent = disp_parent[cur] >= 0 ? rowof[disp_parent[cur]] : -1;
        rows[n].depth = rows[n].parent < 0 ? 0 : rows[rows[n].parent].depth + (kids[disp_parent[cur]] > 1 ? 1 : 0);
        n++;
        for (c = st->n - 1; c >= 0; c--)
            if (shown(st, c) && disp_parent[c] == cur)
                stack[sp++] = c;
    }
    free(disp_parent);
    free(kids);
    free(stack);
    free(order);
    free(rowof);
    *nrows = n;
    return rows;
}

static int row_of_entry(const row *rows, int n, int idx)
{
    int i;
    for (i = 0; i < n; i++)
        if (rows[i].idx == idx)
            return i;
    return -1;
}

static void put_num(jw *w, long v, int width)
{
    char b[16];
    int k = 0, i;
    long t = v;
    do {
        b[k++] = (char)('0' + t % 10);
        t /= 10;
    } while (t);
    for (i = k; i < width; i++)
        jw_raw(w, " ", 1);
    while (k)
        jw_raw(w, &b[--k], 1);
}

static void list_tree(pi_store *st, int all, jw *reply)
{
    row *rows;
    int n, i, from, *path = 0, np, cur = -1;
    pi_s3 *s = &st->s3;
    rows = build_rows(s, &n);
    if (!rows) {
        jw_rawz(reply, "[ERROR] out of memory");
        return;
    }
    if (!n) {
        jw_rawz(reply, "(the session is empty)");
        free(rows);
        return;
    }
    np = s3_path(s, s->leaf, &path);
    /* the row that stands for the leaf: the leaf itself, else its nearest displayed ancestor */
    {
        int j = s->leaf, hops = 0;
        while (j >= 0 && row_of_entry(rows, n, j) < 0 && hops++ <= s->n)
            j = s->e[j].parent;
        cur = j >= 0 ? row_of_entry(rows, n, j) : -1;
    }
    from = !all && n > TREE_SHOWN ? n - TREE_SHOWN : 0;
    if (from) {
        jw_rawz(reply, "(the last ");
        jw_long(reply, TREE_SHOWN);
        jw_rawz(reply, " of ");
        jw_long(reply, n);
        jw_rawz(reply, " entries; /tree all shows every one)\n");
    }
    for (i = from; i < n; i++) {
        int d, k, on = 0;
        const char *lb;
        for (k = 0; k < np; k++)
            on |= path[k] == rows[i].idx;
        put_num(reply, i + 1, 4);
        jw_rawz(reply, on ? " * " : "   ");
        for (d = 0; d < rows[i].depth && d < 12; d++)
            jw_rawz(reply, "  ");
        entry_text(s, rows[i].idx, reply);
        lb = s3_label_of(s, rows[i].idx);
        if (lb) {
            jw_rawz(reply, " [");
            jw_rawz(reply, lb);
            jw_rawz(reply, "]");
        }
        if (i == cur)
            jw_rawz(reply, "   <- you are here");
        if (i < n - 1)
            jw_rawz(reply, "\n");
    }
    free(path);
    free(rows);
}

/* the entry a row number names (1-based): its tree index, -1 */
static int entry_of(pi_store *st, const char *arg, row **rows_out, int *n_out)
{
    int num = 0, n;
    row *rows;
    const char *p = arg;
    if (*p < '0' || *p > '9')
        return -1;
    while (*p >= '0' && *p <= '9' && num < 100000)
        num = num * 10 + (*p++ - '0');
    rows = build_rows(&st->s3, &n);
    if (!rows)
        return -1;
    if (num < 1 || num > n) {
        free(rows);
        return -1;
    }
    num = rows[num - 1].idx;
    if (rows_out)
        *rows_out = rows;
    else
        free(rows);
    if (n_out)
        *n_out = n;
    return num;
}

/* the text of a user or custom message entry, for the input */
static void message_text(pi_s3 *s, int i, jw *out)
{
    long n;
    const char *t = s3_text(s, i, &n);
    jv o, m, v;
    if (!t || json_parse(t, n, &o))
        return;
    if (s->e[i].type == S3_MESSAGE && json_get(o, "message", &m) && json_get(m, "content", &v))
        content_line(v, out);
    else if (s->e[i].type == S3_CUSTOM_MESSAGE && json_get(o, "content", &v))
        content_line(v, out);
}

void pi_user_text(pi_s3 *s, int i, jw *out)
{
    message_text(s, i, out);
}

/* navigateTree */
static void navigate(pi_store *st, int target, int summarize, const char *instr, int replace, const char *label,
                     jw *reply, jw *editor)
{
    pi_s3 *s = &st->s3;
    int new_leaf, role_user, *ent = 0, anc, n = 0;
    jw sum, details, usage;
    int have_sum = 0, has_usage = 0;
    if (target == s->leaf) {
        jw_rawz(reply, "[INFO] Already there.");
        return;
    }
    jw_init(&sum);
    jw_init(&details);
    jw_init(&usage);
    if (summarize) {
        char err[200];
        cp_branch b;
        n = cp_collect(s, s->leaf, target, &ent, &anc);
        if (n < 0) {
            jw_rawz(reply, "[ERROR] out of memory");
            goto done;
        }
        if (n > 0) {
            int rc;
            rc = cp_branch_summary(s, ent, n, st->s->model.context_window, CP_RESERVE, st->s->model.max_tokens, instr, replace,
                                   pi_store_complete, st, &b, err, sizeof(err));
            if (rc == 1) {
                jw_rawz(reply, "[INFO] Stopped.");
                goto done;
            }
            if (rc) {
                jw_rawz(reply, "[ERROR] ");
                jw_rawz(reply, err);
                goto done;
            }
            jw_raw(&sum, b.summary.p, b.summary.n);
            jw_raw(&details, b.details.p, b.details.n);
            if (b.has_usage) {
                llm_w_usage(&usage, &b.usage);
                has_usage = 1;
            }
            have_sum = 1;
            cp_branch_free(&b);
        }
    }
    role_user = s->e[target].type == S3_CUSTOM_MESSAGE || (s->e[target].type == S3_MESSAGE && s->e[target].role == S3R_USER);
    if (role_user) {
        new_leaf = s->e[target].parent;
        message_text(s, target, editor);
    } else
        new_leaf = target;
    if (have_sum) {
        int sidx = s3_branch_summary(s, new_leaf, sum.p, details.p, has_usage ? usage.p : 0, 0);
        if (sidx < 0) {
            jw_rawz(reply, "[ERROR] ");
            jw_rawz(reply, s->err[0] ? s->err : "the summary could not be recorded");
            goto done;
        }
        if (label && *label)
            s3_label(s, sidx, label);       /* on the summary, which stays the leaf's parent chain */
    } else {
        s3_set_leaf(s, new_leaf);
        if (label && *label)
            s3_label(s, target, label);
    }
    if (pi_store_reload(st)) {
        jw_rawz(reply, "[ERROR] the session could not be reloaded");
        goto done;
    }
    jw_rawz(reply, have_sum ? "[INFO] Moved, with a summary of the branch left." : "[INFO] Moved.");
done:
    free(ent);
    jw_free(&sum);
    jw_free(&details);
    jw_free(&usage);
}

void pi_tree_navigate(pi_store *st, int idx, int summarize, const char *instr, int replace, jw *reply, jw *editor)
{
    navigate(st, idx, summarize, instr, replace, 0, reply, editor);
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

/* "word" at p followed by a space or the end: the rest after it, else 0 */
static const char *word(const char *p, const char *w)
{
    long n = (long)strlen(w);
    if (strncmp(p, w, (size_t)n) || (p[n] && p[n] != ' '))
        return 0;
    return skip_ws(p + n);
}

static void fork_list(pi_store *st, jw *reply)
{
    pi_s3 *s = &st->s3;
    int *path = 0, np, i, k = 0;
    np = s3_path(s, s->leaf, &path);
    for (i = 0; i < np; i++)
        if (s->e[path[i]].type == S3_MESSAGE && s->e[path[i]].role == S3R_USER) {
            put_num(reply, ++k, 4);
            jw_rawz(reply, "   ");
            entry_text(s, path[i], reply);
            jw_rawz(reply, "\n");
        }
    if (!k)
        jw_rawz(reply, "(no user messages to fork from)");
    free(path);
}

/* a new session from the branch ending at leaf; -1 a fork at the root: a new empty session with parentSession.
 * 0, or -1 with the reason in reply ("[ERROR] ...") */
int pi_store_branch(pi_store *st, int leaf, jw *reply)
{
    pi_s3 *s = &st->s3;
    if (s->persist && !s->flushed) {
        jw_rawz(reply, "[ERROR] This session has not been saved yet. Send a message before cloning or forking it.");
        return -1;
    }
    if (leaf < 0) {
        char old[320], dir[256], cwd[256];
        cl_copy(old, s->persist ? s->file : "", sizeof(old));
        cl_copy(dir, s->dir, sizeof(dir));
        cl_copy(cwd, s->cwd, sizeof(cwd));
        if (s3_new(s, dir, cwd, old, 0, s->persist)) {
            jw_rawz(reply, "[ERROR] the new session could not be made");
            return -1;
        }
    } else if (s3_branched(s, leaf)) {
        jw_rawz(reply, "[ERROR] ");
        jw_rawz(reply, s->err[0] ? s->err : "Failed to create forked session");
        return -1;
    }
    if (pi_store_reload(st)) {
        jw_rawz(reply, "[ERROR] the session could not be reloaded");
        return -1;
    }
    return 0;
}

int pi_cmd(pi_store *st, const char *line, jw *reply, jw *editor)
{
    pi_s3 *s = &st->s3;
    const char *a;
    int idx;
    if ((a = word(line, "/tree")) != 0) {
        if (!*a || !strcmp(a, "all")) {
            list_tree(st, !strcmp(a, "all"), reply);
            return 1;
        }
        idx = entry_of(st, a, 0, 0);
        if (idx < 0) {
            jw_rawz(reply, "[ERROR] No such entry: /tree lists them, /tree N moves to entry N");
            return 1;
        }
        while (*a >= '0' && *a <= '9')
            a++;
        a = skip_ws(a);
        {
            /* [summarize [focus] | summarize-with prompt] [-l label] */
            char buf[512], *lab = 0, *cut;
            const char *focus = 0;
            int sum = 0, replace = 0;
            cl_copy(buf, a, sizeof(buf));
            cut = strstr(buf, " -l ");
            if (cut) {
                *cut = 0;
                lab = cut + 4;
            } else if (!strncmp(buf, "-l ", 3)) {
                lab = buf + 3;
                buf[0] = 0;
            }
            if (lab)
                lab = (char *)skip_ws(lab);
            if (buf[0]) {
                const char *w;
                if ((w = word(buf, "summarize")) != 0) {
                    sum = 1;
                    focus = *w ? w : 0;
                } else if ((w = word(buf, "summarize-with")) != 0) {
                    sum = replace = 1;
                    focus = *w ? w : 0;
                } else {
                    jw_rawz(reply, "[ERROR] After the number: summarize [focus], summarize-with PROMPT, -l LABEL");
                    return 1;
                }
            }
            navigate(st, idx, sum, focus, replace, lab, reply, editor);
        }
        return 1;
    }
    if ((a = word(line, "/export")) != 0) {
        char done[700], err[200];
        cl_pitheme th;
        pitheme_system(&th);
        if (pi_export(st, &th, st->s3.cwd, *a ? a : 0, done, sizeof(done), err, sizeof(err))) {
            jw_rawz(reply, "[ERROR] Failed to export session: ");
            jw_rawz(reply, err);
        } else
            jw_rawz(reply, done);
        return 1;
    }
    if ((a = word(line, "/fork")) != 0) {
        if (!*a) {
            fork_list(st, reply);
            return 1;
        }
        {
            int num = 0, *path = 0, np, i, k = 0, pick = -1;
            const char *p = a;
            while (*p >= '0' && *p <= '9' && num < 100000)
                num = num * 10 + (*p++ - '0');
            np = s3_path(s, s->leaf, &path);
            for (i = 0; i < np; i++)
                if (s->e[path[i]].type == S3_MESSAGE && s->e[path[i]].role == S3R_USER && ++k == num)
                    pick = path[i];
            free(path);
            if (pick < 0 || p == a) {
                jw_rawz(reply, "[ERROR] Invalid entry for forking: /fork lists the user messages");
                return 1;
            }
            message_text(s, pick, editor);
            if (pi_store_branch(st, s->e[pick].parent, reply))
                return 1;
            jw_rawz(reply, "[INFO] Forked into a new session.");
        }
        return 1;
    }
    if ((a = word(line, "/clone")) != 0) {
        if (s->leaf < 0) {
            jw_rawz(reply, "[ERROR] Nothing to clone yet");
            return 1;
        }
        if (pi_store_branch(st, s->leaf, reply))
            return 1;
        jw_rawz(reply, "[INFO] Cloned into a new session.");
        return 1;
    }
    if ((a = word(line, "/label")) != 0) {
        idx = entry_of(st, a, 0, 0);
        if (idx < 0) {
            jw_rawz(reply, "[ERROR] /label N [text]: N is an entry number of /tree");
            return 1;
        }
        while (*a >= '0' && *a <= '9')
            a++;
        a = skip_ws(a);
        if (s3_label(s, idx, a) < 0)
            jw_rawz(reply, "[ERROR] the label could not be recorded");
        else
            jw_rawz(reply, *a ? "[INFO] Label set." : "[INFO] Label cleared.");
        return 1;
    }
    if ((a = word(line, "/name")) != 0) {
        char nm[128];
        if (!*a) {
            if (s3_name(s, nm, sizeof(nm)))
                jw_rawz(reply, nm);
            else
                jw_rawz(reply, "(no name; /name <text> sets one)");
        } else if (s3_session_info(s, a) < 0)
            jw_rawz(reply, "[ERROR] the name could not be recorded");
        else
            jw_rawz(reply, "[INFO] Named.");
        return 1;
    }
    if ((a = word(line, "/model")) != 0) {
        pi_session *ag = st->s;
        pi_models *ms = st->models;
        char err[200], label[200];
        const llm_model *to = 0;
        llm_model found;
        int lv = -1;
        if (!ms) {
            jw_rawz(reply, "[ERROR] models cannot be switched here");
            return 1;
        }
        if (!*a) {
            pi_models_list(ms, ag, reply);
            jw_rawz(reply, "Switch with /model <number> or /model <provider/model>.");
            return 1;
        }
        if (*a >= '0' && *a <= '9' && !strchr(a, '/') && !strchr(a, ':')) {
            to = pi_models_nth(ms, ag, (int)strtol(a, 0, 10));
            if (!to) {
                jw_rawz(reply, "[ERROR] no such number in /model's list");
                return 1;
            }
        } else if (llm_reg_resolve(&ms->reg, 0, a, &found, &lv, err, sizeof(err))) {
            jw_rawz(reply, "[ERROR] ");
            jw_rawz(reply, err);
            return 1;
        } else
            to = &found;
        if (pi_model_switch(ms, ag, to, err, sizeof(err))) {
            jw_rawz(reply, "[ERROR] ");
            jw_rawz(reply, err);
            return 1;
        }
        if (lv >= 0 && pi_thinking_set(ms, ag, lv) < 0) {
            jw_rawz(reply, "[ERROR] the thinking level change could not be recorded");
            return 1;
        }
        pi_model_label(&ag->model, label, sizeof(label));
        jw_rawz(reply, "[INFO] Model: ");
        jw_rawz(reply, label);
        jw_rawz(reply, ", thinking ");
        jw_rawz(reply, llm_think_name(ag->thinking));
        return 1;
    }
    if ((a = word(line, "/thinking")) != 0) {
        pi_session *ag = st->s;
        unsigned av = llm_think_levels(&ag->model);
        int i, lv;
        if (!st->models) {
            jw_rawz(reply, "[ERROR] the thinking level cannot be changed here");
            return 1;
        }
        if (!*a) {
            jw_rawz(reply, "Thinking level: ");
            jw_rawz(reply, llm_think_name(ag->thinking));
            jw_rawz(reply, "\nSupported: ");
            for (i = 0; i < LT_N; i++)
                if (av & (1u << i)) {
                    jw_rawz(reply, llm_think_name(i));
                    jw_rawz(reply, " ");
                }
            return 1;
        }
        lv = llm_think_of(a);
        if (lv < 0) {
            jw_rawz(reply, "[ERROR] Invalid thinking level \"");
            jw_rawz(reply, a);
            jw_rawz(reply, "\". Valid values: off, minimal, low, medium, high, xhigh, max");
            return 1;
        }
        lv = pi_thinking_set(st->models, ag, lv);
        if (lv < 0) {
            jw_rawz(reply, "[ERROR] the thinking level change could not be recorded");
            return 1;
        }
        jw_rawz(reply, "[INFO] Thinking level: ");
        jw_rawz(reply, llm_think_name(lv));
        return 1;
    }
    if ((a = word(line, "/trust")) != 0) {
        /* pi: "Save project trust decision for future sessions"; /trust lists the answers, /trust N keeps one */
        int pick = 0;
        if (!st->agent_dir[0]) {
            jw_rawz(reply, "[ERROR] no directory to keep trust.json in");
            return 1;
        }
        while (*a >= '0' && *a <= '9' && pick < 100)
            pick = pick * 10 + (*a++ - '0');
        pi_trust_command(st->sys, st->agent_dir, st->cwd, pick, reply);
        return 1;
    }
    if (word(line, "/session")) {
        jw_rawz(reply, s->persist ? s->file : "(not saved: --no-session)");
        jw_rawz(reply, "\nid ");
        jw_rawz(reply, s->id);
        jw_rawz(reply, "\nentries ");
        jw_long(reply, s->n);
        {
            pi_session *ag = st->s;
            char label[200];
            pi_model_label(&ag->model, label, sizeof(label));
            jw_rawz(reply, "\nmodel ");
            jw_rawz(reply, label);
            jw_rawz(reply, "\nthinking ");
            jw_rawz(reply, llm_think_name(ag->thinking));
            if (ag->model.context_window > 0 && ag->ctx_tokens > 0) {
                jw_rawz(reply, "\ncontext ");
                jw_long(reply, ag->ctx_tokens * 100 / ag->model.context_window);
                jw_rawz(reply, "% of ");
                jw_long(reply, ag->model.context_window);
            }
            jw_rawz(reply, "\ncost $");
            llm_w_micro(reply, ag->cost_micro);
        }
        return 1;
    }
    return 0;
}
