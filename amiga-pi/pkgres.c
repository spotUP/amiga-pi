/* see pkgres.h */
#include <stdlib.h>
#include <string.h>
#include "pkgres.h"
#include "frontmatter.h"
#include "claude/path.h"
#include "claude/util.h"
#include "claude/glob.h"
#include "claude/ignore.h"
#include "claude/json.h"

#define PM_FILE_MAX (256L * 1024)
#define PM_DEPTH 16
#define PM_P 300

/* ---- strings ---- */

typedef struct sl {
    char **v;
    int n, cap;
} sl;

static int sl_add(sl *l, const char *s)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 8;
        char **q = (char **)realloc(l->v, sizeof(char *) * (size_t)nc);
        if (!q)
            return -1;
        l->v = q;
        l->cap = nc;
    }
    l->v[l->n] = (char *)malloc(strlen(s) + 1);
    if (!l->v[l->n])
        return -1;
    strcpy(l->v[l->n], s);
    l->n++;
    return 0;
}

static void sl_free(sl *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

static int ends_md(const char *name)
{
    long n = (long)strlen(name);
    return n >= 3 && !strcmp(name + n - 3, ".md");
}

/* the last component of path (a volume's root: the volume) */
static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    if (s)
        return s + 1;
    s = strrchr(path, ':');
    return s ? s + 1 : path;
}

/* ---- paths ---- */

/* host paths: base joined with rel, "." and ".." and empty parts resolved (path.resolve) */
static int host_join(const char *base, const char *rel, char *out, long cap)
{
    char tmp[2 * PM_P + 4];
    long n = 0, i, w;
    const char *p;
    tmp[0] = 0;
    if (rel[0] != '/' && base && base[0]) {
        cl_copy(tmp, base, sizeof(tmp));
        n = (long)strlen(tmp);
    }
    if (n >= (long)sizeof(tmp) - 2)
        return -1;
    tmp[n++] = '/';
    tmp[n] = 0;
    cl_cat(tmp, rel, sizeof(tmp));
    /* the parts */
    w = 0;
    out[0] = 0;
    p = tmp;
    i = 0;
    {
        long len = (long)strlen(tmp);
        long starts[128], nst = 0;
        while (i < len) {
            long j = i;
            while (j < len && p[j] != '/')
                j++;
            if (j == i || (j - i == 1 && p[i] == '.')) {
                /* nothing */
            } else if (j - i == 2 && p[i] == '.' && p[i + 1] == '.') {
                if (nst > 0) {
                    w = starts[--nst];
                    out[w] = 0;
                }
            } else {
                if (nst >= 127 || w + (j - i) + 2 > cap)
                    return -1;
                starts[nst++] = w;
                out[w++] = '/';
                memcpy(out + w, p + i, (size_t)(j - i));
                w += j - i;
                out[w] = 0;
            }
            i = j + 1;
        }
    }
    if (!w) {
        if (cap < 2)
            return -1;
        out[0] = '/';
        out[1] = 0;
    }
    return 0;
}

static int abs_path(const pm_opts *o, const char *p)
{
    return o->host ? p[0] == '/' : strchr(p, ':') != 0;
}

static int join_paths(const pm_opts *o, const char *base, const char *rel, char *out, long cap)
{
    if (o->host)
        return host_join(base, rel, out, cap);
    return path_join(base, rel, out, cap);
}

int pm_path(const pm_opts *o, const char *input, const char *base, int trim, char *out, long cap)
{
    char tmp[PM_P + 8];
    const char *s = input, *e;
    long n;
    const char *home = o->home && o->home[0] ? o->home : "SYS:";
    e = s + strlen(s);
    if (trim) {
        while (*s && pi_js_space_len((const unsigned char *)s))
            s += pi_js_space_len((const unsigned char *)s);
        while (e > s && pi_js_space_back((const unsigned char *)s, (const unsigned char *)e))
            e -= pi_js_space_back((const unsigned char *)s, (const unsigned char *)e);
    }
    n = (long)(e - s);
    if (n >= (long)sizeof(tmp))
        return -1;
    memcpy(tmp, s, (size_t)n);
    tmp[n] = 0;
    if (!strcmp(tmp, "~"))
        return join_paths(o, "", home, out, cap) ? -1 : 0;
    if (tmp[0] == '~' && tmp[1] == '/')
        return join_paths(o, home, tmp + 2, out, cap) ? -1 : 0;
    if (!strncmp(tmp, "file://", 7)) {
        /* fileURLToPath: the host part (empty or localhost) off, percent escapes decoded */
        const char *q = tmp + 7, *slash = strchr(q, '/');
        char dec[PM_P + 8];
        long k = 0;
        if (!slash)
            return -1;
        for (q = slash; *q && k < (long)sizeof(dec) - 1; q++) {
            if (q[0] == '%' && q[1] && q[2]) {
                char hx[3];
                hx[0] = q[1];
                hx[1] = q[2];
                hx[2] = 0;
                dec[k++] = (char)strtol(hx, 0, 16);
                q += 2;
            } else
                dec[k++] = *q;
        }
        dec[k] = 0;
        return join_paths(o, "", dec, out, cap) ? -1 : 0;
    }
    if (abs_path(o, tmp))
        return join_paths(o, "", tmp, out, cap) ? -1 : 0;
    return join_paths(o, base, tmp, out, cap) ? -1 : 0;
}

void pm_canon(cl_sys *sys, const char *path, char *out, long cap)
{
    if (!sys->canon || sys->canon(sys->u, path, out, cap))
        cl_copy(out, path, cap);
}

/* ---- the accumulator (pi's Map per resource type) ---- */

typedef struct pm_ctx {
    cl_sys *sys;
    const pm_opts *o;
    int type;
    pm_list *acc;
    int oom;
} pm_ctx;

static int pm_add(pm_ctx *c, const char *path, int enabled, int scope, int kind, const char *base, const char *src)
{
    pm_list *l = c->acc;
    pm_res *r;
    int i;
    if (!path[0])
        return 0;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].path, path))
            return 0;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        pm_res *q = (pm_res *)realloc(l->e, sizeof(pm_res) * (size_t)nc);
        if (!q) {
            c->oom = 1;
            return -1;
        }
        l->e = q;
        l->cap = nc;
    }
    r = &l->e[l->n++];
    memset(r, 0, sizeof(*r));
    cl_copy(r->path, path, sizeof(r->path));
    r->enabled = enabled;
    r->scope = scope;
    r->kind = kind;
    cl_copy(r->base, base ? base : "", sizeof(r->base));
    cl_copy(r->src, src ? src : "", sizeof(r->src));
    return 0;
}

void pm_free(pm_list *l)
{
    free(l->e);
    memset(l, 0, sizeof(*l));
}

/* ---- ignore rules (skills.ts and package-manager.ts addIgnoreRules, prefixIgnorePattern) ---- */

static void ig_clear(ig_set *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        free(s->r[i].pat);
    free(s->r);
    memset(s, 0, sizeof(*s));
}

/* the lines of .gitignore, .ignore and .fdignore of dir added to set with the prefix rel ("" at the root) */
static int add_ignore_rules(pm_ctx *c, ig_set *set, const char *dir, const char *rel)
{
    static const char *const names[] = { ".gitignore", ".ignore", ".fdignore", 0 };
    int i;
    for (i = 0; names[i]; i++) {
        char p[PM_P], *b = 0;
        long n = 0, a = 0;
        jw out;
        if (path_join(dir, names[i], p, sizeof(p)) || c->sys->kind(c->sys->u, p) != 1)
            continue;
        if (c->sys->read(c->sys->u, p, PM_FILE_MAX, &b, &n))
            continue;
        jw_init(&out);
        while (a < n) {
            long e = a, ts, te;
            const char *line;
            long ln;
            int neg = 0;
            while (e < n && b[e] != '\n')
                e++;
            line = b + a;
            ln = e - a;
            if (ln && line[ln - 1] == '\r')
                ln--;
            a = e + 1;
            for (ts = 0; ts < ln && pi_js_space_len((const unsigned char *)line + ts); ts += pi_js_space_len((const unsigned char *)line + ts))
                ;
            for (te = ln; te > ts && pi_js_space_back((const unsigned char *)line + ts, (const unsigned char *)line + te);
                 te -= pi_js_space_back((const unsigned char *)line + ts, (const unsigned char *)line + te))
                ;
            if (ts == te)
                continue;
            if (line[ts] == '#' && !(line[ts] == '\\'))
                continue;
            if (ln && line[0] == '!') {
                neg = 1;
                line++;
                ln--;
            } else if (ln > 1 && line[0] == '\\' && line[1] == '!') {
                line++;
                ln--;
            }
            if (ln && line[0] == '/') {
                line++;
                ln--;
            }
            if (neg)
                jw_raw(&out, "!", 1);
            if (rel[0]) {
                jw_rawz(&out, rel);
                jw_raw(&out, "/", 1);
            }
            jw_raw(&out, line, ln);
            jw_raw(&out, "\n", 1);
        }
        CL_FREE(b);
        if (out.oom || (out.n && ig_parse(set, out.p, out.n))) {
            jw_free(&out);
            return -1;
        }
        jw_free(&out);
    }
    return 0;
}

/* ---- directory scans ---- */

typedef struct scan_ent {
    cl_dirent *e;
    int n, cap;
    int oom;
} scan_ent;

static int scan_cb(void *c, const cl_dirent *e)
{
    scan_ent *s = (scan_ent *)c;
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        cl_dirent *q = (cl_dirent *)realloc(s->e, sizeof(cl_dirent) * (size_t)nc);
        if (!q) {
            s->oom = 1;
            return 1;
        }
        s->e = q;
        s->cap = nc;
    }
    s->e[s->n++] = *e;
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const cl_dirent *)a)->name, ((const cl_dirent *)b)->name);
}

/* readdirSync(dir): libuv sorts what scandir gives it, by bytes. NULL on a directory that cannot be read */
static scan_ent *read_dir(pm_ctx *c, const char *dir)
{
    scan_ent *se = (scan_ent *)calloc(1, sizeof(scan_ent));
    if (!se)
        return 0;
    if (c->sys->list(c->sys->u, dir, scan_cb, se) || se->oom) {
        free(se->e);
        free(se);
        return 0;
    }
    qsort(se->e, (size_t)se->n, sizeof(cl_dirent), ent_cmp);
    return se;
}

static void scan_free(scan_ent *se)
{
    if (se) {
        free(se->e);
        free(se);
    }
}

static void rel_join(const char *rel, const char *name, char *out, long cap)
{
    cl_copy(out, rel, cap);
    if (rel[0])
        cl_cat(out, "/", cap);
    cl_cat(out, name, cap);
}

/* collectSkillEntries(dir, mode): the skill files below dir. A directory holding SKILL.md ends the search there; else .md files
 * (mode "pi": those of the root only; "agents": those of the sub-directories only) and the sub-directories. rel is dir relative
 * to the root ("" at the root) */
static int collect_skills(pm_ctx *c, const char *dir, const char *rel, int agents, ig_set *ig, sl *out, int depth)
{
    scan_ent *se;
    int i, rc = 0;
    if (depth > PM_DEPTH)
        return 0;
    if (c->sys->kind(c->sys->u, dir) != 2)
        return 0;
    if (add_ignore_rules(c, ig, dir, rel))
        return -1;
    se = read_dir(c, dir);
    if (!se)
        return 0;
    for (i = 0; i < se->n; i++) {
        char full[PM_P], relp[PM_P];
        if (strcmp(se->e[i].name, "SKILL.md"))
            continue;
        if (path_join(dir, se->e[i].name, full, sizeof(full)))
            continue;
        if (c->sys->kind(c->sys->u, full) != 1)
            continue;
        rel_join(rel, se->e[i].name, relp, sizeof(relp));
        if (ig_match(ig, relp, 0) == 1)
            continue;
        rc = sl_add(out, full);
        scan_free(se);
        return rc;
    }
    for (i = 0; i < se->n && rc == 0; i++) {
        const cl_dirent *e = &se->e[i];
        char full[PM_P], relp[PM_P];
        int kind;
        if (e->name[0] == '.' || !strcmp(e->name, "node_modules"))
            continue;
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        kind = c->sys->kind(c->sys->u, full);
        rel_join(rel, e->name, relp, sizeof(relp));
        if (kind == 1 && ends_md(e->name) && ig_match(ig, relp, 0) != 1 && ((!agents && !rel[0]) || (agents && rel[0]))) {
            rc = sl_add(out, full);
            continue;
        }
        if (kind != 2)
            continue;
        if (ig_match(ig, relp, 1) == 1)
            continue;
        rc = collect_skills(c, full, relp, agents, ig, out, depth + 1);
    }
    scan_free(se);
    return rc;
}

/* collectFiles(dir, /\.md$/): every .md file below dir (not names starting with ".", not node_modules), with the ignore files */
static int collect_md(pm_ctx *c, const char *dir, const char *rel, ig_set *ig, sl *out, int depth)
{
    scan_ent *se;
    int i, rc = 0;
    if (depth > PM_DEPTH)
        return 0;
    if (c->sys->kind(c->sys->u, dir) != 2)
        return 0;
    if (add_ignore_rules(c, ig, dir, rel))
        return -1;
    se = read_dir(c, dir);
    if (!se)
        return 0;
    for (i = 0; i < se->n && rc == 0; i++) {
        const cl_dirent *e = &se->e[i];
        char full[PM_P], relp[PM_P];
        int kind;
        if (e->name[0] == '.' || !strcmp(e->name, "node_modules"))
            continue;
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        kind = c->sys->kind(c->sys->u, full);
        if (kind != 1 && kind != 2)
            continue;
        rel_join(rel, e->name, relp, sizeof(relp));
        if (ig_match(ig, relp, kind == 2) == 1)
            continue;
        if (kind == 2)
            rc = collect_md(c, full, relp, ig, out, depth + 1);
        else if (ends_md(e->name))
            rc = sl_add(out, full);
    }
    scan_free(se);
    return rc;
}

/* collectAutoPromptEntries(dir): the .md files directly in dir, not hidden, not node_modules, not ignored by dir's own ignore files */
static int collect_prompts(pm_ctx *c, const char *dir, sl *out)
{
    ig_set ig;
    scan_ent *se;
    int i, rc = 0;
    if (c->sys->kind(c->sys->u, dir) != 2)
        return 0;
    memset(&ig, 0, sizeof(ig));
    if (add_ignore_rules(c, &ig, dir, "")) {
        ig_clear(&ig);
        return -1;
    }
    se = read_dir(c, dir);
    if (!se) {
        ig_clear(&ig);
        return 0;
    }
    for (i = 0; i < se->n && rc == 0; i++) {
        const cl_dirent *e = &se->e[i];
        char full[PM_P];
        if (e->name[0] == '.' || !strcmp(e->name, "node_modules"))
            continue;
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        if (c->sys->kind(c->sys->u, full) != 1)
            continue;
        if (ig_match(&ig, e->name, 0) == 1)
            continue;
        if (ends_md(e->name))
            rc = sl_add(out, full);
    }
    scan_free(se);
    ig_clear(&ig);
    return rc;
}

/* collectResourceFiles(dir, type) */
static int collect_resource_files(pm_ctx *c, const char *dir, sl *out)
{
    ig_set ig;
    int rc;
    memset(&ig, 0, sizeof(ig));
    rc = c->type == PM_SKILLS ? collect_skills(c, dir, "", 0, &ig, out, 0) : collect_md(c, dir, "", &ig, out, 0);
    ig_clear(&ig);
    return rc;
}

/* collectFilesFromPaths(paths, type): a file as it is, a directory searched */
static int collect_from_path(pm_ctx *c, const char *p, sl *out)
{
    int kind = c->sys->kind(c->sys->u, p);
    if (kind == 1)
        return sl_add(out, p);
    if (kind == 2)
        return collect_resource_files(c, p, out);
    return 0;
}

/* ---- patterns (package-manager.ts matchesAnyPattern ... applyAutoloadDisabledPatterns) ---- */

static int is_override(const char *s)
{
    return s[0] == '!' || s[0] == '+' || s[0] == '-';
}

static int is_pattern(const char *s)
{
    return is_override(s) || strchr(s, '*') || strchr(s, '?');
}

/* path relative to base ("" when it is base): 1 inside, else 0 with rel the path itself */
static int rel_to(const char *base, const char *path, char *rel, long cap)
{
    long bn = (long)strlen(base);
    if (bn && !strncmp(path, base, (size_t)bn) &&
        (path[bn] == '/' || (bn && (base[bn - 1] == '/' || base[bn - 1] == ':')))) {
        const char *r = path + bn + (path[bn] == '/' ? 1 : 0);
        cl_copy(rel, r, cap);
        return 1;
    }
    cl_copy(rel, path, cap);
    return 0;
}

/* package-manager.ts normalizePath (pi 1.2.0, #10765) on an entry of !pattern, +path, -path or a manifest pattern: `~`, `~/x` from the
 * home directory and file:// as a path, anything else (a glob, a relative path) as written */
static const char *norm_pat(const pm_opts *o, const char *p, char *buf, long cap)
{
    if (((p[0] == '~' && (!p[1] || p[1] == '/')) || !strncmp(p, "file://", 7)) && !pm_path(o, p, "", 0, buf, cap))
        return buf;
    return p;
}

/* matchesAnyPattern / matchesAnyExactPattern for one file: the entries of pats (without their first character) */
static int matches_any(const pm_opts *o, const char *file, const char *base, char *const *pats, int n, int exact)
{
    char rel[PM_P], parent[PM_P], prel[PM_P];
    const char *name = base_name(file);
    int is_skill = !strcmp(name, "SKILL.md"), i;
    rel_to(base, file, rel, sizeof(rel));
    parent[0] = prel[0] = 0;
    if (is_skill) {
        path_parent(file, parent, sizeof(parent));
        rel_to(base, parent, prel, sizeof(prel));
    }
    for (i = 0; i < n; i++) {
        char nb[PM_P];
        const char *p = norm_pat(o, pats[i], nb, sizeof(nb));
        if (exact) {
            if (p[0] == '.' && p[1] == '/')
                p += 2;                 /* normalizeExactPattern */
            if (!strcmp(p, rel) || !strcmp(p, file))
                return 1;
            if (is_skill && (!strcmp(p, prel) || !strcmp(p, parent)))
                return 1;
        } else {
            if (glob_match_case(p, rel) || glob_match_case(p, name) || glob_match_case(p, file))
                return 1;
            if (is_skill && (glob_match_case(p, prel) || glob_match_case(p, base_name(parent)) || glob_match_case(p, parent)))
                return 1;
        }
    }
    return 0;
}

/* the entries of pats that start with c (or, for 0, none of !, +, -), their first character cut when cut */
static void pick(char *const *pats, int n, int c, int cut, sl *out)
{
    int i;
    for (i = 0; i < n; i++) {
        int has = pats[i][0] == '!' || pats[i][0] == '+' || pats[i][0] == '-';
        if (c ? pats[i][0] == c : !has)
            sl_add(out, pats[i] + (cut && has ? 1 : 0));
    }
}

/* isEnabledByOverrides(file, entries, base): !x switches off, +x on again, -x off for good; the other entries are not read */
static int enabled_by_overrides(const pm_opts *o, const char *file, const char *base, char *const *entries, int n)
{
    sl ex, in, out;
    int en = 1;
    memset(&ex, 0, sizeof(ex));
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    pick(entries, n, '!', 1, &ex);
    pick(entries, n, '+', 1, &in);
    pick(entries, n, '-', 1, &out);
    if (ex.n && matches_any(o, file, base, ex.v, ex.n, 0))
        en = 0;
    if (in.n && matches_any(o, file, base, in.v, in.n, 1))
        en = 1;
    if (out.n && matches_any(o, file, base, out.v, out.n, 1))
        en = 0;
    sl_free(&ex);
    sl_free(&in);
    sl_free(&out);
    return en;
}

/* applyPatterns for one file: includes (plain entries) must match, !x excludes, +x brings back, -x removes */
static int enabled_by_patterns(const pm_opts *o, const char *file, const char *base, char *const *entries, int n)
{
    sl inc, ex, in, out;
    int en = 1;
    memset(&inc, 0, sizeof(inc));
    memset(&ex, 0, sizeof(ex));
    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    pick(entries, n, 0, 0, &inc);
    pick(entries, n, '!', 1, &ex);
    pick(entries, n, '+', 1, &in);
    pick(entries, n, '-', 1, &out);
    if (inc.n && !matches_any(o, file, base, inc.v, inc.n, 0))
        en = 0;
    if (ex.n && matches_any(o, file, base, ex.v, ex.n, 0))
        en = 0;
    if (in.n && matches_any(o, file, base, in.v, in.n, 1))
        en = 1;
    if (out.n && matches_any(o, file, base, out.v, out.n, 1))
        en = 0;
    sl_free(&inc);
    sl_free(&ex);
    sl_free(&in);
    sl_free(&out);
    return en;
}

/* ---- settings.json ---- */

typedef struct pm_pkg {
    char src[PM_P];
    int autoload;               /* 1 unless "autoload": false */
    int has_filter;             /* an object entry */
    int has[2];                 /* the object names "skills" / "prompts" */
    sl pats[2];
} pm_pkg;

typedef struct pm_set {
    sl entries[2];              /* "skills", "prompts" */
    pm_pkg *pkg;
    int npkg, cappkg;
    int commands;               /* enableSkillCommands: -1 absent */
} pm_set;

static void set_free(pm_set *s)
{
    int i;
    sl_free(&s->entries[0]);
    sl_free(&s->entries[1]);
    for (i = 0; i < s->npkg; i++) {
        sl_free(&s->pkg[i].pats[0]);
        sl_free(&s->pkg[i].pats[1]);
    }
    free(s->pkg);
    memset(s, 0, sizeof(*s));
}

static void json_strings(jv arr, sl *out)
{
    jit it;
    jv x;
    json_iter(arr, &it);
    while (json_next(&it, 0, &x))
        if (json_type(x) == J_STR) {
            char e[PM_P];
            json_str(x, e, sizeof(e));
            sl_add(out, e);
        }
}

static void read_settings(cl_sys *sys, const char *file, pm_set *set)
{
    char *b = 0;
    long n = 0;
    jv o, v, x;
    memset(set, 0, sizeof(*set));
    set->commands = -1;
    if (!file[0] || sys->kind(sys->u, file) != 1 || sys->read(sys->u, file, PM_FILE_MAX, &b, &n))
        return;
    if (!json_parse(b, n, &o) && json_type(o) == J_OBJ) {
        if (json_get(o, "enableSkillCommands", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
            set->commands = json_type(v) == J_TRUE;
        if (json_get(o, "skills", &v) && json_type(v) == J_ARR)
            json_strings(v, &set->entries[0]);
        if (json_get(o, "prompts", &v) && json_type(v) == J_ARR)
            json_strings(v, &set->entries[1]);
        if (json_get(o, "packages", &v) && json_type(v) == J_ARR) {
            jit it;
            json_iter(v, &it);
            while (json_next(&it, 0, &x)) {
                pm_pkg *p;
                if (set->npkg == set->cappkg) {
                    int nc = set->cappkg ? set->cappkg * 2 : 4;
                    pm_pkg *q = (pm_pkg *)realloc(set->pkg, sizeof(pm_pkg) * (size_t)nc);
                    if (!q)
                        break;
                    set->pkg = q;
                    set->cappkg = nc;
                }
                p = &set->pkg[set->npkg];
                memset(p, 0, sizeof(*p));
                p->autoload = 1;
                if (json_type(x) == J_STR)
                    json_str(x, p->src, sizeof(p->src));
                else if (json_type(x) == J_OBJ) {
                    jv sv, fv;
                    if (!json_get(x, "source", &sv) || json_type(sv) != J_STR)
                        continue;
                    json_str(sv, p->src, sizeof(p->src));
                    p->has_filter = 1;
                    if (json_get(x, "autoload", &fv) && json_type(fv) == J_FALSE)
                        p->autoload = 0;
                    if (json_get(x, "skills", &fv) && json_type(fv) == J_ARR) {
                        p->has[0] = 1;
                        json_strings(fv, &p->pats[0]);
                    }
                    if (json_get(x, "prompts", &fv) && json_type(fv) == J_ARR) {
                        p->has[1] = 1;
                        json_strings(fv, &p->pats[1]);
                    }
                } else
                    continue;
                set->npkg++;
            }
        }
    }
    CL_FREE(b);
}

/* ---- the entries of a settings file (resolveLocalEntries) ---- */

static int resolve_local_entries(pm_ctx *c, const sl *entries, int scope, const char *base)
{
    sl files;
    int i, rc = 0;
    memset(&files, 0, sizeof(files));
    if (!entries->n)
        return 0;
    for (i = 0; i < entries->n && rc == 0; i++) {
        char p[PM_P];
        if (is_pattern(entries->v[i]))
            continue;
        if (pm_path(c->o, entries->v[i], base, 1, p, sizeof(p)))
            continue;
        rc = collect_from_path(c, p, &files);
    }
    {
        sl pats;
        memset(&pats, 0, sizeof(pats));
        for (i = 0; i < entries->n; i++)
            if (is_pattern(entries->v[i]))
                sl_add(&pats, entries->v[i]);
        for (i = 0; i < files.n && rc == 0; i++)
            rc = pm_add(c, files.v[i], enabled_by_patterns(c->o, files.v[i], base, pats.v, pats.n) ? 1 : 0, scope, PMK_LOCAL, "", "");
        sl_free(&pats);
    }
    sl_free(&files);
    return rc;
}

/* ---- packages ---- */

typedef struct manifest {
    int present;                /* a package.json with a "pi" object */
    sl e[2];
    int has[2];
} manifest;

static void manifest_free(manifest *m)
{
    sl_free(&m->e[0]);
    sl_free(&m->e[1]);
    memset(m, 0, sizeof(*m));
}

/* readPiManifest(root/package.json) */
static void read_manifest(pm_ctx *c, const char *root, manifest *m)
{
    char p[PM_P], *b = 0;
    long n = 0;
    jv o, pi, v;
    int k;
    static const char *const names[2] = { "skills", "prompts" };
    memset(m, 0, sizeof(*m));
    if (path_join(root, "package.json", p, sizeof(p)) || c->sys->kind(c->sys->u, p) != 1 ||
        c->sys->read(c->sys->u, p, PM_FILE_MAX, &b, &n))
        return;
    {
        const char *t = b;
        long tn = n;
        if (tn >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB && (unsigned char)t[2] == 0xBF) {
            t += 3;
            tn -= 3;
        }
        if (!json_parse(t, tn, &o) && json_type(o) == J_OBJ && json_get(o, "pi", &pi) && json_type(pi) == J_OBJ) {
            m->present = 1;
            for (k = 0; k < 2; k++)
                if (json_get(pi, names[k], &v) && json_type(v) == J_ARR) {
                    jit it;
                    jv x;
                    int all = 1;
                    json_iter(v, &it);
                    while (json_next(&it, 0, &x))
                        if (json_type(x) != J_STR)
                            all = 0;
                    if (all) {
                        m->has[k] = 1;
                        json_strings(v, &m->e[k]);
                    }
                }
        }
    }
    CL_FREE(b);
}

/* globSync(pattern, {cwd: root}) for a manifest entry: the paths below root that match, no name with a leading dot, sorted */
static int glob_walk(pm_ctx *c, const char *root, const char *rel, const char *pat, sl *out, int depth, long *budget)
{
    scan_ent *se;
    char dir[PM_P];
    int i, rc = 0;
    if (depth > PM_DEPTH)
        return 0;
    if (rel[0]) {
        if (path_join(root, rel, dir, sizeof(dir)))
            return 0;
    } else
        cl_copy(dir, root, sizeof(dir));
    se = read_dir(c, dir);
    if (!se)
        return 0;
    for (i = 0; i < se->n && rc == 0; i++) {
        const cl_dirent *e = &se->e[i];
        char relp[PM_P], full[PM_P];
        int kind;
        if (e->name[0] == '.' || --(*budget) < 0)
            continue;
        rel_join(rel, e->name, relp, sizeof(relp));
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        kind = c->sys->kind(c->sys->u, full);
        if (glob_match_case(pat, relp))
            rc = sl_add(out, full);
        if (kind == 2 && rc == 0)
            rc = glob_walk(c, root, relp, pat, out, depth + 1, budget);
    }
    scan_free(se);
    return rc;
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* collectFilesFromManifestEntries(entries, root): the non-pattern entries (a path, or a glob expanded) searched for resources */
static int manifest_files(pm_ctx *c, const sl *entries, const char *root, sl *files)
{
    sl paths;
    int i, rc = 0;
    memset(&paths, 0, sizeof(paths));
    for (i = 0; i < entries->n && rc == 0; i++) {
        const char *e = entries->v[i];
        if (is_override(e))
            continue;
        if (!strchr(e, '*') && !strchr(e, '?')) {
            char p[PM_P];
            /* path.resolve(root, entry): "~" is not special here */
            if (!join_paths(c->o, root, e, p, sizeof(p)))
                rc = sl_add(&paths, p);
        } else {
            sl found;
            long budget = 20000;
            memset(&found, 0, sizeof(found));
            rc = glob_walk(c, root, "", e, &found, 0, &budget);
            if (found.n > 1)
                qsort(found.v, (size_t)found.n, sizeof(char *), str_cmp);
            {
                int j;
                for (j = 0; j < found.n && rc == 0; j++)
                    rc = sl_add(&paths, found.v[j]);
            }
            sl_free(&found);
        }
    }
    for (i = 0; i < paths.n && rc == 0; i++)
        rc = collect_from_path(c, paths.v[i], files);
    sl_free(&paths);
    return rc;
}

/* the files that pass a list of patterns (applyPatterns), in the order of files */
static int apply_patterns(const pm_opts *o, const sl *files, const sl *pats, const char *base, sl *out)
{
    int i, rc = 0;
    for (i = 0; i < files->n && rc == 0; i++)
        if (enabled_by_patterns(o, files->v[i], base, pats->v, pats->n))
            rc = sl_add(out, files->v[i]);
    return rc;
}

/* collectManifestFiles(root, type).allFiles: the manifest's files that pass the manifest's own patterns, else the convention
 * directory's */
static int manifest_all_files(pm_ctx *c, const char *root, const manifest *m, sl *all)
{
    int t = c->type, rc = 0;
    if (m->present && m->has[t] && m->e[t].n > 0) {
        sl files, pats;
        int i;
        memset(&files, 0, sizeof(files));
        memset(&pats, 0, sizeof(pats));
        rc = manifest_files(c, &m->e[t], root, &files);
        for (i = 0; i < m->e[t].n; i++)
            if (is_override(m->e[t].v[i]))
                sl_add(&pats, m->e[t].v[i]);
        if (rc == 0) {
            if (pats.n)
                rc = apply_patterns(c->o, &files, &pats, root, all);
            else
                for (i = 0; i < files.n && rc == 0; i++)
                    rc = sl_add(all, files.v[i]);
        }
        sl_free(&files);
        sl_free(&pats);
        return rc;
    }
    {
        char dir[PM_P];
        if (!path_join(root, c->type == PM_SKILLS ? "skills" : "prompts", dir, sizeof(dir)) && c->sys->kind(c->sys->u, dir) > 0)
            rc = collect_resource_files(c, dir, all);
    }
    return rc;
}

/* addManifestEntries / collectDefaultResources for the type of c: the manifest's, else the convention directory's */
static int add_default(pm_ctx *c, const char *root, const manifest *m, int scope, const char *src, int convention)
{
    int t = c->type, rc = 0, i;
    if (m->present && m->has[t]) {
        sl files, pats, enabled;
        memset(&files, 0, sizeof(files));
        memset(&pats, 0, sizeof(pats));
        memset(&enabled, 0, sizeof(enabled));
        rc = manifest_files(c, &m->e[t], root, &files);
        for (i = 0; i < m->e[t].n; i++)
            if (is_override(m->e[t].v[i]))
                sl_add(&pats, m->e[t].v[i]);
        if (rc == 0)
            rc = apply_patterns(c->o, &files, &pats, root, &enabled);
        for (i = 0; i < files.n && rc == 0; i++) {
            int j, on = 0;
            for (j = 0; j < enabled.n; j++)
                if (!strcmp(enabled.v[j], files.v[i]))
                    on = 1;
            if (on)
                rc = pm_add(c, files.v[i], 1, scope, PMK_PACKAGE, root, src);
        }
        sl_free(&files);
        sl_free(&pats);
        sl_free(&enabled);
        return rc;
    }
    if (convention) {
        char dir[PM_P];
        sl files;
        memset(&files, 0, sizeof(files));
        if (!path_join(root, t == PM_SKILLS ? "skills" : "prompts", dir, sizeof(dir)) && c->sys->kind(c->sys->u, dir) > 0)
            rc = collect_resource_files(c, dir, &files);
        for (i = 0; i < files.n && rc == 0; i++)
            rc = pm_add(c, files.v[i], 1, scope, PMK_PACKAGE, root, src);
        sl_free(&files);
    }
    return rc;
}

/* applyAutoloadDisabledPatterns(allFiles, patterns, root) then addResource for each: a Map keyed by file, in the order the
 * files were first set; the last pattern that matches decides */
static void delta_filter(pm_ctx *c, const sl *all, const sl *pats, const char *root, int scope, const char *src)
{
    int i, k, norder = 0;
    int *val = (int *)calloc((size_t)(all->n ? all->n : 1), sizeof(int));
    int *order = (int *)calloc((size_t)(all->n ? all->n : 1), sizeof(int));
    if (!val || !order) {
        free(val);
        free(order);
        return;
    }
    for (k = 0; k < pats->n; k++) {
        const char *pat = pats->v[k];
        int exact = pat[0] == '+' || pat[0] == '-', en = !(pat[0] == '-' || pat[0] == '!');
        char *arr[1];
        arr[0] = (char *)(pat + (is_override(pat) ? 1 : 0));
        for (i = 0; i < all->n; i++)
            if (matches_any(c->o, all->v[i], root, arr, 1, exact)) {
                if (!val[i])
                    order[norder++] = i;
                val[i] = en ? 1 : 2;      /* 1 on, 2 off */
            }
    }
    for (i = 0; i < norder; i++)
        pm_add(c, all->v[order[i]], val[order[i]] == 1, scope, PMK_PACKAGE, root, src);
    free(val);
    free(order);
}

static int pkg_resources(pm_ctx *c, const char *root, const pm_pkg *filter, int scope, const char *src)
{
    manifest m;
    int t = c->type, rc = 0;
    read_manifest(c, root, &m);
    if (filter && filter->has_filter) {
        const sl *pats = &filter->pats[t];
        if (!filter->autoload) {
            if (pats->n || filter->has[t]) {
                if (pats->n) {
                    sl all;
                    memset(&all, 0, sizeof(all));
                    rc = manifest_all_files(c, root, &m, &all);
                    if (rc == 0)
                        delta_filter(c, &all, pats, root, scope, src);
                    sl_free(&all);
                }
            }
        } else if (filter->has[t]) {
            sl all, on;
            int i;
            memset(&all, 0, sizeof(all));
            memset(&on, 0, sizeof(on));
            rc = manifest_all_files(c, root, &m, &all);
            if (rc == 0 && pats->n)
                rc = apply_patterns(c->o, &all, pats, root, &on);
            for (i = 0; i < all.n && rc == 0; i++) {
                int j, en = 0;
                for (j = 0; j < on.n; j++)
                    if (!strcmp(on.v[j], all.v[i]))
                        en = 1;
                rc = pm_add(c, all.v[i], pats->n ? en : 0, scope, PMK_PACKAGE, root, src);
            }
            sl_free(&all);
            sl_free(&on);
        } else
            rc = add_default(c, root, &m, scope, src, 1);
        manifest_free(&m);
        return rc;
    }
    if (m.present)
        rc = add_default(c, root, &m, scope, src, 0);
    else {
        char dir[PM_P];
        sl files;
        int i;
        memset(&files, 0, sizeof(files));
        if (!path_join(root, t == PM_SKILLS ? "skills" : "prompts", dir, sizeof(dir)) && c->sys->kind(c->sys->u, dir) > 0)
            rc = collect_resource_files(c, dir, &files);
        for (i = 0; i < files.n && rc == 0; i++)
            rc = pm_add(c, files.v[i], 1, scope, PMK_PACKAGE, root, src);
        sl_free(&files);
    }
    manifest_free(&m);
    return rc;
}

/* a package source that is not a local path: npm:, git:, github:, http(s):, ssh: (and builtin:, which is for extensions) */
static int non_local(const char *s)
{
    static const char *const pre[] = { "npm:", "git:", "github:", "http:", "https:", "ssh:", "builtin:", 0 };
    int i;
    while (*s == ' ' || *s == '\t')
        s++;
    for (i = 0; pre[i]; i++)
        if (!strncmp(s, pre[i], strlen(pre[i])))
            return 1;
    return 0;
}

typedef struct pm_entry {
    const pm_pkg *pkg;
    int scope;
} pm_entry;

static int pkg_identity(pm_ctx *c, const pm_pkg *p, int scope, const char *proj_base, const char *agent, char *out, long cap)
{
    char path[PM_P];
    if (non_local(p->src)) {
        cl_copy(out, "x:", cap);
        cl_cat(out, p->src, cap);
        return 0;
    }
    if (pm_path(c->o, p->src, scope == PMS_PROJECT ? proj_base : agent, 1, path, sizeof(path)))
        path[0] = 0;
    cl_copy(out, "local:", cap);
    cl_cat(out, path, cap);
    return 0;
}

/* resolvePackageSources + dedupePackages for the local ones of both settings files */
static int resolve_packages(pm_ctx *c, const pm_set *pset, const pm_set *gset, const char *proj_base, const char *agent)
{
    pm_entry *all = 0;
    char (*ids)[PM_P + 8] = 0;
    int n = 0, i, j, rc = 0, *keep = 0, nkeep = 0;
    int total = (pset ? pset->npkg : 0) + (gset ? gset->npkg : 0);
    if (!total)
        return 0;
    all = (pm_entry *)calloc((size_t)total, sizeof(pm_entry));
    ids = (char (*)[PM_P + 8])calloc((size_t)total, PM_P + 8);
    keep = (int *)calloc((size_t)total * 2, sizeof(int));
    if (!all || !ids || !keep) {
        free(all);
        free(ids);
        free(keep);
        return -1;
    }
    for (i = 0; pset && i < pset->npkg; i++) {
        all[n].pkg = &pset->pkg[i];
        all[n].scope = PMS_PROJECT;
        n++;
    }
    for (i = 0; gset && i < gset->npkg; i++) {
        all[n].pkg = &gset->pkg[i];
        all[n].scope = PMS_USER;
        n++;
    }
    for (i = 0; i < n; i++)
        pkg_identity(c, all[i].pkg, all[i].scope, proj_base, agent, ids[i], PM_P + 8);
    /* dedupePackages: a project entry wins over a user entry of one identity, unless it is an autoload:false delta (both stay) */
    {
        int *res = keep, nres = 0;
        for (i = 0; i < n; i++) {
            int found = -1;
            for (j = 0; j < nres; j++)
                if (!strcmp(ids[res[j]], ids[i])) {
                    found = j;
                    break;
                }
            if (found < 0) {
                res[nres++] = i;
                continue;
            }
            if (all[res[found]].scope == PMS_PROJECT && all[i].scope == PMS_USER) {
                if (!all[res[found]].pkg->autoload)
                    res[nres++] = i;
            } else if (all[i].scope == PMS_PROJECT)
                res[found] = i;
        }
        nkeep = nres;
    }
    for (i = 0; i < nkeep && rc == 0; i++) {
        const pm_entry *en = &all[keep[i]];
        const pm_pkg *pkg = en->pkg;
        int scope = en->scope, base_scope = scope;
        const pm_pkg *delta_user = 0;
        char root[PM_P], *srcp = (char *)pkg->src;
        const char *basedir;
        if (non_local(pkg->src))
            continue;               /* npm and git packages are fetched by pi; not here */
        /* findAutoloadDeltaBase: a project entry with autoload false builds on the user entry of the same package */
        if (scope == PMS_PROJECT && pkg->has_filter && !pkg->autoload) {
            for (j = 0; j < n; j++)
                if (all[j].scope == PMS_USER && !strcmp(ids[j], ids[keep[i]]) && keep[i] != j) {
                    delta_user = all[j].pkg;
                    break;
                }
        }
        if (delta_user)
            base_scope = PMS_USER;
        basedir = base_scope == PMS_PROJECT ? proj_base : agent;
        if (pm_path(c->o, delta_user ? delta_user->src : pkg->src, basedir, 1, root, sizeof(root)))
            continue;
        if (c->sys->kind(c->sys->u, root) == 2)
            rc = pkg_resources(c, root, pkg, scope, srcp);
        /* a file as the package: it brings extensions only */
    }
    free(all);
    free(ids);
    free(keep);
    return rc;
}

/* ---- the whole resolve ---- */

static int rank_of(const pm_res *r)
{
    if (r->kind == PMK_PACKAGE)
        return 4;
    return (r->scope == PMS_PROJECT ? 0 : 2) + (r->kind == PMK_LOCAL ? 0 : 1);
}

/* pi's findGitRepoRoot + collectAncestorAgentsSkillDirs */
static int ancestor_agents(pm_ctx *c, const char *cwd, sl *dirs)
{
    char top[PM_P], cur[PM_P], up[PM_P], g[PM_P];
    int repo = 0, rc = 0;
    cl_copy(cur, cwd, sizeof(cur));
    for (;;) {
        if (path_join(cur, ".git", g, sizeof(g)) == 0 && c->sys->kind(c->sys->u, g) > 0) {
            cl_copy(top, cur, sizeof(top));
            repo = 1;
            break;
        }
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            break;
        cl_copy(cur, up, sizeof(cur));
    }
    cl_copy(cur, cwd, sizeof(cur));
    for (;;) {
        char d[PM_P];
        if (path_join(cur, ".agents/skills", d, sizeof(d)) == 0 && dirs->n < 32)
            rc = sl_add(dirs, d);
        if (rc || (repo && !strcmp(cur, top)))
            break;
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            break;
        cl_copy(cur, up, sizeof(cur));
    }
    return rc;
}

/* addResources for one auto directory: each file with its enabled flag from the overrides of settings */
static int add_autos(pm_ctx *c, const sl *files, const pm_set *set, int type_ix, int scope, const char *base)
{
    int i, rc = 0;
    for (i = 0; i < files->n && rc == 0; i++)
        rc = pm_add(c, files->v[i], enabled_by_overrides(c->o, files->v[i], base, set->entries[type_ix].v, set->entries[type_ix].n),
                    scope, PMK_AUTO, base, "");
    return rc;
}

int pm_resolve(cl_sys *sys, const pm_opts *o, int rtype, pm_list *out, int *commands)
{
    pm_ctx c;
    pm_set gset, pset;
    pm_list acc;
    char gfile[PM_P], pfile[PM_P], proj_base[PM_P], d[PM_P];
    int rc = 0, ix = rtype == PM_SKILLS ? 0 : 1, i;
    memset(&c, 0, sizeof(c));
    memset(&acc, 0, sizeof(acc));
    memset(out, 0, sizeof(*out));
    c.sys = sys;
    c.o = o;
    c.type = rtype;
    c.acc = &acc;
    gfile[0] = pfile[0] = proj_base[0] = 0;
    if (o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "settings.json", gfile, sizeof(gfile)))
        gfile[0] = 0;
    if (o->trusted && o->cwd && path_join(o->cwd, ".pi", proj_base, sizeof(proj_base)) == 0 &&
        path_join(proj_base, "settings.json", pfile, sizeof(pfile)))
        pfile[0] = 0;
    read_settings(sys, gfile, &gset);
    if (o->trusted)
        read_settings(sys, pfile, &pset);
    else {
        memset(&pset, 0, sizeof(pset));
        pset.commands = -1;
    }
    if (commands) {
        *commands = pset.commands >= 0 ? pset.commands : gset.commands;
    }
    /* packages first (pi resolves them before the entries; their rank puts them last whatever the order) */
    if (o->trusted && proj_base[0])
        rc = resolve_packages(&c, &pset, &gset, proj_base, o->agent_dir ? o->agent_dir : "");
    else
        rc = resolve_packages(&c, 0, &gset, "", o->agent_dir ? o->agent_dir : "");
    /* the entries: project, then user */
    if (rc == 0 && o->trusted && proj_base[0])
        rc = resolve_local_entries(&c, &pset.entries[ix], PMS_PROJECT, proj_base);
    if (rc == 0 && o->agent_dir && o->agent_dir[0])
        rc = resolve_local_entries(&c, &gset.entries[ix], PMS_USER, o->agent_dir);
    /* addAutoDiscoveredResources */
    if (rtype == PM_SKILLS) {
        sl files, dirs;
        memset(&files, 0, sizeof(files));
        memset(&dirs, 0, sizeof(dirs));
        if (rc == 0 && o->trusted && proj_base[0] && path_join(proj_base, "skills", d, sizeof(d)) == 0) {
            ig_set ig;
            memset(&ig, 0, sizeof(ig));
            rc = collect_skills(&c, d, "", 0, &ig, &files, 0);
            ig_clear(&ig);
            if (rc == 0)
                rc = add_autos(&c, &files, &pset, ix, PMS_PROJECT, proj_base);
            sl_free(&files);
        }
        if (rc == 0 && o->trusted && o->cwd)
            rc = ancestor_agents(&c, o->cwd, &dirs);
        for (i = 0; i < dirs.n && rc == 0; i++) {
            ig_set ig;
            char agents_base[PM_P];
            memset(&ig, 0, sizeof(ig));
            path_parent(dirs.v[i], agents_base, sizeof(agents_base));
            rc = collect_skills(&c, dirs.v[i], "", 1, &ig, &files, 0);
            ig_clear(&ig);
            if (rc == 0)
                rc = add_autos(&c, &files, &pset, ix, PMS_PROJECT, agents_base);
            sl_free(&files);
        }
        sl_free(&dirs);
        if (rc == 0 && o->agent_dir && o->agent_dir[0]) {
            ig_set ig;
            memset(&ig, 0, sizeof(ig));
            if (path_join(o->agent_dir, "skills", d, sizeof(d)) == 0) {
                rc = collect_skills(&c, d, "", 0, &ig, &files, 0);
                ig_clear(&ig);
                if (rc == 0)
                    rc = add_autos(&c, &files, &gset, ix, PMS_USER, o->agent_dir);
                sl_free(&files);
            }
            memset(&ig, 0, sizeof(ig));
            if (rc == 0 && path_join(o->agent_dir, "agents-skills", d, sizeof(d)) == 0) {
                rc = collect_skills(&c, d, "", 1, &ig, &files, 0);
                ig_clear(&ig);
                if (rc == 0)
                    rc = add_autos(&c, &files, &gset, ix, PMS_USER, o->agent_dir);
                sl_free(&files);
            }
        }
        /* D10: bulky data lives in the data directory when the kit is there: its skills count as the user's, after the rest */
        if (rc == 0 && o->data_dir && o->data_dir[0] && !(o->agent_dir && !strcmp(o->agent_dir, o->data_dir)) &&
            path_join(o->data_dir, "skills", d, sizeof(d)) == 0) {
            ig_set ig;
            memset(&ig, 0, sizeof(ig));
            rc = collect_skills(&c, d, "", 0, &ig, &files, 0);
            ig_clear(&ig);
            if (rc == 0)
                rc = add_autos(&c, &files, &gset, ix, PMS_USER, o->data_dir);
            sl_free(&files);
        }
    } else {
        sl files;
        memset(&files, 0, sizeof(files));
        if (rc == 0 && o->trusted && proj_base[0] && path_join(proj_base, "prompts", d, sizeof(d)) == 0) {
            rc = collect_prompts(&c, d, &files);
            if (rc == 0)
                rc = add_autos(&c, &files, &pset, ix, PMS_PROJECT, proj_base);
            sl_free(&files);
        }
        if (rc == 0 && o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "prompts", d, sizeof(d)) == 0) {
            rc = collect_prompts(&c, d, &files);
            if (rc == 0)
                rc = add_autos(&c, &files, &gset, ix, PMS_USER, o->agent_dir);
            sl_free(&files);
        }
    }
    set_free(&gset);
    set_free(&pset);
    if (rc || c.oom) {
        pm_free(&acc);
        return -1;
    }
    /* toResolvedPaths: by precedence rank (stable), a file once by its canonical name */
    {
        int r;
        char (*seen)[PM_P] = (char (*)[PM_P])calloc((size_t)(acc.n ? acc.n : 1), PM_P);
        int nseen = 0;
        pm_res *res = (pm_res *)calloc((size_t)(acc.n ? acc.n : 1), sizeof(pm_res));
        int nres = 0;
        if (!seen || !res) {
            free(seen);
            free(res);
            pm_free(&acc);
            return -1;
        }
        for (r = 0; r <= 4; r++)
            for (i = 0; i < acc.n; i++) {
                char canon[PM_P];
                int j, dup = 0;
                if (rank_of(&acc.e[i]) != r)
                    continue;
                pm_canon(sys, acc.e[i].path, canon, sizeof(canon));
                for (j = 0; j < nseen; j++)
                    if (!strcmp(seen[j], canon)) {
                        dup = 1;
                        break;
                    }
                if (dup)
                    continue;
                cl_copy(seen[nseen++], canon, PM_P);
                res[nres++] = acc.e[i];
            }
        free(seen);
        pm_free(&acc);
        out->e = res;
        out->n = nres;
        out->cap = nres;
    }
    return 0;
}

void pm_commands(cl_sys *sys, const pm_opts *o, int *commands)
{
    char gfile[PM_P], pfile[PM_P], proj_base[PM_P];
    pm_set gset, pset;
    gfile[0] = pfile[0] = proj_base[0] = 0;
    if (o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "settings.json", gfile, sizeof(gfile)))
        gfile[0] = 0;
    if (o->trusted && o->cwd && path_join(o->cwd, ".pi", proj_base, sizeof(proj_base)) == 0 &&
        path_join(proj_base, "settings.json", pfile, sizeof(pfile)))
        pfile[0] = 0;
    read_settings(sys, gfile, &gset);
    if (o->trusted)
        read_settings(sys, pfile, &pset);
    else {
        memset(&pset, 0, sizeof(pset));
        pset.commands = -1;
    }
    *commands = pset.commands >= 0 ? pset.commands : gset.commands;
    set_free(&gset);
    set_free(&pset);
}

int pm_skill_dir(cl_sys *sys, const char *dir, pm_path_fn fn, void *u)
{
    pm_ctx c;
    pm_opts o;
    ig_set ig;
    sl files;
    int i, rc;
    memset(&c, 0, sizeof(c));
    memset(&o, 0, sizeof(o));
    memset(&ig, 0, sizeof(ig));
    memset(&files, 0, sizeof(files));
    c.sys = sys;
    c.o = &o;
    c.type = PM_SKILLS;
    rc = collect_skills(&c, dir, "", 0, &ig, &files, 0);
    ig_clear(&ig);
    for (i = 0; i < files.n && rc == 0; i++)
        fn(u, files.v[i]);
    sl_free(&files);
    return rc;
}
