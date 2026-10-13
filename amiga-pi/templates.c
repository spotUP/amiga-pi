/* see templates.h */
#include <stdlib.h>
#include <string.h>
#include "templates.h"
#include "frontmatter.h"
#include "pkgres.h"
#include "claude/path.h"
#include "claude/util.h"

long pi_tmpl_reads, pi_tmpl_expansions;

static char *dupn(const char *s, long n)
{
    char *d = (char *)malloc((size_t)n + 1);
    if (d) {
        memcpy(d, s, (size_t)n);
        d[n] = 0;
    }
    return d;
}

#define sp_len pi_js_space_len   /* the same table as JavaScript's trim() (frontmatter.h) */
#define sp_back pi_js_space_back

int pi_tmpl_args(const char *str, char ***out, int *n)
{
    const unsigned char *s = (const unsigned char *)str;
    char **a = 0;
    int na = 0, cap = 0;
    jw cur;
    char q = 0;
    *out = 0;
    *n = 0;
    jw_init(&cur);
    for (;;) {
        int sl = *s ? sp_len(s) : 0;
        if (!*s || (!q && sl)) {
            if (cur.n) {
                if (na == cap) {
                    char **g = (char **)realloc(a, sizeof(char *) * (size_t)(cap ? cap * 2 : 8));
                    if (!g)
                        goto oom;
                    a = g;
                    cap = cap ? cap * 2 : 8;
                }
                a[na] = (char *)malloc((size_t)cur.n + 1);
                if (!a[na])
                    goto oom;
                memcpy(a[na], cur.p, (size_t)cur.n + 1);
                na++;
                jw_reset(&cur);
            }
            if (!*s)
                break;
            s += sl;
            continue;
        }
        if (q) {
            if (*s == (unsigned char)q)
                q = 0;
            else
                jw_raw(&cur, (const char *)s, 1);
            s++;
        } else if (*s == '"' || *s == '\'') {
            q = (char)*s++;
        } else {
            jw_raw(&cur, (const char *)s, 1);
            s++;
        }
    }
    if (cur.oom)
        goto oom;
    jw_free(&cur);
    *out = a;
    *n = na;
    return 0;
oom:
    jw_free(&cur);
    pi_tmpl_args_free(a, na);
    return -1;
}

void pi_tmpl_args_free(char **a, int n)
{
    int i;
    for (i = 0; i < n; i++)
        free(a[i]);
    free(a);
}

#define NUM_MAX 1000000000L

/* the digits at *p as parseInt reads them, saturated; *p moved past them */
static long digits(const char **p)
{
    long v = 0;
    while (**p >= '0' && **p <= '9') {
        if (v < NUM_MAX)
            v = v * 10 + (**p - '0');
        (*p)++;
    }
    return v > NUM_MAX ? NUM_MAX : v;
}

/* the part of args from index start (0-based) for count of them, joined with spaces */
static void slice_args(jw *out, char *const *args, int nargs, long start, long count)
{
    long i;
    if (start >= nargs)
        return;
    if (count > nargs - start)
        count = nargs - start;
    for (i = 0; i < count; i++) {
        if (i)
            jw_raw(out, " ", 1);
        jw_rawz(out, args[start + i]);
    }
}

void pi_tmpl_subst(jw *out, const char *c, char *const *args, int nargs)
{
    while (*c) {
        const char *p = c + 1, *e;
        if (*c != '$') {
            jw_raw(out, c, 1);
            c++;
            continue;
        }
        if (*p == '{') {
            /* ${N:-d}, ${@:-d}, ${ARGUMENTS:-d}: the default when the value is empty or missing */
            const char *q = p + 1;
            int all = 0, ok = 0;
            long num = 0;
            if (*q == '@') {
                all = 1;
                q++;
                ok = 1;
            } else if (!strncmp(q, "ARGUMENTS", 9)) {
                all = 1;
                q += 9;
                ok = 1;
            } else if (*q >= '0' && *q <= '9') {
                num = digits(&q);
                ok = 1;
            }
            if (ok && q[0] == ':' && q[1] == '-' && (e = strchr(q + 2, '}')) != 0) {
                jw v;
                jw_init(&v);
                if (all)
                    slice_args(&v, args, nargs, 0, nargs);
                else if (num >= 1 && num <= nargs)
                    jw_rawz(&v, args[num - 1]);
                if (v.n)
                    jw_raw(out, v.p, v.n);
                else
                    jw_raw(out, q + 2, (long)(e - q - 2));
                jw_free(&v);
                c = e + 1;
                continue;
            }
            /* ${@:N} and ${@:N:L}: N counts from 1 (0 is 1) */
            if (p[1] == '@' && p[2] == ':' && p[3] >= '0' && p[3] <= '9') {
                long start, len = -1;
                q = p + 3;
                start = digits(&q) - 1;
                if (start < 0)
                    start = 0;
                if (q[0] == ':' && q[1] >= '0' && q[1] <= '9') {
                    q++;
                    len = digits(&q);
                }
                if (q[0] == '}') {
                    slice_args(out, args, nargs, start, len < 0 ? nargs : len);
                    c = q + 1;
                    continue;
                }
            }
            jw_raw(out, c, 1);
            c++;
            continue;
        }
        /* $ARGUMENTS, $@, $N */
        if (!strncmp(p, "ARGUMENTS", 9)) {
            slice_args(out, args, nargs, 0, nargs);
            c = p + 9;
        } else if (*p == '@') {
            slice_args(out, args, nargs, 0, nargs);
            c = p + 1;
        } else if (*p >= '0' && *p <= '9') {
            long num = digits(&p);
            if (num >= 1 && num <= nargs)
                jw_rawz(out, args[num - 1]);
            c = p;
        } else {
            jw_raw(out, c, 1);
            c++;
        }
    }
}

/* ---- loading ---- */

static void tmpl_free(pi_tmpl *t)
{
    free(t->description);
    free(t->hint);
    free(t->content);
}

/* pi's description when the frontmatter has none: the first line with something on it, 60 UTF-16 units, "..." after
 * a longer one */
static char *first_line(const char *body)
{
    const char *l = body;
    while (*l) {
        const char *e = strchr(l, '\n'), *k;
        if (!e)
            e = l + strlen(l);
        k = l;
        while (k < e && sp_len((const unsigned char *)k))
            k += sp_len((const unsigned char *)k);
        if (k < e) {
            long units = 0, b = 0, len = (long)(e - l);
            char *d;
            while (b < len) {
                unsigned char ch = (unsigned char)l[b];
                int cl = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4, u = cl == 4 ? 2 : 1;
                if (units + u > 60)
                    break;
                units += u;
                b += cl;
            }
            if (b > len)
                b = len;
            d = (char *)malloc((size_t)b + 4);
            if (!d)
                return 0;
            memcpy(d, l, (size_t)b);
            if (b < len) {
                memcpy(d + b, "...", 3);
                b += 3;
            }
            d[b] = 0;
            return d;
        }
        if (!*e)
            break;
        l = e + 1;
    }
    return dupn("", 0);
}

/* diagnostics, as pi's ResourceDiagnostic: a warning for a file that cannot be read or whose YAML is refused, an error for a
 * --prompt-template path that is not there, a collision for a name two files have */
static int add_diag(pi_tmpls *ts, int type, const char *msg, const char *path, const char *winner, const char *name)
{
    pi_tmpl_diag *d;
    if (ts->nd == ts->capd) {
        int nc = ts->capd ? ts->capd * 2 : 8;
        pi_tmpl_diag *q = (pi_tmpl_diag *)realloc(ts->d, sizeof(pi_tmpl_diag) * (size_t)nc);
        if (!q)
            return -1;
        ts->d = q;
        ts->capd = nc;
    }
    d = &ts->d[ts->nd];
    memset(d, 0, sizeof(*d));
    d->type = type;
    d->msg = dupn(msg, (long)strlen(msg));
    d->name = name ? dupn(name, (long)strlen(name)) : 0;
    cl_copy(d->path, path ? path : "", sizeof(d->path));
    cl_copy(d->winner, winner ? winner : "", sizeof(d->winner));
    if (!d->msg || (name && !d->name))
        return -1;
    ts->nd++;
    return 0;
}

/* t appended (every template found; dedupePrompts runs once all are in): 0, -1 out of memory */
static int add(pi_tmpls *ts, const pi_tmpl *t)
{
    if (ts->n == ts->cap) {
        pi_tmpl *g = (pi_tmpl *)realloc(ts->t, sizeof(pi_tmpl) * (size_t)(ts->cap ? ts->cap * 2 : 8));
        if (!g)
            return -1;
        ts->t = g;
        ts->cap = ts->cap ? ts->cap * 2 : 8;
    }
    ts->t[ts->n++] = *t;
    return 0;
}

/* mergePaths' result: the paths in order, each once by its canonical name; explicit_[i] says it came from --prompt-template */
typedef struct sl_paths {
    char **v;
    int *explicit_;
    int n, cap;
} sl_paths;

static int mp_add(sl_paths *l, cl_sys *sys, const char *path, int explicit_flag)
{
    char canon[300], c2[300];
    int i;
    pm_canon(sys, path, canon, sizeof(canon));
    for (i = 0; i < l->n; i++) {
        pm_canon(sys, l->v[i], c2, sizeof(c2));
        if (!strcmp(c2, canon))
            return 0;
    }
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        char **q = (char **)realloc(l->v, sizeof(char *) * (size_t)nc);
        int *e;
        if (!q)
            return -1;
        l->v = q;
        e = (int *)realloc(l->explicit_, sizeof(int) * (size_t)nc);
        if (!e)
            return -1;
        l->explicit_ = e;
        l->cap = nc;
    }
    l->v[l->n] = dupn(path, (long)strlen(path));
    if (!l->v[l->n])
        return -1;
    l->explicit_[l->n] = explicit_flag;
    l->n++;
    return 0;
}

/* where a template's file sits for sourceInfo when it is not one of the resolved resources (loadPromptTemplates getSourceInfo) */
typedef struct tload {
    pi_tmpls *ts;
    cl_sys *sys;
    const pm_list *res;         /* the resolved resources: their metadata is a template's sourceInfo (findSourceInfoForPath) */
    char user_prompts[300], project_prompts[300];
    const char *agent_dir;
    char cwd_pi[300];
} tload;

static int under(const char *path, const char *root)
{
    size_t n = strlen(root);
    return n && !strncmp(path, root, n) && (!path[n] || path[n] == '/' || root[n - 1] == '/' || root[n - 1] == ':');
}

static void template_source(const tload *L, pi_tmpl *t)
{
    int i;
    for (i = 0; L->res && i < L->res->n; i++)
        if (!strcmp(L->res->e[i].path, t->path)) {
            const pm_res *r = &L->res->e[i];
            t->scope = r->scope;
            t->kind = r->kind;
            cl_copy(t->base, r->base, sizeof(t->base));
            cl_copy(t->psrc, r->src, sizeof(t->psrc));
            return;
        }
    t->kind = PMK_LOCAL;
    t->psrc[0] = 0;
    if (L->user_prompts[0] && under(t->path, L->user_prompts)) {
        t->scope = PMS_USER;
        cl_copy(t->base, L->user_prompts, sizeof(t->base));
    } else if (L->project_prompts[0] && under(t->path, L->project_prompts)) {
        t->scope = PMS_PROJECT;
        cl_copy(t->base, L->project_prompts, sizeof(t->base));
    } else
        t->scope = PMS_TEMP;      /* base set by the caller: the directory, or the file's directory */
}

/* one file as pi's loadTemplateFromFile: 0 added or dropped (with its diagnostic), -1 out of memory. dir is the directory the
 * template was found by when a --prompt-template directory named it, else 0 */
static int load_file(tload *L, const char *path, const char *dir)
{
    static const char *const keys[2] = { "description", "argument-hint" };
    pi_tmpls *ts = L->ts;
    pi_tmpl t;
    char *raw = 0, *body = 0, err[600];
    const char *file;
    long n = 0, bodylen = 0;
    pi_yv v[2];
    size_t fl;
    int frc;
    memset(&t, 0, sizeof(t));
    if (L->sys->read(L->sys->u, path, 1024L * 1024, &raw, &n)) {
        const char *why = L->sys->err ? L->sys->err(L->sys->u) : 0;
        return add_diag(ts, TD_WARNING, why && why[0] ? why : "failed to read prompt template file", path, 0, 0);
    }
    pi_tmpl_reads++;
    frc = pi_fm_parse(raw, n, keys, 2, v, &body, &bodylen, err, sizeof(err));
    CL_FREE(raw);
    if (frc == -1)
        return -1;
    if (frc == -2)
        return add_diag(ts, TD_WARNING, err, path, 0, 0);   /* the YAML is refused: pi warns and skips the file */
    cl_copy(t.path, path, sizeof(t.path));
    file = strrchr(path, '/');
    if (!file || (strrchr(path, ':') && strrchr(path, ':') > file))
        file = strrchr(path, ':');
    file = file ? file + 1 : path;
    fl = strlen(file);
    cl_copy(t.name, file, sizeof(t.name));
    if (fl > 3 && !strcmp(file + fl - 3, ".md"))
        t.name[fl - 3] = 0;
    template_source(L, &t);
    if (t.scope == PMS_TEMP && t.kind == PMK_LOCAL && !t.base[0]) {
        if (dir)
            cl_copy(t.base, dir, sizeof(t.base));
        else
            path_parent(path, t.base, sizeof(t.base));
    }
    t.content = body;
    t.description = v[0].kind == YV_STR && v[0].s && v[0].s[0] ? dupn(v[0].s, (long)strlen(v[0].s)) : first_line(body);
    t.hint = dupn(v[1].kind == YV_STR && v[1].s ? v[1].s : "", v[1].kind == YV_STR && v[1].s ? (long)strlen(v[1].s) : 0);
    pi_yv_free(v, 2);
    if (!t.description || !t.hint) {
        tmpl_free(&t);
        return -1;
    }
    if (add(ts, &t)) {
        tmpl_free(&t);
        return -1;
    }
    return 0;
}

typedef struct names {
    char (*n)[108];
    int count, cap;
    int oom;
} names;

/* loadTemplatesFromDir: the .md files of the directory, hidden ones too */
static int collect(void *c, const cl_dirent *e)
{
    names *ns = (names *)c;
    size_t l = strlen(e->name);
    if (e->dir || l < 3 || strcmp(e->name + l - 3, ".md"))
        return 0;
    if (ns->count == ns->cap) {
        char (*g)[108] = (char (*)[108])realloc(ns->n, 108 * (size_t)(ns->cap ? ns->cap * 2 : 16));
        if (!g) {
            ns->oom = 1;
            return 1;
        }
        ns->n = g;
        ns->cap = ns->cap ? ns->cap * 2 : 16;
    }
    memcpy(ns->n[ns->count++], e->name, 108);
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static int load_dir(tload *L, const char *dir)
{
    names ns;
    int i, rc = 0;
    memset(&ns, 0, sizeof(ns));
    L->sys->list(L->sys->u, dir, collect, &ns);
    if (ns.count > 1)
        qsort(ns.n, (size_t)ns.count, 108, cmp_name);     /* readdirSync: libuv sorts by bytes */
    for (i = 0; i < ns.count && !rc; i++) {
        char path[300];
        if (path_join(dir, ns.n[i], path, sizeof(path)) || L->sys->kind(L->sys->u, path) != 1)
            continue;
        rc = load_file(L, path, dir);
    }
    if (ns.oom)
        rc = -1;
    free(ns.n);
    return rc;
}

int pi_tmpl_load(pi_tmpls *ts, cl_sys *sys, const pi_tmpl_opts *o)
{
    pm_opts po;
    pm_list res;
    tload L;
    sl_paths merged;
    int rc = 0, i, j;
    memset(ts, 0, sizeof(*ts));
    memset(&res, 0, sizeof(res));
    memset(&L, 0, sizeof(L));
    memset(&merged, 0, sizeof(merged));
    memset(&po, 0, sizeof(po));
    po.agent_dir = o->agent_dir;
    po.cwd = o->cwd;
    po.trusted = o->trusted;
    po.home = o->home;
    po.host = o->host;
    L.ts = ts;
    L.sys = sys;
    L.res = &res;
    L.agent_dir = o->agent_dir;
    if (o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "prompts", L.user_prompts, sizeof(L.user_prompts)))
        L.user_prompts[0] = 0;
    if (o->cwd && o->cwd[0] && path_join(o->cwd, ".pi", L.cwd_pi, sizeof(L.cwd_pi)) == 0 &&
        path_join(L.cwd_pi, "prompts", L.project_prompts, sizeof(L.project_prompts)))
        L.project_prompts[0] = 0;
    /* pi's DefaultPackageManager.resolve(): settings.json entries and packages, the default directories, in precedence order;
     * -np leaves all of it out */
    if (!o->no_defaults)
        rc = pm_resolve(sys, &po, PM_PROMPTS, &res, 0);
    /* mergePaths: the resolved files, then the --prompt-template paths, a file once by its canonical name */
    for (i = 0; i < res.n && rc == 0; i++)
        if (res.e[i].enabled)
            rc = mp_add(&merged, sys, res.e[i].path, 0);
    for (i = 0; i < o->npaths && rc == 0; i++) {
        char p[300];
        /* main.ts resolveCliPaths (no trim), then resolveResourcePath (trim) */
        if (!o->cwd || pm_path(&po, o->paths[i], o->cwd, 0, p, sizeof(p)) || pm_path(&po, p, o->cwd, 1, p, sizeof(p)))
            continue;
        rc = mp_add(&merged, sys, p, 1);
    }
    /* loadPromptTemplates: a directory's .md files, a .md file; a path that is not there is passed over */
    for (i = 0; i < merged.n && rc == 0; i++) {
        int kind = sys->kind(sys->u, merged.v[i]);
        if (kind == 2)
            rc = load_dir(&L, merged.v[i]);
        else if (kind == 1 && strlen(merged.v[i]) > 3 && !strcmp(merged.v[i] + strlen(merged.v[i]) - 3, ".md"))
            rc = load_file(&L, merged.v[i], 0);
    }
    /* dedupePrompts: the first of two templates with one name keeps it; the other is a collision */
    for (i = 0; i < ts->n && rc == 0;) {
        int dup = -1;
        for (j = 0; j < i; j++)
            if (!strcmp(ts->t[j].name, ts->t[i].name)) {
                dup = j;
                break;
            }
        if (dup < 0) {
            i++;
            continue;
        }
        {
            char m[200];
            cl_copy(m, "name \"/", sizeof(m));
            cl_cat(m, ts->t[i].name, sizeof(m));
            cl_cat(m, "\" collision", sizeof(m));
            rc = add_diag(ts, TD_COLLISION, m, ts->t[i].path, ts->t[dup].path, ts->t[i].name);
        }
        tmpl_free(&ts->t[i]);
        memmove(&ts->t[i], &ts->t[i + 1], sizeof(pi_tmpl) * (size_t)(ts->n - i - 1));
        ts->n--;
    }
    /* reload(): a --prompt-template path that is not there is an error (unless a warning already names it) */
    for (i = 0; i < merged.n && rc == 0; i++)
        if (merged.explicit_[i] && sys->kind(sys->u, merged.v[i]) <= 0) {
            int have = 0;
            for (j = 0; j < ts->nd; j++)
                if (!strcmp(ts->d[j].path, merged.v[i]))
                    have = 1;
            if (!have)
                rc = add_diag(ts, TD_ERROR, "Prompt template path does not exist", merged.v[i], 0, 0);
        }
    for (i = 0; i < merged.n; i++)
        free(merged.v[i]);
    free(merged.v);
    free(merged.explicit_);
    pm_free(&res);
    return rc;
}

void pi_tmpl_free(pi_tmpls *ts)
{
    int i;
    for (i = 0; i < ts->n; i++)
        tmpl_free(&ts->t[i]);
    free(ts->t);
    for (i = 0; i < ts->nd; i++) {
        free(ts->d[i].msg);
        free(ts->d[i].name);
    }
    free(ts->d);
    for (i = 0; ts->menu_text && i < ts->n * 2; i++)
        free(ts->menu_text[i]);
    free(ts->menu_text);
    free(ts->menu);
    memset(ts, 0, sizeof(*ts));
}

int pi_tmpl_expand(const pi_tmpls *ts, const char *text, jw *out)
{
    const char *p = text + 1, *name_end;
    char **args = 0;
    int i, na = 0, hit = 0;
    if (!ts || text[0] != '/')
        return 0;
    while (*p && !sp_len((const unsigned char *)p))
        p++;
    name_end = p;
    if (name_end == text + 1)
        return 0;
    for (i = 0; i < ts->n; i++)
        if (strlen(ts->t[i].name) == (size_t)(name_end - text - 1) && !strncmp(ts->t[i].name, text + 1, (size_t)(name_end - text - 1))) {
            hit = 1;
            break;
        }
    if (!hit)
        return 0;
    while (*p && sp_len((const unsigned char *)p))     /* \s+ takes all the space: the arguments start at the next word */
        p += sp_len((const unsigned char *)p);
    if (pi_tmpl_args(p, &args, &na))
        return 0;
    pi_tmpl_expansions++;
    pi_tmpl_subst(out, ts->t[i].content, args, na);
    pi_tmpl_args_free(args, na);
    return 1;
}

const cl_cmd *pi_tmpl_menu(pi_tmpls *ts, int *n)
{
    int i;
    *n = ts->n;
    if (ts->menu || !ts->n)
        return ts->menu;
    ts->menu = (cl_cmd *)calloc((size_t)ts->n, sizeof(cl_cmd));
    ts->menu_text = (char **)calloc((size_t)ts->n * 2, sizeof(char *));
    if (!ts->menu || !ts->menu_text) {
        free(ts->menu);
        free(ts->menu_text);
        ts->menu = 0;
        ts->menu_text = 0;
        *n = 0;
        return 0;
    }
    for (i = 0; i < ts->n; i++) {
        const pi_tmpl *t = &ts->t[i];
        jw nm, hp;
        jw_init(&nm);
        jw_init(&hp);
        jw_raw(&nm, "/", 1);
        jw_rawz(&nm, t->name);
        if (t->hint[0]) {
            jw_rawz(&hp, t->hint);
            jw_rawz(&hp, " \342\200\224 ");
        }
        jw_rawz(&hp, t->scope == PMS_PROJECT ? "[p]" : t->scope == PMS_USER ? "[u]" : "[t]");
        if (t->description[0]) {
            jw_raw(&hp, " ", 1);
            jw_rawz(&hp, t->description);
        }
        ts->menu_text[2 * i] = dupn(nm.p, nm.n);
        ts->menu_text[2 * i + 1] = dupn(hp.p, hp.n);
        ts->menu[i].name = ts->menu_text[2 * i];
        ts->menu[i].help = ts->menu_text[2 * i + 1];
        jw_free(&nm);
        jw_free(&hp);
    }
    return ts->menu;
}

int pi_tmpls_commands(const pi_tmpls *ts, jw *out)
{
    int i;
    for (i = 0; i < ts->n; i++) {
        const pi_tmpl *t = &ts->t[i];
        jw_rawz(out, i ? "," : "");
        jw_rawz(out, "{\"name\":");
        jw_strz(out, t->name);
        jw_rawz(out, ",\"description\":");
        jw_strz(out, t->description);
        jw_rawz(out, ",\"source\":\"prompt\",\"sourceInfo\":{\"path\":");
        jw_strz(out, t->path);
        /* the sourceInfo (source-info.ts createSourceInfo from the resource's PathMetadata): the default directories' templates are
         * "auto" of their scope, a --prompt-template path "local" and temporary, a package's the package's source string */
        jw_rawz(out, ",\"source\":");
        if (t->kind == PMK_PACKAGE)
            jw_strz(out, t->psrc);
        else
            jw_rawz(out, t->kind == PMK_AUTO ? "\"auto\"" : "\"local\"");
        jw_rawz(out, t->scope == PMS_PROJECT ? ",\"scope\":\"project\"" : t->scope == PMS_USER ? ",\"scope\":\"user\"" : ",\"scope\":\"temporary\"");
        jw_rawz(out, t->kind == PMK_PACKAGE ? ",\"origin\":\"package\"" : ",\"origin\":\"top-level\"");
        if (t->base[0]) {
            jw_rawz(out, ",\"baseDir\":");
            jw_strz(out, t->base);
        }
        jw_rawz(out, "}}");
    }
    return out->oom ? -1 : i;
}
