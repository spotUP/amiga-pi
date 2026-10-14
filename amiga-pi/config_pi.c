/* see config_pi.h */
#include <stdlib.h>
#include <string.h>
#include "config_pi.h"
#include "pkgres.h"
#include "authstore.h"
#include "claude/tui.h"
#include "claude/json.h"
#include "claude/util.h"
#include "view/vw_text.h"

#define CF_MAX_GROUPS 40

typedef struct cgroup {
    char label[300];
    char src[300];              /* PMK_PACKAGE: the package's source string */
    char base[300];             /* PathMetadata.baseDir */
    int pkg, scope, kind;
} cgroup;

typedef struct citem {
    char name[200];
    char path[300];
    int type, enabled, group;
} citem;

typedef struct centry {
    int kind;                   /* 0 group, 1 subgroup, 2 item */
    int group, type, item;
} centry;

typedef struct cfg {
    const pi_config_env *e;
    cgroup g[CF_MAX_GROUPS];
    int ord[CF_MAX_GROUPS];     /* the groups in the order pi lists them */
    int ng;
    citem *it;
    int nit, cap;
    centry *all, *vis;          /* the flat list, and what the search leaves of it */
    int nall;
    int maxv;                   /* ResourceList.maxVisible */
    char head[900];
    char dir[300];              /* the agent directory */
    int cols;
    tui_pick pk;
} cfg;

static const char *const type_label[2] = { "Skills", "Prompts" };
static const char *const type_key[2] = { "skills", "prompts" };

/* localeCompare enough for names: the letters' case does not count first, a lower case letter comes before its capital on a tie */
static int lcmp(const char *a, const char *b)
{
    const char *x = a, *y = b;
    for (; *x && *y; x++, y++) {
        int cx = *x >= 'A' && *x <= 'Z' ? *x + 32 : *x, cy = *y >= 'A' && *y <= 'Z' ? *y + 32 : *y;
        if (cx != cy)
            return cx - cy;
    }
    if (*x || *y)
        return *x ? 1 : -1;
    return -strcmp(a, b);
}

/* formatBaseDir: the home directory is "~", a trailing separator ends it */
static void format_base(const cfg *c, const char *base, char *out, long cap)
{
    const char *home = c->e->home ? c->e->home : "";
    long hl = (long)strlen(home);
    if (hl > 1 && !strncmp(base, home, (size_t)hl) && (base[hl] == '/' || !base[hl])) {
        cl_copy(out, "~", cap);
        cl_cat(out, base + hl, cap);
    } else
        cl_copy(out, base, cap);
    hl = (long)strlen(out);
    if (hl && out[hl - 1] != '/' && out[hl - 1] != ':')
        cl_cat(out, "/", cap);
}

/* path.relative for a path below base; the path itself when it is not */
static void rel_path(const char *base, const char *path, char *out, long cap)
{
    long bl = (long)strlen(base);
    if (bl && !strncmp(path, base, (size_t)bl)) {
        const char *r = path + bl;
        if (*r == '/')
            r++;
        else if (base[bl - 1] != '/' && base[bl - 1] != ':') {
            cl_copy(out, path, cap);
            return;
        }
        cl_copy(out, r, cap);
        return;
    }
    cl_copy(out, path, cap);
}

static int add_group(cfg *c, const pm_res *r)
{
    int i;
    for (i = 0; i < c->ng; i++)
        if (c->g[i].kind == r->kind && c->g[i].scope == r->scope && !strcmp(c->g[i].src, r->src) && !strcmp(c->g[i].base, r->base))
            return i;
    if (c->ng >= CF_MAX_GROUPS)
        return -1;
    i = c->ng++;
    memset(&c->g[i], 0, sizeof(c->g[i]));
    c->g[i].kind = r->kind;
    c->g[i].scope = r->scope;
    c->g[i].pkg = r->kind == PMK_PACKAGE;
    cl_copy(c->g[i].src, r->src, sizeof(c->g[i].src));
    cl_copy(c->g[i].base, r->base, sizeof(c->g[i].base));
    if (r->kind == PMK_PACKAGE) {
        cl_copy(c->g[i].label, r->src, sizeof(c->g[i].label));
        cl_cat(c->g[i].label, r->scope == PMS_USER ? " (user)" : " (project)", sizeof(c->g[i].label));
    } else if (r->kind == PMK_AUTO) {
        char b[300];
        cl_copy(c->g[i].label, r->scope == PMS_USER ? "User (" : "Project (", sizeof(c->g[i].label));
        format_base(c, r->base[0] ? r->base : c->dir, b, sizeof(b));
        cl_cat(c->g[i].label, b, sizeof(c->g[i].label));
        cl_cat(c->g[i].label, ")", sizeof(c->g[i].label));
    } else
        cl_copy(c->g[i].label, r->scope == PMS_USER ? "User settings" : "Project settings", sizeof(c->g[i].label));
    return i;
}

/* the name pi shows: a skill's folder (its file is SKILL.md), else the file name */
static void display_name(const char *path, int type, char *out, long cap)
{
    const char *slash = strrchr(path, '/'), *file = slash ? slash + 1 : path;
    if (type == PM_SKILLS && !strcmp(file, "SKILL.md") && slash) {
        const char *p = slash;
        long n;
        while (p > path && p[-1] != '/' && p[-1] != ':')
            p--;
        n = (long)(slash - p);
        if (n >= cap)
            n = cap - 1;
        memcpy(out, p, (size_t)n);
        out[n] = 0;
    } else
        cl_copy(out, file, cap);
}

/* the sort key of a group: pi sorts by origin (packages first), scope (user first) and source */
static const char *group_source(const cgroup *g)
{
    return g->kind == PMK_PACKAGE ? g->src : g->kind == PMK_AUTO ? "auto" : "local";
}

static int group_after(const cgroup *a, const cgroup *b)
{
    if (a->pkg != b->pkg)
        return !a->pkg && b->pkg;
    if (a->scope != b->scope)
        return a->scope > b->scope;
    return lcmp(group_source(a), group_source(b)) > 0;
}

static int load(cfg *c)
{
    pm_opts po;
    int type, i, j;
    memset(&po, 0, sizeof(po));
    po.agent_dir = c->e->agent_dir;
    po.data_dir = c->e->data_dir;
    po.cwd = c->e->cwd;
    po.trusted = 0;                         /* the global settings alone: pi's globalSettingsManager */
    po.home = c->e->home && c->e->home[0] ? c->e->home : 0;
    po.host = 1;
    cl_copy(c->dir, c->e->agent_dir ? c->e->agent_dir : "", sizeof(c->dir));
    for (type = 0; type < 2; type++) {
        pm_list l;
        memset(&l, 0, sizeof(l));
        if (pm_resolve(c->e->sys, &po, type, &l, 0)) {
            pm_free(&l);
            return -1;
        }
        for (i = 0; i < l.n; i++) {
            const pm_res *r = &l.e[i];
            int g = add_group(c, r);
            citem *it;
            if (g < 0)
                continue;
            if (c->nit == c->cap) {
                int nc = c->cap ? c->cap * 2 : 32;
                citem *n = (citem *)realloc(c->it, sizeof(citem) * (size_t)nc);
                if (!n) {
                    pm_free(&l);
                    return -1;
                }
                c->it = n;
                c->cap = nc;
            }
            it = &c->it[c->nit++];
            memset(it, 0, sizeof(*it));
            cl_copy(it->path, r->path, sizeof(it->path));
            display_name(r->path, type, it->name, sizeof(it->name));
            it->type = type;
            it->enabled = r->enabled;
            it->group = g;
        }
        pm_free(&l);
    }
    for (i = 0; i < c->ng; i++)
        c->ord[i] = i;
    for (i = 1; i < c->ng; i++) {                       /* a stable insertion sort */
        int x = c->ord[i];
        for (j = i; j > 0 && group_after(&c->g[c->ord[j - 1]], &c->g[x]); j--)
            c->ord[j] = c->ord[j - 1];
        c->ord[j] = x;
    }
    return 0;
}

/* the flat list: each group, its subgroups (Skills, Prompts) and the items by name */
static int flatten(cfg *c)
{
    int gi, type, i, n = 0;
    free(c->all);
    free(c->vis);
    c->all = (centry *)malloc(sizeof(centry) * (size_t)(c->nit + c->ng * 3 + 1));
    c->vis = (centry *)malloc(sizeof(centry) * (size_t)(c->nit + c->ng * 3 + 1));
    if (!c->all || !c->vis)
        return -1;
    for (gi = 0; gi < c->ng; gi++) {
        int g = c->ord[gi];
        c->all[n].kind = 0;
        c->all[n].group = g;
        n++;
        for (type = 0; type < 2; type++) {
            int k, j;
            int idx[1024], cnt = 0;
            for (i = 0; i < c->nit && cnt < 1024; i++)
                if (c->it[i].group == g && c->it[i].type == type)
                    idx[cnt++] = i;
            if (!cnt)
                continue;
            for (k = 1; k < cnt; k++) {                 /* by displayName, stable */
                int x = idx[k];
                for (j = k; j > 0 && lcmp(c->it[idx[j - 1]].name, c->it[x].name) > 0; j--)
                    idx[j] = idx[j - 1];
                idx[j] = x;
            }
            c->all[n].kind = 1;
            c->all[n].group = g;
            c->all[n].type = type;
            n++;
            for (k = 0; k < cnt; k++) {
                c->all[n].kind = 2;
                c->all[n].group = g;
                c->all[n].type = type;
                c->all[n].item = idx[k];
                n++;
            }
        }
    }
    c->nall = n;
    return 0;
}

static void lower(const char *s, char *o, long cap)
{
    long i;
    for (i = 0; s[i] && i < cap - 1; i++)
        o[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    o[i] = 0;
}

static int has_text(const char *hay, const char *needle)
{
    char a[400], b[100];
    lower(hay, a, sizeof(a));
    lower(needle, b, sizeof(b));
    return strstr(a, b) != 0;
}

/* filterItems: an empty query keeps everything; else the items whose name, type or path hold the text, with their headers */
static void filter(cfg *c)
{
    const char *q = c->pk.query;
    int i, n = 0, first_item = -1;
    char t[100];
    long l = (long)strlen(q), a = 0;
    while (a < l && q[a] == ' ')
        a++;
    if (a == l) {
        memcpy(c->vis, c->all, sizeof(centry) * (size_t)c->nall);
        n = c->nall;
    } else {
        int *keep = (int *)calloc((size_t)c->nall + 1, sizeof(int));
        if (!keep)
            return;
        cl_copy(t, q, sizeof(t));
        for (i = 0; i < c->nall; i++) {
            const centry *e = &c->all[i];
            if (e->kind == 2) {
                const citem *it = &c->it[e->item];
                if (has_text(it->name, t) || has_text(type_key[it->type], t) || has_text(it->path, t))
                    keep[i] = 1;
            }
        }
        for (i = 0; i < c->nall; i++) {
            int j;
            if (!keep[i])
                continue;
            for (j = i; j >= 0; j--)                    /* its subgroup and group */
                if (c->all[j].kind < 2 && c->all[j].group == c->all[i].group && (c->all[j].kind == 0 || c->all[j].type == c->all[i].type))
                    keep[j] = 1;
        }
        for (i = 0; i < c->nall; i++)
            if (keep[i])
                c->vis[n++] = c->all[i];
        free(keep);
    }
    c->pk.n = n;
    for (i = 0; i < n; i++)
        if (c->vis[i].kind == 2) {
            first_item = i;
            break;
        }
    c->pk.sel = first_item >= 0 ? first_item : 0;
}

/* ---- writing settings.json ---- */

/* the array at settings[key] as strings (malloc'ed, each malloc'ed); the file text read to find it */
typedef struct slist {
    char **s;
    int n;
} slist;

static void sl_free(slist *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->s[i]);
    free(l->s);
    l->s = 0;
    l->n = 0;
}

static int sl_add(slist *l, const char *s)
{
    char **n = (char **)realloc(l->s, sizeof(char *) * (size_t)(l->n + 1));
    char *c = (char *)malloc(strlen(s) + 1);
    if (!n || !c) {
        free(c);
        return -1;
    }
    l->s = n;
    strcpy(c, s);
    l->s[l->n++] = c;
    return 0;
}

static void sl_from(jv arr, slist *l)
{
    jit it;
    jv e;
    char b[400];
    if (json_type(arr) != J_ARR)
        return;
    json_iter(arr, &it);
    while (json_next(&it, 0, &e))
        if (json_type(e) == J_STR) {
            json_str(e, b, sizeof(b));
            sl_add(l, b);
        }
}

/* the entries of `list` but those that name `pattern` (with a !, + or - in front or not), then the one new entry */
static void patterns_update(slist *l, const char *pattern, int enable)
{
    slist out;
    int i;
    char ne[420];
    memset(&out, 0, sizeof(out));
    for (i = 0; i < l->n; i++) {
        const char *s = l->s[i], *stripped = (s[0] == '!' || s[0] == '+' || s[0] == '-') ? s + 1 : s;
        if (strcmp(stripped, pattern))
            sl_add(&out, s);
    }
    ne[0] = enable ? '+' : '-';
    ne[1] = 0;
    cl_cat(ne, pattern, sizeof(ne));
    sl_add(&out, ne);
    sl_free(l);
    *l = out;
}

static void sl_json(const slist *l, jw *w)
{
    int i;
    jw_rawz(w, "[");
    for (i = 0; i < l->n; i++) {
        if (i)
            jw_rawz(w, ",");
        jw_strz(w, l->s[i]);
    }
    jw_rawz(w, "]");
}

static int toggle_top(cfg *c, const citem *it, const cgroup *g, int enable)
{
    long n = 0;
    char *text = pi_store_read(c->e->sys, c->e->agent_dir, "settings.json", &n), pat[320];
    jv root, arr;
    slist l;
    jw w;
    int rc;
    memset(&l, 0, sizeof(l));
    if (text && !json_parse(text, n, &root) && json_get(root, type_key[it->type], &arr))
        sl_from(arr, &l);
    free(text);
    rel_path(g->base[0] ? g->base : c->dir, it->path, pat, sizeof(pat));
    patterns_update(&l, pat, enable);
    jw_init(&w);
    sl_json(&l, &w);
    rc = w.oom ? -1 : pi_store_set_path(c->e->sys, c->e->agent_dir, type_key[it->type], w.p);
    jw_free(&w);
    sl_free(&l);
    return rc;
}

/* togglePackageResource: the package's entry of "packages" becomes an object holding the type's patterns; with no filters left it is the
 * source string again */
static int toggle_package(cfg *c, const citem *it, const cgroup *g, int enable)
{
    long n = 0;
    char *text = pi_store_read(c->e->sys, c->e->agent_dir, "settings.json", &n), pat[320];
    jv root, arr, el, src, k, v;
    jit ia, im;
    jw w;
    int rc = -1, at = -1, i = 0;
    if (!text || json_parse(text, n, &root) || !json_get(root, "packages", &arr) || json_type(arr) != J_ARR) {
        free(text);
        return -1;
    }
    json_iter(arr, &ia);
    while (json_next(&ia, 0, &el)) {
        char sb[400];
        int match = 0;
        if (json_type(el) == J_STR) {
            json_str(el, sb, sizeof(sb));
            match = !strcmp(sb, g->src);
        } else if (json_type(el) == J_OBJ && json_get(el, "source", &src) && json_type(src) == J_STR) {
            json_str(src, sb, sizeof(sb));
            match = !strcmp(sb, g->src);
        }
        if (match && at < 0)
            at = i;
        i++;
    }
    if (at < 0) {
        free(text);
        return -1;
    }
    jw_init(&w);
    jw_rawz(&w, "[");
    i = 0;
    json_iter(arr, &ia);
    while (json_next(&ia, 0, &el)) {
        if (i)
            jw_rawz(&w, ",");
        if (i != at)
            jw_raw(&w, el.p, el.n);
        else {
            slist l;
            int filters = 0, wrote = 0;
            jw body;
            memset(&l, 0, sizeof(l));
            jw_init(&body);
            if (json_type(el) == J_OBJ && json_get(el, type_key[it->type], &v))
                sl_from(v, &l);
            if (g->base[0])
                rel_path(g->base, it->path, pat, sizeof(pat));
            else {                                      /* dirname(path) is the base */
                char *sl = strrchr(it->path, '/');
                cl_copy(pat, sl ? sl + 1 : it->path, sizeof(pat));
            }
            patterns_update(&l, pat, enable);
            jw_rawz(&body, "{");
            if (json_type(el) == J_OBJ) {
                json_iter(el, &im);
                while (json_next(&im, &k, &v)) {
                    if (json_streq(k, type_key[it->type]))
                        continue;
                    if (wrote++)
                        jw_rawz(&body, ",");
                    jw_raw(&body, k.p, k.n);
                    jw_rawz(&body, ":");
                    jw_raw(&body, v.p, v.n);
                    if (json_streq(k, "skills") || json_streq(k, "prompts") || json_streq(k, "extensions") || json_streq(k, "themes"))
                        filters++;
                }
            } else {
                jw_rawz(&body, "\"source\":");
                jw_strz(&body, g->src);
                wrote = 1;
            }
            if (l.n) {
                if (wrote++)
                    jw_rawz(&body, ",");
                jw_strz(&body, type_key[it->type]);
                jw_rawz(&body, ":");
                sl_json(&l, &body);
                filters++;
            }
            jw_rawz(&body, "}");
            if (!filters && body.p)
                jw_strz(&w, g->src);                    /* no filters left: the plain source string */
            else if (body.p)
                jw_rawz(&w, body.p);
            sl_free(&l);
            jw_free(&body);
        }
        i++;
    }
    jw_rawz(&w, "]");
    rc = w.oom ? -1 : pi_store_set_path(c->e->sys, c->e->agent_dir, "packages", w.p);
    jw_free(&w);
    free(text);
    return rc;
}

static int toggle_item(cfg *c, int idx)
{
    citem *it = &c->it[idx];
    const cgroup *g = &c->g[it->group];
    int enable = !it->enabled, rc;
    rc = g->kind == PMK_PACKAGE ? toggle_package(c, it, g, enable) : toggle_top(c, it, g, enable);
    if (!rc)
        it->enabled = enable;
    return rc;
}

/* ---- the screen ---- */

static void role(const cfg *c, int r, const char *text, char *out, long cap)
{
    char sg[48];
    pitheme_sgr(c->e->th, r, sg);
    cl_cat(out, sg, cap);
    cl_cat(out, text, cap);
    cl_cat(out, "\033[0m", cap);
}

static void header(cfg *c)
{
    char title[40], hint[160], left[200], b[300];
    int w, i, sp;
    cl_copy(title, "Global Resources", sizeof(title));
    /* rawKeyHint("space", "toggle"), rawKeyHint("esc", "close"): the key dim, the words muted, joined by a muted " . " */
    hint[0] = 0;
    role(c, PR_DIM, "space", hint, sizeof(hint));
    role(c, PR_MUTED, " toggle", hint, sizeof(hint));
    role(c, PR_MUTED, " \302\267 ", hint, sizeof(hint));
    role(c, PR_DIM, "esc", hint, sizeof(hint));
    role(c, PR_MUTED, " close", hint, sizeof(hint));
    {
        static const char plain[] = "space toggle \302\267 esc close";
        w = vw_width(title, (long)strlen(title)) + vw_width(plain, (long)strlen(plain));
    }
    sp = c->cols - w;
    if (sp < 1)
        sp = 1;
    left[0] = 0;
    cl_cat(left, "\033[1m", sizeof(left));
    cl_cat(left, title, sizeof(left));
    cl_cat(left, "\033[22m", sizeof(left));
    for (i = 0; i < sp; i++)
        cl_cat(left, " ", sizeof(left));
    cl_cat(left, hint, sizeof(left));
    c->head[0] = '\002';
    c->head[1] = 0;
    cl_cat(c->head, "\n\001\n\n", sizeof(c->head));
    cl_cat(c->head, left, sizeof(c->head));
    cl_cat(c->head, "\n", sizeof(c->head));
    b[0] = 0;
    {
        char sf[300];
        cl_copy(sf, c->dir, sizeof(sf));
        cl_cat(sf, sf[0] && sf[strlen(sf) - 1] != '/' && sf[strlen(sf) - 1] != ':' ? "/settings.json" : "settings.json", sizeof(sf));
        role(c, PR_MUTED, sf, b, sizeof(b));
    }
    cl_cat(c->head, b, sizeof(c->head));
    cl_cat(c->head, "\n", sizeof(c->head));
    c->pk.top = c->head;
}

static void cf_row(void *u, int i, int selected, char *out, long cap)
{
    cfg *c = (cfg *)u;
    const centry *e = &c->vis[i];
    out[0] = 0;
    if (e->kind == 0) {
        char b[340];
        cl_copy(b, c->g[e->group].label, sizeof(b));
        cl_cat(out, "  ", cap);
        cl_cat(out, "\033[1m", cap);
        role(c, PR_ACCENT, b, out, cap);
        cl_cat(out, "\033[22m", cap);
    } else if (e->kind == 1) {
        cl_cat(out, "    ", cap);
        role(c, PR_MUTED, type_label[e->type], out, cap);
    } else {
        const citem *it = &c->it[e->item];
        cl_cat(out, selected ? "> " : "  ", cap);
        cl_cat(out, "    ", cap);
        if (it->enabled)
            role(c, PR_SUCCESS, "[x]", out, cap);
        else
            role(c, PR_DIM, "[ ]", out, cap);
        cl_cat(out, " ", cap);
        if (selected) {
            cl_cat(out, "\033[1m", cap);
            cl_cat(out, it->name, cap);
            cl_cat(out, "\033[22m", cap);
        } else
            cl_cat(out, it->name, cap);
    }
}

static void cf_tail(void *u, char *out, long cap)
{
    cfg *c = (cfg *)u;
    int st, en;
    out[0] = 0;
    if (!c->pk.n) {
        role(c, PR_MUTED, "  No resources found", out, cap);
        cl_cat(out, "\n", cap);
        return;
    }
    st = c->pk.sel - c->maxv / 2;
    if (st > c->pk.n - c->maxv)
        st = c->pk.n - c->maxv;
    if (st < 0)
        st = 0;
    en = st + c->maxv < c->pk.n ? st + c->maxv : c->pk.n;
    if (st > 0 || en < c->pk.n) {
        int total = 0, cur = 1, i;
        char b[40], num[16];
        for (i = 0; i < c->pk.n; i++)
            if (c->vis[i].kind == 2) {
                total++;
                if (i < c->pk.sel)
                    cur++;
            }
        cl_copy(b, "  (", sizeof(b));
        cl_ltoa(cur, num);
        cl_cat(b, num, sizeof(b));
        cl_cat(b, "/", sizeof(b));
        cl_ltoa(total, num);
        cl_cat(b, num, sizeof(b));
        cl_cat(b, ")", sizeof(b));
        role(c, PR_DIM, b, out, cap);
        cl_cat(out, "\n", cap);              /* the indicator, then the Spacer */
    }
}

static int next_item(const cfg *c, int from, int dir)
{
    int i = from + dir;
    while (i >= 0 && i < c->pk.n) {
        if (c->vis[i].kind == 2)
            return i;
        i += dir;
    }
    return from;
}

static int cf_key(void *u, const cl_key *key, int act)
{
    cfg *c = (cfg *)u;
    (void)act;
    if (key->k == K_UP || key->k == K_DOWN) {
        c->pk.sel = next_item(c, c->pk.sel, key->k == K_UP ? -1 : 1);
        return 1;
    }
    if (key->k == K_PGUP) {
        int t = c->pk.sel - c->maxv;
        if (t < 0)
            t = 0;
        while (t < c->pk.n && c->vis[t].kind != 2)
            t++;
        if (t < c->pk.n)
            c->pk.sel = t;
        return 1;
    }
    if (key->k == K_PGDN) {
        int t = c->pk.sel + c->maxv;
        if (t > c->pk.n - 1)
            t = c->pk.n - 1;
        while (t >= 0 && c->vis[t].kind != 2)
            t--;
        if (t >= 0)
            c->pk.sel = t;
        return 1;
    }
    if (key->k == K_ESC || (key->k == K_CTRL && key->ch == 'c')) {
        c->pk.done = 1;
        return 1;
    }
    if (key->k == K_ENTER || (key->k == K_CHAR && key->ch == ' ')) {
        if (c->pk.n && c->vis[c->pk.sel].kind == 2)
            toggle_item(c, c->vis[c->pk.sel].item);
        return 1;
    }
    if (key->k == K_TAB)
        return 1;                                       /* the project mode is not built */
    if (key->k == K_CHAR || key->k == K_BS || key->k == K_PASTE) {
        long ql = (long)strlen(c->pk.query);
        switch (key->k) {
        case K_BS:
            if (ql) {
                do
                    ql--;
                while (ql > 0 && ((unsigned char)c->pk.query[ql] & 0xc0) == 0x80);
                c->pk.query[ql] = 0;
            }
            break;
        case K_CHAR:
            if (key->ch >= 0x20 && ql + 5 < (long)sizeof(c->pk.query)) {
                ql += vw_put_utf8(c->pk.query + ql, key->ch);
                c->pk.query[ql] = 0;
            }
            break;
        default: {
            long i;
            for (i = 0; i < key->n && ql + 1 < (long)sizeof(c->pk.query); i++)
                if ((unsigned char)key->text[i] >= 0x20)
                    c->pk.query[ql++] = key->text[i];
            c->pk.query[ql] = 0;
            break;
        }
        }
        filter(c);
    }
    return 1;
}

/* ---- the numbered list of a console with no screen ---- */

static void lines(cfg *c, cl_io *io)
{
    char l[500], num[16];
    int i, k = 0;
    for (i = 0; i < c->nall; i++) {
        const centry *e = &c->all[i];
        if (e->kind == 0) {
            cl_copy(l, c->g[e->group].label, sizeof(l));
        } else if (e->kind == 1) {
            cl_copy(l, "  ", sizeof(l));
            cl_cat(l, type_label[e->type], sizeof(l));
        } else {
            const citem *it = &c->it[e->item];
            cl_copy(l, "    ", sizeof(l));
            cl_ltoa(++k, num);
            cl_cat(l, num, sizeof(l));
            cl_cat(l, ". ", sizeof(l));
            cl_cat(l, it->enabled ? "[x] " : "[ ] ", sizeof(l));
            cl_cat(l, it->name, sizeof(l));
        }
        cl_cat(l, "\n", sizeof(l));
        io->write(io->u, l, (long)strlen(l));
    }
}

static int line_item(cfg *c, int num)
{
    int i, k = 0;
    for (i = 0; i < c->nall; i++)
        if (c->all[i].kind == 2 && ++k == num)
            return c->all[i].item;
    return -1;
}

static void run_lines(cfg *c)
{
    cl_io *io = c->e->io;
    char buf[64];
    static const char head[] = "Global Resources\n";
    io->write(io->u, head, (long)sizeof(head) - 1);
    for (;;) {
        static const char ask[] = "Number to switch on or off, empty to close: ";
        long n;
        int it;
        lines(c, io);
        io->write(io->u, ask, (long)sizeof(ask) - 1);
        if (!io->read_line)
            return;
        n = io->read_line(io->u, buf, sizeof(buf));
        if (n <= 0 || !buf[0] || buf[0] == '\n' || buf[0] == '\r')
            return;
        it = line_item(c, (int)strtol(buf, 0, 10));
        if (it < 0) {
            static const char bad[] = "[ERROR] no such number\n";
            io->write(io->u, bad, (long)sizeof(bad) - 1);
            continue;
        }
        if (toggle_item(c, it)) {
            static const char bad[] = "[ERROR] settings.json could not be written\n";
            io->write(io->u, bad, (long)sizeof(bad) - 1);
        }
    }
}

int pi_config_run(const pi_config_env *e)
{
    static cl_tui t;
    cfg *c = (cfg *)calloc(1, sizeof(cfg));
    int rc = 0;
    if (!c)
        return -1;
    c->e = e;
    if (load(c) || flatten(c)) {
        rc = -1;
        goto out;
    }
    if (!e->io->read || !e->io->raw || tui_init(&t, e->io)) {
        run_lines(c);
        goto out;
    }
    km_free(&t.km);
    if (e->kb)
        km_load_pi(&t.km, e->kb, e->kbn);
    else
        km_init_pi(&t.km);
    t.look = TUI_LOOK_PI;
    t.pith = e->th;
    t.screen = TUI_SCREEN_REGULAR;
    t.pi_nofoot = 1;
    if (tui_start(&t)) {
        tui_free(&t);
        run_lines(c);
        goto out;
    }
    c->cols = t.cols;
    c->maxv = t.rows - 8 < 5 ? 5 : t.rows - 8;          /* ResourceList: eight rows of the screen are chrome */
    header(c);
    c->pk.search = 1;
    c->pk.maxrows = c->maxv;
    c->pk.row = cf_row;
    c->pk.key = cf_key;
    c->pk.tail = cf_tail;
    c->pk.u = c;
    c->pk.layout = TUI_PICK_FRAME;
    filter(c);
    tui_pick_run(&t, &c->pk);
    tui_stop(&t);
    tui_free(&t);
out:
    free(c->it);
    free(c->all);
    free(c->vis);
    free(c);
    return rc;
}
