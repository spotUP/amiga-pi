/* see templates.h */
#include <stdlib.h>
#include <string.h>
#include "templates.h"
#include "frontmatter.h"
#include "claude/commands.h"
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

/* t appended unless the name is taken (dedupePrompts: the first keeps it): 0 added, 1 taken, -1 out of memory */
static int add(pi_tmpls *ts, const pi_tmpl *t)
{
    int i;
    for (i = 0; i < ts->n; i++)
        if (!strcmp(ts->t[i].name, t->name))
            return 1;
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

/* one file as pi's loadTemplateFromFile: 0 added or dropped, -1 out of memory */
static int load_file(pi_tmpls *ts, cl_sys *sys, const char *dir, const char *file, int scope)
{
    pi_tmpl t;
    char path[300], *raw = 0, *norm, *body;
    long n = 0, i, o = 0, nl;
    size_t fl = strlen(file);
    cl_def d;
    int rc = 0, fm = 0;
    memset(&t, 0, sizeof(t));
    if (path_join(dir, file, path, sizeof(path)) || sys->kind(sys->u, path) != 1 || sys->read(sys->u, path, 1024L * 1024, &raw, &n))
        return 0;
    pi_tmpl_reads++;
    /* stripBom, then \r\n and \r to \n (utils/frontmatter.ts) */
    i = n >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF ? 3 : 0;
    norm = (char *)malloc((size_t)n + 1);
    if (!norm) {
        CL_FREE(raw);
        return -1;
    }
    for (; i < n; i++) {
        if (raw[i] == '\r') {
            norm[o++] = '\n';
            if (i + 1 < n && raw[i + 1] == '\n')
                i++;
        } else
            norm[o++] = raw[i];
    }
    norm[o] = 0;
    CL_FREE(raw);
    nl = o;
    body = norm;
    memset(&d, 0, sizeof(d));
    if (nl >= 3 && !strncmp(norm, "---", 3)) {
        /* extractFrontmatter: the first "\n---" from index 3 closes it; an empty block is no frontmatter */
        char *end = 0, *q;
        for (q = norm + 3; (q = strchr(q, '\n')) != 0; q++)
            if (!strncmp(q, "\n---", 4)) {
                end = q;
                break;
            }
        if (end) {
            const unsigned char *b = (const unsigned char *)end + 4, *e = (const unsigned char *)norm + nl;
            while (b < e && sp_len(b))
                b += sp_len(b);
            while (e > b && sp_back(b, e))
                e -= sp_back(b, e);
            body = dupn((const char *)b, (long)(e - b));
            fm = end - norm > 4;
            if (fm && defs_parse(norm, nl, &d)) {
                free(body);
                free(norm);
                def_free(&d);
                return -1;
            }
            if (!body) {
                free(norm);
                def_free(&d);
                return -1;
            }
        }
    }
    if (body == norm)
        body = dupn(norm, nl);
    free(norm);
    if (!body) {
        def_free(&d);
        return -1;
    }
    cl_copy(t.path, path, sizeof(t.path));
    cl_copy(t.name, file, sizeof(t.name));
    if (fl > 3 && !strcmp(file + fl - 3, ".md"))
        t.name[fl - 3] = 0;
    t.scope = scope;
    cl_copy(t.base, dir, sizeof(t.base));
    t.content = body;
    t.description = d.description && d.description[0] ? dupn(d.description, (long)strlen(d.description)) : first_line(body);
    t.hint = dupn(d.hint ? d.hint : "", d.hint ? (long)strlen(d.hint) : 0);
    def_free(&d);
    if (!t.description || !t.hint) {
        tmpl_free(&t);
        return -1;
    }
    rc = add(ts, &t);
    if (rc)
        tmpl_free(&t);
    return rc < 0 ? -1 : 0;
}

typedef struct names {
    char (*n)[108];
    int count, cap;
    int oom;
    int auto_dir;               /* a default directory: hidden files and node_modules left out (an explicit one is read whole) */
} names;

/* collectAutoPromptEntries: the .md files that are not hidden, not node_modules */
static int collect(void *c, const cl_dirent *e)
{
    names *ns = (names *)c;
    size_t l = strlen(e->name);
    if (e->dir || (ns->auto_dir && (e->name[0] == '.' || !strcmp(e->name, "node_modules"))) || l < 3 || strcmp(e->name + l - 3, ".md"))
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

static int load_dir(pi_tmpls *ts, cl_sys *sys, const char *dir, int scope)
{
    names ns;
    int i, rc = 0;
    memset(&ns, 0, sizeof(ns));
    ns.auto_dir = scope != PI_TMPL_PATH;
    if (sys->kind(sys->u, dir) == 2) {
        sys->list(sys->u, dir, collect, &ns);
        if (ns.count > 1)
            qsort(ns.n, (size_t)ns.count, 108, cmp_name);     /* the directory's order is the system's: by name */
        for (i = 0; i < ns.count && !rc; i++)
            rc = load_file(ts, sys, dir, ns.n[i], scope);
    }
    if (ns.oom)
        rc = -1;
    free(ns.n);
    return rc;
}

int pi_tmpl_load(pi_tmpls *ts, cl_sys *sys, const char *agent_dir, const char *cwd, int trusted, int no_defaults,
                 char (*paths)[300], int npaths)
{
    char dir[300];
    int rc = 0, i;
    memset(ts, 0, sizeof(*ts));
    if (cwd && cwd[0] && path_join(cwd, ".pi", ts->base_project, sizeof(ts->base_project)))
        ts->base_project[0] = 0;
    if (agent_dir)
        cl_copy(ts->base_user, agent_dir, sizeof(ts->base_user));
    /* loadPromptTemplates' explicit paths: a directory's .md files, or one .md file */
    for (i = 0; i < npaths && !rc; i++) {
        int k;
        if (path_join(cwd ? cwd : "", paths[i], dir, sizeof(dir)))
            continue;
        k = sys->kind(sys->u, dir);
        if (k == 2)
            rc = load_dir(ts, sys, dir, PI_TMPL_PATH);
        else if (k == 1 && strlen(dir) > 3 && !strcmp(dir + strlen(dir) - 3, ".md")) {
            char *sl = strrchr(dir, '/'), *co = strrchr(dir, ':'), *base = sl && (!co || sl > co) ? sl : co;
            if (base) {
                char file[108];
                cl_copy(file, base + 1, sizeof(file));
                if (base == sl)
                    *base = 0;
                else
                    base[1] = 0;
                rc = load_file(ts, sys, dir, file, PI_TMPL_PATH);
            }
        }
    }
    if (no_defaults)
        return rc;
    /* the project's first: a name in both is the project's (package-manager.ts adds the project's resources, then the user's) */
    if (!rc && trusted && ts->base_project[0] && !path_join(ts->base_project, "prompts", dir, sizeof(dir)))
        rc = load_dir(ts, sys, dir, PI_TMPL_PROJECT);
    if (!rc && agent_dir && agent_dir[0] && !path_join(agent_dir, "prompts", dir, sizeof(dir)))
        rc = load_dir(ts, sys, dir, PI_TMPL_USER);
    return rc;
}

void pi_tmpl_free(pi_tmpls *ts)
{
    int i;
    for (i = 0; i < ts->n; i++)
        tmpl_free(&ts->t[i]);
    free(ts->t);
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
        jw_rawz(&hp, t->scope == PI_TMPL_PROJECT ? "[p]" : t->scope == PI_TMPL_USER ? "[u]" : "[t]");
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
