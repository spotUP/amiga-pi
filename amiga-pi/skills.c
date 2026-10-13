/* see skills.h */
#include <stdlib.h>
#include <string.h>
#include "skills.h"
#include "frontmatter.h"
#include "tools_pi.h"
#include "claude/path.h"
#include "claude/util.h"
#include "claude/glob.h"
#include "claude/ignore.h"

#define SK_MAX_NAME 64          /* pi: MAX_NAME_LENGTH */
#define SK_MAX_DESC 1024        /* pi: MAX_DESCRIPTION_LENGTH */
#define SK_FILE_MAX (256L * 1024)
#define SK_DEPTH 16

long pi_skills_reads;

/* ---- small things ---- */

static char *dupn(const char *s, long n)
{
    char *p = (char *)malloc((size_t)n + 1);
    if (p) {
        memcpy(p, s, (size_t)n);
        p[n] = 0;
    }
    return p;
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* JavaScript string length (UTF-16 code units) of UTF-8 text */
static long js_len(const char *s)
{
    long n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c & 0xC0) == 0x80)
            continue;           /* a continuation byte */
        n += c >= 0xF0 ? 2 : 1;
    }
    return n;
}

/* ---- the skill files ---- */

typedef struct fl_ent {
    char path[300];
    int source, scope;
    int bad;                    /* a --skill path: 1 does not exist, 2 is not a markdown file (reported in its turn) */
    char sbase[300];            /* the source's baseDir ("" none) */
} fl_ent;

typedef struct fl_list {
    fl_ent *e;
    int n, cap;
} fl_list;

static void fl_free(fl_list *l)
{
    free(l->e);
    memset(l, 0, sizeof(*l));
}

/* a file once (pi's Map keyed by path; the first one's source stays): 0, -1 */
static int fl_add_base(fl_list *l, const char *path, int source, int scope, int bad, const char *sbase)
{
    int i;
    for (i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].path, path))
            return 0;
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        fl_ent *q = (fl_ent *)realloc(l->e, (size_t)nc * sizeof(fl_ent));
        if (!q)
            return -1;
        l->e = q;
        l->cap = nc;
    }
    cl_copy(l->e[l->n].path, path, sizeof(l->e[0].path));
    l->e[l->n].source = source;
    l->e[l->n].scope = scope;
    l->e[l->n].bad = bad;
    cl_copy(l->e[l->n].sbase, sbase, sizeof(l->e[0].sbase));
    l->n++;
    return 0;
}

static int fl_add(fl_list *l, const char *path, int source, int scope)
{
    return fl_add_base(l, path, source, scope, 0, "");
}

typedef struct sk_ctx {
    cl_sys *sys;
    pi_skills *k;
    int oom;
    int cur_source, cur_scope;  /* the source of the files being collected */
    const char *cur_base;       /* ... and its baseDir */
} sk_ctx;

static int add_diag(pi_skills *k, int type, const char *msg, const char *path, const char *winner, const char *name)
{
    pi_skill_diag *d;
    if (k->nd == k->capd) {
        int nc = k->capd ? k->capd * 2 : 16;
        pi_skill_diag *q = (pi_skill_diag *)realloc(k->d, (size_t)nc * sizeof(pi_skill_diag));
        if (!q)
            return -1;
        k->d = q;
        k->capd = nc;
    }
    d = &k->d[k->nd];
    memset(d, 0, sizeof(*d));
    d->type = type;
    d->msg = dupn(msg, (long)strlen(msg));
    d->name = name ? dupn(name, (long)strlen(name)) : 0;
    cl_copy(d->path, path ? path : "", sizeof(d->path));
    cl_copy(d->winner, winner ? winner : "", sizeof(d->winner));
    if (!d->msg || (name && !d->name))
        return -1;
    k->nd++;
    return 0;
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

/* ---- directory scans (pi: collectSkillEntries, loadSkillsFromDirInternal) with the ignore files of skills.ts ---- */

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

/* pi's prefixIgnorePattern for each line of an ignore file below the root, added to set */
static int add_ignore_rules(sk_ctx *c, ig_set *set, const char *dir, const char *rel)
{
    static const char *const names[] = { ".gitignore", ".ignore", ".fdignore", 0 };
    int i;
    for (i = 0; names[i]; i++) {
        char p[300], *b = 0;
        long n = 0, a = 0;
        jw out;
        if (path_join(dir, names[i], p, sizeof(p)) || c->sys->kind(c->sys->u, p) != 1)
            continue;
        if (c->sys->read(c->sys->u, p, SK_FILE_MAX, &b, &n))
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
            for (ts = 0; ts < ln && is_ws(line[ts]); ts++)
                ;
            for (te = ln; te > ts && is_ws(line[te - 1]); te--)
                ;
            if (ts == te)
                continue;
            if (line[ts] == '#')
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

static void ig_clear(ig_set *s)
{
    int i;
    for (i = 0; i < s->n; i++)
        free(s->r[i].pat);
    free(s->r);
    memset(s, 0, sizeof(*s));
}

/* the skill files below dir: SKILL.md of a directory ends the search there; else .md files (md_root: those of the root
 * only -- "pi" style; else those of the sub-directories only -- "agents" style) and the sub-directories. rel is dir
 * relative to the root ("" at the root) */
static int scan_dir(sk_ctx *c, const char *dir, const char *rel, int agents, ig_set *ig, fl_list *out, int depth)
{
    scan_ent *se;
    int i, j, rc = 0;
    if (depth > SK_DEPTH)
        return 0;
    if (c->sys->kind(c->sys->u, dir) != 2)
        return 0;
    if (add_ignore_rules(c, ig, dir, rel))
        return -1;
    se = (scan_ent *)calloc(1, sizeof(scan_ent));
    if (!se)
        return -1;
    if (c->sys->list(c->sys->u, dir, scan_cb, se) || se->oom) {
        int oom = se->oom;
        free(se->e);
        free(se);
        return oom ? -1 : 0;
    }
    qsort(se->e, (size_t)se->n, sizeof(cl_dirent), ent_cmp);
    for (i = 0; i < se->n; i++) {
        char full[300], relp[300];
        if (strcmp(se->e[i].name, "SKILL.md"))
            continue;
        if (path_join(dir, se->e[i].name, full, sizeof(full)))
            continue;
        if (c->sys->kind(c->sys->u, full) != 1)
            continue;
        cl_copy(relp, rel, sizeof(relp));
        if (rel[0])
            cl_cat(relp, "/", sizeof(relp));
        cl_cat(relp, se->e[i].name, sizeof(relp));
        if (ig_match(ig, relp, 0) == 1)
            continue;
        rc = fl_add_base(out, full, c->cur_source, c->cur_scope, 0, c->cur_base ? c->cur_base : "");
        free(se->e);
        free(se);
        return rc;
    }
    for (j = 0; j < se->n && rc == 0; j++) {
        const cl_dirent *e = &se->e[j];
        char full[300], relp[300];
        int kind;
        if (e->name[0] == '.' || !strcmp(e->name, "node_modules"))
            continue;
        if (path_join(dir, e->name, full, sizeof(full)))
            continue;
        kind = c->sys->kind(c->sys->u, full);
        cl_copy(relp, rel, sizeof(relp));
        if (rel[0])
            cl_cat(relp, "/", sizeof(relp));
        cl_cat(relp, e->name, sizeof(relp));
        if (kind == 1 && ends_md(e->name) && ig_match(ig, relp, 0) != 1 && ((!agents && !rel[0]) || (agents && rel[0]))) {
            rc = fl_add_base(out, full, c->cur_source, c->cur_scope, 0, c->cur_base ? c->cur_base : "");
            continue;
        }
        if (kind != 2)
            continue;
        if (ig_match(ig, relp, 1) == 1)
            continue;
        rc = scan_dir(c, full, relp, agents, ig, out, depth + 1);
    }
    free(se->e);
    free(se);
    return rc;
}

/* collectSkillEntries(dir, mode) into out */
static int collect_dir(sk_ctx *c, const char *dir, int agents, fl_list *out)
{
    ig_set ig;
    int rc;
    memset(&ig, 0, sizeof(ig));
    rc = scan_dir(c, dir, "", agents, &ig, out, 0);
    ig_clear(&ig);
    return rc;
}

/* ---- settings.json "skills" (pi: resolveLocalEntries, applyPatterns, isEnabledByOverrides) ---- */

#define SK_PATS 24

typedef struct sk_set {
    char plain[SK_PATS][300];
    int nplain;
    char pat[SK_PATS][300];     /* patterns: "!x", "+x", "-x", or an include glob */
    int npat;
} sk_set;

/* settings.json: the "skills" entries into set (plain paths and patterns), enableSkillCommands into *commands (-1 absent) */
static void read_settings(cl_sys *sys, const char *file, sk_set *set, int *commands)
{
    char *b = 0;
    long n = 0;
    jv o, v, x;
    jit it;
    set->nplain = set->npat = 0;
    *commands = -1;
    if (!file[0] || sys->kind(sys->u, file) != 1 || sys->read(sys->u, file, SK_FILE_MAX, &b, &n))
        return;
    if (!json_parse(b, n, &o) && json_type(o) == J_OBJ) {
        if (json_get(o, "enableSkillCommands", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
            *commands = json_type(v) == J_TRUE;
        if (json_get(o, "skills", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &x))
                if (json_type(x) == J_STR) {
                    char e[300];
                    json_str(x, e, sizeof(e));
                    if (e[0] == '!' || e[0] == '+' || e[0] == '-' || strchr(e, '*') || strchr(e, '?')) {
                        if (set->npat < SK_PATS)
                            cl_copy(set->pat[set->npat++], e, 300);
                    } else if (set->nplain < SK_PATS)
                        cl_copy(set->plain[set->nplain++], e, 300);
                }
        }
    }
    CL_FREE(b);
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

static int norm_exact(const char *pat, char *out, long cap)
{
    if (pat[0] == '.' && pat[1] == '/')
        pat += 2;
    cl_copy(out, pat, cap);
    return 0;
}

/* pi's matchesAnyPattern / matchesAnyExactPattern for one file */
static int matches(const char *file, const char *base, char (*pats)[300], const int *idx, int n, int exact)
{
    char rel[300], parent[300], prel[300];
    const char *name = base_name(file);
    int is_skill = !strcmp(name, "SKILL.md"), i;
    rel_to(base, file, rel, sizeof(rel));
    parent[0] = prel[0] = 0;
    if (is_skill) {
        path_parent(file, parent, sizeof(parent));
        rel_to(base, parent, prel, sizeof(prel));
    }
    for (i = 0; i < n; i++) {
        const char *p = pats[idx[i]] + (pats[idx[i]][0] == '!' || pats[idx[i]][0] == '+' || pats[idx[i]][0] == '-' ? 1 : 0);
        char np[300];
        if (exact) {
            norm_exact(p, np, sizeof(np));
            if (!strcmp(np, rel) || !strcmp(np, file))
                return 1;
            if (is_skill && (!strcmp(np, prel) || !strcmp(np, parent)))
                return 1;
        } else {
            if (glob_match(p, rel) || glob_match(p, name) || glob_match(p, file))
                return 1;
            if (is_skill && (glob_match(p, prel) || glob_match(p, base_name(parent)) || glob_match(p, parent)))
                return 1;
        }
    }
    return 0;
}

/* isEnabledByOverrides: !x excludes, +x forces in, -x forces out */
static int enabled_by_overrides(const char *file, const char *base, char (*pats)[300], int npat)
{
    int idx_ex[SK_PATS], idx_in[SK_PATS], idx_out[SK_PATS], nex = 0, nin = 0, nout = 0, i, en = 1;
    for (i = 0; i < npat; i++) {
        if (pats[i][0] == '!')
            idx_ex[nex++] = i;
        else if (pats[i][0] == '+')
            idx_in[nin++] = i;
        else if (pats[i][0] == '-')
            idx_out[nout++] = i;
    }
    if (nex && matches(file, base, pats, idx_ex, nex, 0))
        en = 0;
    if (nin && matches(file, base, pats, idx_in, nin, 1))
        en = 1;
    if (nout && matches(file, base, pats, idx_out, nout, 1))
        en = 0;
    return en;
}

/* applyPatterns over the files of one settings file's plain entries */
static int enabled_by_patterns(const char *file, const char *base, char (*pats)[300], int npat)
{
    int idx_inc[SK_PATS], idx_ex[SK_PATS], idx_in[SK_PATS], idx_out[SK_PATS], ninc = 0, nex = 0, nin = 0, nout = 0, i, en = 1;
    for (i = 0; i < npat; i++) {
        if (pats[i][0] == '+')
            idx_in[nin++] = i;
        else if (pats[i][0] == '-')
            idx_out[nout++] = i;
        else if (pats[i][0] == '!')
            idx_ex[nex++] = i;
        else
            idx_inc[ninc++] = i;
    }
    if (ninc && !matches(file, base, pats, idx_inc, ninc, 0))
        en = 0;
    if (nex && matches(file, base, pats, idx_ex, nex, 0))
        en = 0;
    if (nin && matches(file, base, pats, idx_in, nin, 1))
        en = 1;
    if (nout && matches(file, base, pats, idx_out, nout, 1))
        en = 0;
    return en;
}

/* an absolute AmigaDOS path or a Unix one */
static int is_abs(const char *p)
{
    return p[0] == '/' || strchr(p, ':') != 0;
}

/* the plain entries of a settings file collected (their files, each enabled by the patterns), source local */
static int settings_files(sk_ctx *c, sk_set *set, const char *base, int scope, fl_list *out)
{
    int i, rc = 0;
    for (i = 0; i < set->nplain && rc == 0; i++) {
        char p[300];
        fl_list found;
        int j, kind;
        memset(&found, 0, sizeof(found));
        if (is_abs(set->plain[i]))
            cl_copy(p, set->plain[i], sizeof(p));
        else if (path_join(base, set->plain[i], p, sizeof(p)))
            continue;
        kind = c->sys->kind(c->sys->u, p);
        c->cur_source = SKO_LOCAL;
        c->cur_scope = scope;
        if (kind == 1)
            rc = fl_add(&found, p, SKO_LOCAL, scope);
        else if (kind == 2)
            rc = collect_dir(c, p, 0, &found);
        for (j = 0; j < found.n && rc == 0; j++)
            if (enabled_by_patterns(found.e[j].path, base, set->pat, set->npat))
                rc = fl_add(out, found.e[j].path, SKO_LOCAL, scope);
        fl_free(&found);
    }
    return rc;
}

/* auto-discovered files of dir ("pi" or "agents" style) kept when the overrides allow them */
static int auto_files(sk_ctx *c, const char *dir, int agents, const char *base, sk_set *set, int scope, fl_list *out)
{
    fl_list found;
    int j, rc;
    memset(&found, 0, sizeof(found));
    c->cur_source = SKO_AUTO;
    c->cur_scope = scope;
    c->cur_base = base;
    rc = collect_dir(c, dir, agents, &found);
    c->cur_base = 0;
    for (j = 0; j < found.n && rc == 0; j++)
        if (enabled_by_overrides(found.e[j].path, base, set->pat, set->npat))
            rc = fl_add_base(out, found.e[j].path, SKO_AUTO, scope, 0, base);
    fl_free(&found);
    return rc;
}

/* pi's collectAncestorAgentsSkillDirs: <dir>/.agents/skills for cwd and each ancestor up to the one holding .git (all the way up
 * without one) */
static int ancestor_agents(sk_ctx *c, const char *cwd, fl_list *out, sk_set *set)
{
    char dirs[32][300], top[300], cur[300], up[300], g[300];
    int n = 0, i, rc = 0, repo = 0;
    /* the repository's top: the nearest directory holding .git, starting at cwd */
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
        if (n < 32 && path_join(cur, ".agents/skills", dirs[n], 300) == 0)
            n++;
        if (repo && !strcmp(cur, top))
            break;
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            break;
        cl_copy(cur, up, sizeof(cur));
    }
    for (i = 0; i < n && rc == 0; i++) {
        char agents_base[300];
        path_parent(dirs[i], agents_base, sizeof(agents_base));
        rc = auto_files(c, dirs[i], 1, agents_base, set, SKS_PROJECT, out);
    }
    return rc;
}

/* ---- loading one file (pi: loadSkillFromFile) ---- */

static int add_skill(pi_skills *k, pi_skill *s)
{
    if (k->n == k->cap) {
        int nc = k->cap ? k->cap * 2 : 16;
        pi_skill *q = (pi_skill *)realloc(k->s, (size_t)nc * sizeof(pi_skill));
        if (!q)
            return -1;
        k->s = q;
        k->cap = nc;
    }
    k->s[k->n++] = *s;
    return 0;
}

/* a file's validation messages and, when it is a skill, the skill: 0, -1 out of memory. *got 1 when s is filled in */
static int load_file(sk_ctx *c, const char *path, int source, int scope, const char *sbase, pi_skill *s, int *got)
{
    static const char *const keys[3] = { "name", "description", "disable-model-invocation" };
    char *b = 0, err[600], dir[300];
    long n = 0;
    pi_yv f[3];
    int declared = !strcmp(base_name(path), "SKILL.md"), frc, has_desc, rc = 0;
    const char *name;
    *got = 0;
    if (c->sys->read(c->sys->u, path, SK_FILE_MAX, &b, &n)) {
        const char *why = c->sys->err ? c->sys->err(c->sys->u) : 0;
        return add_diag(c->k, SKD_WARNING, why && why[0] ? why : "failed to read skill file", path, 0, 0);
    }
    c->k->reads++;
    pi_skills_reads++;
    frc = pi_fm_parse(b, n, keys, 3, f, 0, 0, err, sizeof(err));
    CL_FREE(b);
    if (frc == -1)
        return -1;
    if (frc == -2) {
        if (declared)
            return add_diag(c->k, SKD_WARNING, err, path, 0, 0);
        return 0;
    }
    has_desc = f[1].kind == YV_STR && f[1].s;
    if (has_desc) {
        long i = 0;
        while (f[1].s[i] && pi_js_space_len((const unsigned char *)f[1].s + i))
            i += pi_js_space_len((const unsigned char *)f[1].s + i);
        has_desc = f[1].s[i] != 0;
    }
    if (!declared && !has_desc) {
        pi_yv_free(f, 3);
        return 0;
    }
    path_parent(path, dir, sizeof(dir));
    /* the description */
    if (!has_desc)
        rc = add_diag(c->k, SKD_WARNING, "description is required", path, 0, 0);
    else if (js_len(f[1].s) > SK_MAX_DESC) {
        char m[100];
        long v = js_len(f[1].s);
        char r[16];
        int t = 0, q;
        while (v) {
            r[t++] = (char)('0' + v % 10);
            v /= 10;
        }
        cl_copy(m, "description exceeds 1024 characters (", sizeof(m));
        q = (int)strlen(m);
        while (t)
            m[q++] = r[--t];
        m[q] = 0;
        cl_cat(m, ")", sizeof(m));
        rc = add_diag(c->k, SKD_WARNING, m, path, 0, 0);
    }
    /* the name: the frontmatter's, else the directory's */
    name = f[0].kind == YV_STR && f[0].s && f[0].s[0] ? f[0].s : base_name(dir);
    if (rc == 0) {
        long len = js_len(name);
        const char *p;
        int bad = 0;
        if (len > SK_MAX_NAME) {
            char m[100], r[16];
            long v = len;
            int t = 0, q;
            while (v) {
                r[t++] = (char)('0' + v % 10);
                v /= 10;
            }
            cl_copy(m, "name exceeds 64 characters (", sizeof(m));
            q = (int)strlen(m);
            while (t)
                m[q++] = r[--t];
            m[q] = 0;
            cl_cat(m, ")", sizeof(m));
            rc = add_diag(c->k, SKD_WARNING, m, path, 0, 0);
        }
        for (p = name; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                bad = 1;
        if (!*name)
            bad = 1;
        if (rc == 0 && bad)
            rc = add_diag(c->k, SKD_WARNING, "name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)", path, 0, 0);
        if (rc == 0 && (name[0] == '-' || (*name && name[strlen(name) - 1] == '-')))
            rc = add_diag(c->k, SKD_WARNING, "name must not start or end with a hyphen", path, 0, 0);
        if (rc == 0 && strstr(name, "--"))
            rc = add_diag(c->k, SKD_WARNING, "name must not contain consecutive hyphens", path, 0, 0);
    }
    if (rc == 0 && has_desc) {
        memset(s, 0, sizeof(*s));
        s->name = dupn(name, (long)strlen(name));
        s->desc = dupn(f[1].s, (long)strlen(f[1].s));
        cl_copy(s->path, path, sizeof(s->path));
        cl_copy(s->base, dir, sizeof(s->base));
        s->hidden = f[2].kind == YV_TRUE;
        s->source = source;
        s->scope = scope;
        /* a --skill path has no source directory of its own: pi's loadSkills gives the skill's directory */
        cl_copy(s->sbase, scope == SKS_TEMP && source == SKO_LOCAL ? dir : sbase, sizeof(s->sbase));
        if (!s->name || !s->desc) {
            free(s->name);
            free(s->desc);
            rc = -1;
        } else
            *got = 1;
    }
    pi_yv_free(f, 3);
    return rc;
}

/* pi's addSkills for one file: a skill kept unless its file or its name was seen */
static int keep_skill(sk_ctx *c, pi_skill *s, char (*canon)[300], int *ncanon)
{
    pi_skills *k = c->k;
    char real[300];
    int i;
    if (!c->sys->canon || c->sys->canon(c->sys->u, s->path, real, sizeof(real)))
        cl_copy(real, s->path, sizeof(real));
    for (i = 0; i < *ncanon; i++)
        if (!strcmp(canon[i], real)) {
            free(s->name);
            free(s->desc);
            return 0;           /* the same file reached twice (a link): silent */
        }
    for (i = 0; i < k->n; i++)
        if (!strcmp(k->s[i].name, s->name)) {
            /* a collision: reported after the other diagnostics (pi: collisionDiagnostics), see pi_skills_load */
            char m[200];
            int rc;
            cl_copy(m, "name \"", sizeof(m));
            cl_cat(m, s->name, sizeof(m));
            cl_cat(m, "\" collision", sizeof(m));
            rc = add_diag(k, SKD_COLLISION, m, s->path, k->s[i].path, s->name);
            free(s->name);
            free(s->desc);
            return rc;
        }
    if (*ncanon < 512)
        cl_copy(canon[(*ncanon)++], real, 300);
    return add_skill(k, s);
}

/* ---- the whole load ---- */

static int diag_is_coll(const pi_skill_diag *d)
{
    return d->type == SKD_COLLISION;
}

/* the collision diagnostics go after all the others (pi: [...allDiagnostics, ...collisionDiagnostics]) */
static void collisions_last(pi_skills *k)
{
    pi_skill_diag *t = (pi_skill_diag *)malloc(sizeof(pi_skill_diag) * (size_t)(k->nd ? k->nd : 1));
    int i, m = 0;
    if (!t)
        return;
    for (i = 0; i < k->nd; i++)
        if (!diag_is_coll(&k->d[i]))
            t[m++] = k->d[i];
    for (i = 0; i < k->nd; i++)
        if (diag_is_coll(&k->d[i]))
            t[m++] = k->d[i];
    memcpy(k->d, t, sizeof(pi_skill_diag) * (size_t)k->nd);
    free(t);
}

int pi_skills_load(pi_skills *k, cl_sys *sys, const pi_skill_opts *o)
{
    sk_ctx c;
    fl_list files;
    sk_set gset, pset;
    int gcmd = -1, pcmd = -1, rc = 0, i;
    char gfile[300], pfile[300], proj_base[300], user_skills[300], user_agents[300], pskills[300];
    static char canon[512][300];
    int ncanon = 0;
    memset(k, 0, sizeof(*k));
    k->commands = 1;
    k->sys = sys;
    memset(&c, 0, sizeof(c));
    c.sys = sys;
    c.k = k;
    memset(&files, 0, sizeof(files));
    gfile[0] = pfile[0] = proj_base[0] = 0;
    memset(&gset, 0, sizeof(gset));
    memset(&pset, 0, sizeof(pset));
    if (o->agent_dir && o->agent_dir[0] && path_join(o->agent_dir, "settings.json", gfile, sizeof(gfile)))
        gfile[0] = 0;
    if (o->trusted && o->cwd && path_join(o->cwd, ".pi", proj_base, sizeof(proj_base)) == 0 &&
        path_join(proj_base, "settings.json", pfile, sizeof(pfile)))
        pfile[0] = 0;
    read_settings(sys, gfile, &gset, &gcmd);
    if (o->trusted)
        read_settings(sys, pfile, &pset, &pcmd);
    /* settings.json's enableSkillCommands: the project's over the global (pi's merged settings) */
    if (pcmd >= 0)
        k->commands = pcmd;
    else if (gcmd >= 0)
        k->commands = gcmd;
    if (!o->none) {
        /* resolve(): the settings entries (project first), then the auto-discovered directories */
        if (o->trusted && o->cwd)
            rc = settings_files(&c, &pset, proj_base, SKS_PROJECT, &files);
        if (rc == 0 && o->agent_dir && o->agent_dir[0])
            rc = settings_files(&c, &gset, o->agent_dir, SKS_USER, &files);
        if (rc == 0 && o->trusted && o->cwd && path_join(proj_base, "skills", pskills, sizeof(pskills)) == 0)
            rc = auto_files(&c, pskills, 0, proj_base, &pset, SKS_PROJECT, &files);
        if (rc == 0 && o->trusted && o->cwd)
            rc = ancestor_agents(&c, o->cwd, &files, &pset);
        if (rc == 0 && o->agent_dir && o->agent_dir[0]) {
            if (path_join(o->agent_dir, "skills", user_skills, sizeof(user_skills)) == 0)
                rc = auto_files(&c, user_skills, 0, o->agent_dir, &gset, SKS_USER, &files);
            if (rc == 0 && path_join(o->agent_dir, "agents-skills", user_agents, sizeof(user_agents)) == 0)
                rc = auto_files(&c, user_agents, 1, o->agent_dir, &gset, SKS_USER, &files);
        }
        /* D10: bulky data lives in the data directory (UP-Term:var/amiga-pi) when the kit is there: its skills count as the
         * user's, after the agent directory's own */
        if (rc == 0 && o->data_dir && o->data_dir[0] && !(o->agent_dir && !strcmp(o->agent_dir, o->data_dir)) &&
            path_join(o->data_dir, "skills", user_skills, sizeof(user_skills)) == 0)
            rc = auto_files(&c, user_skills, 0, o->data_dir, &gset, SKS_USER, &files);
    }
    /* --skill: loadSkills' skillPaths, after the discovered ones */
    for (i = 0; i < o->npaths && rc == 0; i++) {
        char p[300];
        int kind;
        if (is_abs(o->paths[i]))
            cl_copy(p, o->paths[i], sizeof(p));
        else if (!o->cwd || path_join(o->cwd, o->paths[i], p, sizeof(p)))
            continue;
        kind = sys->kind(sys->u, p);
        c.cur_source = SKO_LOCAL;
        c.cur_scope = SKS_TEMP;
        if (kind == 2)
            rc = collect_dir(&c, p, 0, &files);
        else if (kind == 1)
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, ends_md(p) ? 0 : 2, "");
        else
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, 1, "");
    }
    for (i = 0; i < files.n && rc == 0; i++) {
        pi_skill s;
        int got = 0;
        if (files.e[i].bad || !ends_md(files.e[i].path)) {
            rc = add_diag(k, SKD_WARNING, files.e[i].bad == 1 ? "skill path does not exist" : "skill path is not a markdown file",
                          files.e[i].path, 0, 0);
            continue;
        }
        rc = load_file(&c, files.e[i].path, files.e[i].source, files.e[i].scope, files.e[i].sbase, &s, &got);
        if (rc == 0 && got)
            rc = keep_skill(&c, &s, canon, &ncanon);
    }
    fl_free(&files);
    collisions_last(k);
    return rc ? -1 : 0;
}

void pi_skills_free(pi_skills *k)
{
    int i;
    for (i = 0; i < k->n; i++) {
        free(k->s[i].name);
        free(k->s[i].desc);
    }
    for (i = 0; i < k->nd; i++) {
        free(k->d[i].msg);
        free(k->d[i].name);
    }
    free(k->s);
    free(k->d);
    memset(k, 0, sizeof(*k));
}

/* ---- the prompt section ---- */

static void xml_escape(jw *out, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&': jw_rawz(out, "&amp;"); break;
        case '<': jw_rawz(out, "&lt;"); break;
        case '>': jw_rawz(out, "&gt;"); break;
        case '"': jw_rawz(out, "&quot;"); break;
        case '\'': jw_rawz(out, "&apos;"); break;
        default: jw_raw(out, s, 1); break;
        }
    }
}

int pi_skills_format(const pi_skills *k, const char *reader, jw *out)
{
    int i, vis = 0;
    for (i = 0; i < k->n; i++)
        if (!k->s[i].hidden)
            vis++;
    if (!vis)
        return 0;
    jw_rawz(out, "The following skills provide specialized instructions for specific tasks.\n");
    jw_rawz(out, !reader ? "Load a skill's file when the task matches its description."
                         : !strcmp(reader, "read") ? "Use the read tool to load a skill's file when the task matches its description."
                                                   : "Use bash to load a skill's file when the task matches its description.");
    jw_rawz(out, "\nWhen a skill file references a relative path, resolve it against the skill directory (parent of SKILL.md / "
                 "dirname of the path) and use that absolute path in tool commands.\n\n<available_skills>");
    for (i = 0; i < k->n; i++) {
        if (k->s[i].hidden)
            continue;
        jw_rawz(out, "\n  <skill>\n    <name>");
        xml_escape(out, k->s[i].name);
        jw_rawz(out, "</name>\n    <description>");
        xml_escape(out, k->s[i].desc);
        jw_rawz(out, "</description>\n    <location>");
        xml_escape(out, k->s[i].path);
        jw_rawz(out, "</location>\n  </skill>");
    }
    jw_rawz(out, "\n</available_skills>");
    return out->oom ? -1 : 0;
}

/* ---- /skill:name ---- */

int pi_skill_expand(const pi_skills *k, const char *text, jw *out)
{
    const char *sp, *args, *nm;
    long nl, an;
    int i;
    char *b = 0, *body = 0, err[600];
    long n = 0, bn = 0;
    if (strncmp(text, "/skill:", 7))
        return 0;
    sp = strchr(text, ' ');
    nm = text + 7;
    nl = sp ? (long)(sp - nm) : (long)strlen(nm);
    args = sp ? sp + 1 : "";
    for (i = 0; i < k->n; i++)
        if ((long)strlen(k->s[i].name) == nl && !strncmp(k->s[i].name, nm, (size_t)nl))
            break;
    if (i == k->n)
        return 0;
    if (!k->sys || k->sys->read(k->sys->u, k->s[i].path, SK_FILE_MAX, &b, &n))
        return 0;
    pi_skills_reads++;
    /* stripFrontmatter(content).trim(): the body after the frontmatter, or the whole text, trimmed; a YAML error is pi's
     * catch: the text goes on as typed */
    {
        int frc = pi_fm_parse(b, n, 0, 0, 0, &body, &bn, err, sizeof(err));
        const unsigned char *x, *e;
        CL_FREE(b);
        if (frc == -1)
            return -1;
        if (frc)
            return 0;
        x = (const unsigned char *)body;
        e = x + bn;
        while (x < e && pi_js_space_len(x))
            x += pi_js_space_len(x);
        while (e > x && pi_js_space_back(x, e))
            e -= pi_js_space_back(x, e);
        jw_rawz(out, "<skill name=\"");
        jw_rawz(out, k->s[i].name);
        jw_rawz(out, "\" location=\"");
        jw_rawz(out, k->s[i].path);
        jw_rawz(out, "\">\nReferences are relative to ");
        jw_rawz(out, k->s[i].base);
        jw_rawz(out, ".\n\n");
        jw_raw(out, (const char *)x, (long)(e - x));
        jw_rawz(out, "\n</skill>");
    }
    free(body);
    /* args: text after the first space, trimmed */
    an = (long)strlen(args);
    while (an && pi_js_space_len((const unsigned char *)args)) {
        int sl = pi_js_space_len((const unsigned char *)args);
        args += sl;
        an -= sl;
    }
    while (an && pi_js_space_back((const unsigned char *)args, (const unsigned char *)args + an))
        an -= pi_js_space_back((const unsigned char *)args, (const unsigned char *)args + an);
    if (an) {
        jw_rawz(out, "\n\n");
        jw_raw(out, args, an);
    }
    return out->oom ? -1 : 1;
}

void pi_skill_command(const pi_skill *s, char *name, long ncap, char *desc, long dcap)
{
    const char *tag = s->scope == SKS_USER ? "[u] " : s->scope == SKS_PROJECT ? "[p] " : "[t] ";
    cl_copy(name, "skill:", ncap);
    cl_cat(name, s->name, ncap);
    cl_copy(desc, tag, dcap);
    cl_cat(desc, s->desc, dcap);
}

int pi_skills_commands(const pi_skills *k, jw *out)
{
    int i;
    for (i = 0; i < k->n; i++) {
        const pi_skill *s = &k->s[i];
        char nm[400];
        cl_copy(nm, "skill:", sizeof(nm));
        cl_cat(nm, s->name, sizeof(nm));
        jw_rawz(out, i ? "," : "");
        jw_rawz(out, "{\"name\":");
        jw_str(out, nm, (long)strlen(nm));
        jw_rawz(out, ",\"description\":");
        jw_str(out, s->desc, (long)strlen(s->desc));
        jw_rawz(out, ",\"source\":\"skill\",\"sourceInfo\":{\"path\":");
        jw_str(out, s->path, (long)strlen(s->path));
        jw_rawz(out, ",\"source\":\"");
        jw_rawz(out, s->source == SKO_AUTO ? "auto" : "local");
        jw_rawz(out, "\",\"scope\":\"");
        jw_rawz(out, s->scope == SKS_USER ? "user" : s->scope == SKS_PROJECT ? "project" : "temporary");
        jw_rawz(out, "\",\"origin\":\"top-level\"");
        if (s->sbase[0]) {
            jw_rawz(out, ",\"baseDir\":");
            jw_str(out, s->sbase, (long)strlen(s->sbase));
        }
        jw_rawz(out, "}}");
    }
    return out->oom ? -1 : i;
}

/* ---- the startup report ---- */

void pi_skill_label(const pi_skill *s, char *out, long cap)
{
    if (s->source == SKO_AUTO)
        cl_copy(out, s->scope == SKS_USER ? "auto (user)" : "auto (project)", cap);
    else if (s->scope == SKS_USER)
        cl_copy(out, "user", cap);
    else if (s->scope == SKS_PROJECT)
        cl_copy(out, "project", cap);
    else
        cl_copy(out, "path (temp)", cap);
}

/* pi's formatPathWithSource: "<label> <path>" for a loaded skill's file, else the path */
static void path_with_source(const pi_skills *k, const char *path, char *out, long cap)
{
    int i;
    for (i = 0; i < k->n; i++)
        if (!strcmp(k->s[i].path, path)) {
            pi_skill_label(&k->s[i], out, cap);
            cl_cat(out, " ", cap);
            cl_cat(out, path, cap);
            return;
        }
    cl_copy(out, path, cap);
}

static int name_cmp(const void *a, const void *b)
{
    return pi_locale_compare(*(const char *const *)a, *(const char *const *)b);
}

void pi_skills_listing(const pi_skills *k, pi_skl_fn fn, void *u)
{
    const char **names;
    int i;
    jw w;
    if (!k->n)
        return;
    names = (const char **)malloc(sizeof(char *) * (size_t)k->n);
    if (!names)
        return;
    for (i = 0; i < k->n; i++)
        names[i] = k->s[i].name;
    qsort(names, (size_t)k->n, sizeof(char *), name_cmp);
    jw_init(&w);
    jw_rawz(&w, "  ");
    for (i = 0; i < k->n; i++) {
        if (i)
            jw_rawz(&w, ", ");
        jw_rawz(&w, names[i]);
    }
    if (!w.oom)
        fn(u, SKL_LISTING, w.p);
    jw_free(&w);
    free(names);
}

int pi_skills_report(const pi_skills *k, pi_skl_fn fn, void *u)
{
    int i, lines = 0;
    char buf[900], p[700];
    /* the collisions grouped by name, in the order the names first collided */
    for (i = 0; i < k->nd; i++) {
        int j, seen = 0;
        if (k->d[i].type != SKD_COLLISION)
            continue;
        for (j = 0; j < i; j++)
            if (k->d[j].type == SKD_COLLISION && !strcmp(k->d[j].name, k->d[i].name))
                seen = 1;
        if (seen)
            continue;
        cl_copy(buf, "  \"", sizeof(buf));
        cl_cat(buf, k->d[i].name, sizeof(buf));
        cl_cat(buf, "\" collision:", sizeof(buf));
        fn(u, SKL_COLL_HEAD, buf);
        lines++;
        path_with_source(k, k->d[i].winner, p, sizeof(p));
        cl_copy(buf, "    \342\234\223 ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, SKL_COLL_WIN, buf);
        lines++;
        for (j = i; j < k->nd; j++)
            if (k->d[j].type == SKD_COLLISION && !strcmp(k->d[j].name, k->d[i].name)) {
                path_with_source(k, k->d[j].path, p, sizeof(p));
                cl_copy(buf, "    \342\234\227 ", sizeof(buf));
                cl_cat(buf, p, sizeof(buf));
                cl_cat(buf, " (skipped)", sizeof(buf));
                fn(u, SKL_COLL_LOSE, buf);
                lines++;
            }
    }
    for (i = 0; i < k->nd; i++) {
        if (k->d[i].type == SKD_COLLISION)
            continue;
        path_with_source(k, k->d[i].path, p, sizeof(p));
        cl_copy(buf, "  ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, k->d[i].type == SKD_ERROR ? SKL_ERROR : SKL_WARN, buf);
        cl_copy(buf, "    ", sizeof(buf));
        cl_cat(buf, k->d[i].msg, sizeof(buf));
        fn(u, k->d[i].type == SKD_ERROR ? SKL_ERROR : SKL_WARN, buf);
        lines += 2;
    }
    return lines;
}
