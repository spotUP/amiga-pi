/* see skills.h */
#include <stdlib.h>
#include <string.h>
#include "skills.h"
#include "frontmatter.h"
#include "pkgres.h"
#include "tools_pi.h"
#include "claude/path.h"
#include "claude/util.h"

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
    char psrc[300];             /* a package's source string */
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
static int fl_add_base(fl_list *l, const char *path, int source, int scope, int bad, const char *sbase, const char *psrc)
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
    cl_copy(l->e[l->n].psrc, psrc, sizeof(l->e[0].psrc));
    l->n++;
    return 0;
}

typedef struct sk_ctx {
    cl_sys *sys;
    pi_skills *k;
    int oom;
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
static int load_file(sk_ctx *c, const char *path, int source, int scope, const char *sbase, const char *psrc, pi_skill *s, int *got)
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
        cl_copy(s->psrc, psrc, sizeof(s->psrc));
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

typedef struct dir_paths {
    fl_list *files;
    int rc;
} dir_paths;

static void dir_path_cb(void *u, const char *path)
{
    dir_paths *d = (dir_paths *)u;
    if (!d->rc)
        d->rc = fl_add_base(d->files, path, SKO_LOCAL, SKS_TEMP, 0, "", "");
}

int pi_skills_load(pi_skills *k, cl_sys *sys, const pi_skill_opts *o)
{
    sk_ctx c;
    fl_list files;
    pm_opts po;
    pm_list res;
    int cmd = -1, rc = 0, i;
    static char canon[512][300];
    int ncanon = 0;
    memset(k, 0, sizeof(*k));
    k->commands = 1;
    k->sys = sys;
    memset(&c, 0, sizeof(c));
    c.sys = sys;
    c.k = k;
    memset(&files, 0, sizeof(files));
    memset(&res, 0, sizeof(res));
    memset(&po, 0, sizeof(po));
    po.agent_dir = o->agent_dir;
    po.data_dir = o->data_dir;
    po.cwd = o->cwd;
    po.trusted = o->trusted;
    po.home = o->home;
    po.host = o->host;
    /* pi's DefaultPackageManager.resolve(): settings.json entries and packages, the default directories, in precedence order;
     * --no-skills leaves all of it out (enableSkillCommands is still read) */
    if (!o->none) {
        rc = pm_resolve(sys, &po, PM_SKILLS, &res, &cmd);
        for (i = 0; i < res.n && rc == 0; i++) {
            const pm_res *r = &res.e[i];
            if (r->enabled)
                rc = fl_add_base(&files, r->path, r->kind == PMK_AUTO ? SKO_AUTO : r->kind == PMK_LOCAL ? SKO_LOCAL : SKO_PACKAGE,
                                 r->scope == PMS_USER ? SKS_USER : r->scope == PMS_PROJECT ? SKS_PROJECT : SKS_TEMP, 0, r->base, r->src);
        }
        pm_free(&res);
    } else
        pm_commands(sys, &po, &cmd);
    if (cmd >= 0)
        k->commands = cmd;
    /* --skill: loadSkills' skillPaths, after the discovered ones */
    for (i = 0; i < o->npaths && rc == 0; i++) {
        char p[300], canon_p[300];
        int kind, j, dup = 0;
        /* main.ts resolveCliPaths: resolvePath(value, cwd), then loadSkills: resolvePath(path, cwd, {trim: true}) */
        if (!o->cwd || pm_path(&po, o->paths[i], o->cwd, 0, p, sizeof(p)) || pm_path(&po, p, o->cwd, 1, p, sizeof(p)))
            continue;
        kind = sys->kind(sys->u, p);
        /* mergePaths: a path that names a file already there is not read again */
        pm_canon(sys, p, canon_p, sizeof(canon_p));
        for (j = 0; j < files.n; j++) {
            char cj[300];
            pm_canon(sys, files.e[j].path, cj, sizeof(cj));
            if (!strcmp(cj, canon_p))
                dup = 1;
        }
        if (dup)
            continue;
        if (kind == 2) {
            dir_paths dp;
            dp.files = &files;
            dp.rc = 0;
            rc = pm_skill_dir(sys, p, dir_path_cb, &dp);
            if (!rc)
                rc = dp.rc;
        } else if (kind == 1)
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, ends_md(p) ? 0 : 2, "", "");
        else
            rc = fl_add_base(&files, p, SKO_LOCAL, SKS_TEMP, 1, "", "");
    }
    for (i = 0; i < files.n && rc == 0; i++) {
        pi_skill s;
        int got = 0;
        if (files.e[i].bad || !ends_md(files.e[i].path)) {
            rc = add_diag(k, SKD_WARNING, files.e[i].bad == 1 ? "skill path does not exist" : "skill path is not a markdown file",
                          files.e[i].path, 0, 0);
            continue;
        }
        rc = load_file(&c, files.e[i].path, files.e[i].source, files.e[i].scope, files.e[i].sbase, files.e[i].psrc, &s, &got);
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
        jw_rawz(out, ",\"source\":");
        if (s->source == SKO_PACKAGE)
            jw_str(out, s->psrc, (long)strlen(s->psrc));
        else
            jw_rawz(out, s->source == SKO_AUTO ? "\"auto\"" : "\"local\"");
        jw_rawz(out, ",\"scope\":\"");
        jw_rawz(out, s->scope == SKS_USER ? "user" : s->scope == SKS_PROJECT ? "project" : "temporary");
        jw_rawz(out, s->source == SKO_PACKAGE ? "\",\"origin\":\"package\"" : "\",\"origin\":\"top-level\"");
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
    if (s->source == SKO_PACKAGE) {
        /* a package: its source string, accent coloured, with the scope after it */
        cl_copy(out, s->psrc, cap);
        cl_cat(out, s->scope == SKS_USER ? " (user)" : s->scope == SKS_PROJECT ? " (project)" : " (temp)", cap);
    } else if (s->source == SKO_AUTO)
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
