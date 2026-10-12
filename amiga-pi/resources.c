/* see resources.h */
#include <stdlib.h>
#include <string.h>
#include "resources.h"
#include "claude/memory.h"
#include "claude/trust.h"
#include "claude/path.h"
#include "claude/util.h"

/* pi's loadContextFileFromDir candidates, the first found per directory */
static const mem_cand pi_files[] = {
    { "AGENTS.override.md", MEMC_ALONE }, { "AGENTS.md", MEMC_ALONE }, { "AGENTS.MD", MEMC_ALONE },
    { "CLAUDE.md", MEMC_ALONE }, { "CLAUDE.MD", MEMC_ALONE }, { 0, 0 }
};

long pi_res_reads;

typedef struct rtake {
    pi_res *r;
    cl_sys *sys;
} rtake;

/* a file's text, a UTF-8 BOM dropped (pi's stripBom): 0, -1 not a file or not readable */
static int read_text(pi_res *r, cl_sys *sys, const char *path, char **out, long *n)
{
    *out = 0;
    *n = 0;
    if (sys->kind(sys->u, path) != 1 || sys->read(sys->u, path, PI_RES_MAX, out, n))
        return -1;
    r->reads++;
    pi_res_reads++;
    if (*n >= 3 && (unsigned char)(*out)[0] == 0xEF && (unsigned char)(*out)[1] == 0xBB &&
        (unsigned char)(*out)[2] == 0xBF) {
        memmove(*out, *out + 3, (size_t)(*n - 3));
        *n -= 3;
        (*out)[*n] = 0;
    }
    return 0;
}

static int take_ctx(void *u, const char *path, int flags)
{
    rtake *t = (rtake *)u;
    pi_res *r = t->r;
    char same[300];
    int i;
    (void)flags;
    if (t->sys->kind(t->sys->u, path) != 1)
        return 0;
    /* a file is the same file by its canonical name, as the working directory's is (NameFromLock): the agent
     * directory spelled through an assign (ENVARC:amiga-pi) is the working directory when one is inside the other */
    if (!t->sys->canon || t->sys->canon(t->sys->u, path, same, sizeof(same)))
        cl_copy(same, path, sizeof(same));
    for (i = 0; i < r->nf; i++)
        if (cl_strieq(r->f[i].same, same))
            return 1;           /* seen (the agent directory is an ancestor): this directory's file, once */
    if (r->nf >= PI_CTX_FILES)
        return 1;
    if (read_text(r, t->sys, path, &r->f[r->nf].text, &r->f[r->nf].n))
        return 0;               /* pi: a file it cannot read is passed over */
    cl_copy(r->f[r->nf].path, path, sizeof(r->f[0].path));
    cl_copy(r->f[r->nf].same, same, sizeof(r->f[0].same));
    r->nf++;
    return 1;
}

/* pi's discoverSystemPromptFile / discoverAppendSystemPromptFile and resolvePromptInput */
static void prompt_file(pi_res *r, cl_sys *sys, const char *agent_dir, const char *cwd, const char *name,
                        char **text, char *where)
{
    char p[300];
    long n;
    *text = 0;
    where[0] = 0;
    cl_copy(p, ".pi/", sizeof(p));
    cl_cat(p, name, sizeof(p));
    if (r->trusted && path_join(cwd, p, where, 300) == 0 && read_text(r, sys, where, text, &n) == 0)
        return;
    where[0] = 0;
    if (agent_dir && agent_dir[0] && path_join(agent_dir, name, where, 300) == 0 &&
        read_text(r, sys, where, text, &n) == 0)
        return;
    where[0] = 0;
}

int pi_res_load(pi_res *r, cl_sys *sys, const char *agent_dir, const char *cwd, int trusted)
{
    rtake t;
    memset(r, 0, sizeof(*r));
    r->trusted = trusted;
    t.r = r;
    t.sys = sys;
    if (agent_dir && agent_dir[0])
        mem_dir(agent_dir, pi_files, take_ctx, &t);
    mem_walk(cwd, pi_files, take_ctx, &t);
    prompt_file(r, sys, agent_dir, cwd, "SYSTEM.md", &r->system, r->system_path);
    prompt_file(r, sys, agent_dir, cwd, "APPEND_SYSTEM.md", &r->append, r->append_path);
    return 0;
}

void pi_res_free(pi_res *r)
{
    int i;
    for (i = 0; i < r->nf; i++)
        free(r->f[i].text);
    free(r->system);
    free(r->append);
    pi_skills_free(&r->skills);
    memset(r, 0, sizeof(*r));
}

/* ---- project trust ---- */

static const char *const guarded[] = { "settings.json", "mcp.json", "extensions", "skills", "prompts", "themes",
                                       "SYSTEM.md", "APPEND_SYSTEM.md", 0 };

int pi_trust_needed(cl_sys *sys, const char *cwd)
{
    char p[300], cur[300], up[300];
    int i;
    for (i = 0; guarded[i]; i++) {
        cl_copy(p, ".pi/", sizeof(p));
        cl_cat(p, guarded[i], sizeof(p));
        if (path_join(cwd, p, cur, sizeof(cur)) == 0 && sys->kind(sys->u, cur) > 0)
            return 1;
    }
    /* .agents/skills here or above (pi leaves out the user's own ~/.agents/skills; the Amiga has no home) */
    cl_copy(cur, cwd, sizeof(cur));
    for (;;) {
        if (path_join(cur, ".agents/skills", p, sizeof(p)) == 0 && sys->kind(sys->u, p) > 0)
            return 1;
        if (path_parent(cur, up, sizeof(up)) || !up[0])
            return 0;
        cl_copy(cur, up, sizeof(cur));
    }
}

static int trust_file(const char *agent_dir, char *out, long cap)
{
    return agent_dir && agent_dir[0] ? path_join(agent_dir, "trust.json", out, cap) : -1;
}

/* pi's getProjectTrustOptions(cwd, { includeSessionOnly }): kinds and labels; the count */
static int options(const char *cwd, int session, int *kind, char labels[5][360])
{
    char parent[300];
    int n = 0, has_parent = path_parent(cwd, parent, sizeof(parent)) == 0 && parent[0];
    kind[n] = PI_TRUST_YES;
    cl_copy(labels[n++], "Trust", 360);
    if (has_parent) {
        kind[n] = PI_TRUST_PARENT;
        cl_copy(labels[n], "Trust parent folder (", 360);
        cl_cat(labels[n], parent, 360);
        cl_cat(labels[n++], ")", 360);
    }
    if (session) {
        kind[n] = PI_TRUST_SESSION;
        cl_copy(labels[n++], "Trust (this session only)", 360);
    }
    kind[n] = PI_TRUST_NO;
    cl_copy(labels[n++], "Do not trust", 360);
    if (session) {
        kind[n] = PI_TRUST_NO_SESSION;
        cl_copy(labels[n++], "Do not trust (this session only)", 360);
    }
    return n;
}

/* an answer's updates (pi's option.updates) written: 0, -1 */
static int keep(cl_sys *sys, const char *agent_dir, const char *cwd, int kind)
{
    char f[300], parent[300];
    if (kind == PI_TRUST_SESSION || kind == PI_TRUST_NO_SESSION)
        return 0;
    if (trust_file(agent_dir, f, sizeof(f)))
        return -1;
    if (kind == PI_TRUST_PARENT) {
        if (path_parent(cwd, parent, sizeof(parent)) || trust_put(sys, f, parent, 1))
            return -1;
        return trust_put(sys, f, cwd, -1);
    }
    return trust_put(sys, f, cwd, kind == PI_TRUST_YES);
}

int pi_trust_decide(cl_sys *sys, const char *agent_dir, const char *cwd, int override, const char *dflt,
                    pi_trust_ask_fn ask, void *ask_u)
{
    char f[300], title[600];
    char labels[5][360];
    const char *lp[5];
    int kind[5], n, i, got;
    if (override >= 0)
        return override;
    if (!pi_trust_needed(sys, cwd))
        return 1;
    if (trust_file(agent_dir, f, sizeof(f)) == 0 && (got = trust_near(sys, f, cwd, 0, 0)) != -1) {
        /* pi's readTrustFile throws on a file it cannot read: pi stops, the file stays as it is */
        return got < 0 ? -1 : got;
    }
    if (dflt && !strcmp(dflt, "always"))
        return 1;
    if (dflt && !strcmp(dflt, "never"))
        return 0;
    if (!ask)
        return 0;
    /* pi's formatProjectTrustPrompt, APP_NAME amiga-pi */
    cl_copy(title, "Trust project folder?\n", sizeof(title));
    cl_cat(title, cwd, sizeof(title));
    cl_cat(title, "\n\nThis allows amiga-pi to load .pi settings and resources, install missing project packages, "
                  "and execute project extensions.", sizeof(title));
    n = options(cwd, 1, kind, labels);
    for (i = 0; i < n; i++)
        lp[i] = labels[i];
    i = ask(ask_u, title, lp, n);
    if (i < 0 || i >= n)
        return 0;
    if (keep(sys, agent_dir, cwd, kind[i]))
        return -1;              /* pi's setMany throws: the error is pi's, not a silent "not kept" */
    return kind[i] == PI_TRUST_YES || kind[i] == PI_TRUST_PARENT || kind[i] == PI_TRUST_SESSION;
}

void pi_trust_why(cl_sys *sys, const char *agent_dir, char *err, long cap)
{
    char f[300];
    err[0] = 0;
    if (trust_file(agent_dir, f, sizeof(f))) {
        cl_copy(err, "Failed to write trust store: no agent directory", cap);
        return;
    }
    if (trust_check(sys, f, err, cap) && err[0])
        return;
    cl_copy(err, "Failed to write trust store ", cap);
    cl_cat(err, f, cap);
}

int pi_trust_command(cl_sys *sys, const char *agent_dir, const char *cwd, int pick, jw *out)
{
    char labels[5][360], f[300], where[300], num[8];
    int kind[5], n = options(cwd, 0, kind, labels), i, got;
    if (pick <= 0) {
        jw_rawz(out, "Project trust for ");
        jw_rawz(out, cwd);
        jw_rawz(out, " (/trust N keeps answer N for future sessions):");
        for (i = 0; i < n; i++) {
            num[0] = (char)('1' + i);
            num[1] = 0;
            jw_rawz(out, "\n  ");
            jw_rawz(out, num);
            jw_rawz(out, "  ");
            jw_rawz(out, labels[i]);
        }
        got = trust_file(agent_dir, f, sizeof(f)) == 0 ? trust_near(sys, f, cwd, where, sizeof(where)) : -1;
        if (got == -2) {
            char err[400];
            pi_trust_why(sys, agent_dir, err, sizeof(err));
            jw_rawz(out, "\n[ERROR] ");
            jw_rawz(out, err);
            return -1;
        }
        jw_rawz(out, got < 0 ? "\nNo decision kept." : got ? "\nKept: trusted (" : "\nKept: not trusted (");
        if (got >= 0) {
            jw_rawz(out, where);
            jw_rawz(out, ")");
        }
        return 0;
    }
    if (pick > n) {
        jw_rawz(out, "[ERROR] no answer with that number (/trust lists them)");
        return -1;
    }
    if (keep(sys, agent_dir, cwd, kind[pick - 1])) {
        char err[400];
        pi_trust_why(sys, agent_dir, err, sizeof(err));
        jw_rawz(out, "[ERROR] ");
        jw_rawz(out, err);
        return -1;
    }
    /* pi's showStatus after trustStore.setMany(selection.updates), APP_NAME amiga-pi */
    jw_rawz(out, kind[pick - 1] == PI_TRUST_NO ? "Saved trust decision: untrusted. Restart amiga-pi for this to take effect."
                                                : "Saved trust decision: trusted. Restart amiga-pi for this to take effect.");
    return 0;
}
