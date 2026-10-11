/* see ext_gate.h */
#include <stdlib.h>
#include <string.h>
#include "ext_gate.h"
#include "claude/session.h"
#include "claude/tools_int.h"   /* tl_resolve: "outside" as C:Claude decides it */
#include "claude/util.h"
#include "claude/datadir.h"
#include "claude/path.h"

/* the call's one-line summary for the question: the command or the path */
static void what_of(jv in, char *out, long cap)
{
    jv x;
    out[0] = 0;
    if ((json_get(in, "command", &x) || json_get(in, "file_path", &x) || json_get(in, "path", &x)) &&
        json_type(x) == J_STR)
        json_str(x, out, cap);
}

static int gate_call(void *u, const char *id, const char *name, jw *args, char *reason, long cap)
{
    pi_gate *g = (pi_gate *)u;
    int kind = pi_tools_kind(name), tool = T_BASH, outside = 0, d = RULE_NONE, ans;
    const cl_rule *which = 0;
    char what[300], full[512];
    jv in, x;
    jw cin;
    (void)id;
    if (!g->on)
        return 0;
    what[0] = 0;
    jw_init(&cin);
    if (kind >= 0 && pi_tools_claude(kind, args->p ? args->p : "{}", args->n, &tool, &cin) == 0 &&
        json_parse(cin.p, cin.n, &in) == 0) {
        d = cfg_decide(&g->cfg, tools_name(tool), in, g->tools->t.root, &which);
        what_of(in, what, sizeof(what));
        if ((json_get(in, "file_path", &x) || json_get(in, "path", &x)) && json_type(x) == J_STR) {
            char p[400];
            json_str(x, p, sizeof(p));
            if (tl_resolve(&g->tools->t, p, full, sizeof(full), &outside))
                outside = 1;
        }
    } else
        kind = -1;              /* not one of pi's tools: it asks */
    if (d == RULE_DENY) {
        g->blocks++;
        cl_copy(reason, "Permission to use ", cap);
        cl_cat(reason, name, cap);
        cl_cat(reason, " has been denied", cap);
        if (which) {
            cl_cat(reason, " by the rule ", cap);
            cl_cat(reason, which->text, cap);
        }
        cl_cat(reason, ".", cap);
        jw_free(&cin);
        return 1;
    }
    if (d == RULE_ALLOW || (d == RULE_NONE && kind >= 0 && !perm_must_ask(&g->tools->t.perm, tool, outside))) {
        jw_free(&cin);
        return 0;
    }
    if (!g->s->ask) {
        jw_free(&cin);
        g->blocks++;
        cl_copy(reason, "Permission to use ", cap);
        cl_cat(reason, name, cap);
        cl_cat(reason, " was denied: nobody can approve it in this run.", cap);
        return 1;
    }
    g->questions++;
    ans = g->s->ask(g->s->ask_u, tool, name, what, outside);
    if (ans == ASK_PROJECT && kind >= 0 && g->proj[0]) {
        /* kept for the project (C:Claude's rule for the call), from now on */
        char rule[300], up[400];
        cfg_rule_for(tools_name(tool), cin.p, cin.n, rule, sizeof(rule));
        if (path_parent(g->proj, up, sizeof(up)) == 0)
            dd_mkdirs(g->tools->t.sys, up);
        if (cfg_write_rule(g->tools->t.sys, g->proj, RULE_ALLOW, rule, 1) == 0)
            g->kept++;
        cfg_add_rule(&g->cfg, RULE_ALLOW, CFG_LOCAL, rule);
        jw_free(&cin);
        return 0;
    }
    jw_free(&cin);
    if (ans == ASK_ONCE)
        return 0;
    if ((ans == ASK_SESSION || ans == ASK_PROJECT) && kind >= 0) {
        if (!outside)
            perm_grant(&g->tools->t.perm, tool);
        return 0;
    }
    g->blocks++;
    cl_copy(reason, "the user declined this tool call", cap);
    return 1;
}

void pi_gate_init(pi_gate *g, pi_tools *tools, pi_session *s)
{
    memset(g, 0, sizeof(*g));
    g->tools = tools;
    g->s = s;
    cfg_init(&g->cfg);
    g->ext.name = "permission-gate";
    g->ext.u = g;
    g->ext.tool_call = gate_call;
}

void pi_gate_free(pi_gate *g)
{
    cfg_free(&g->cfg);
}

int pi_gate_settings(pi_gate *g, const char *json, long n, const char *name, int *gate)
{
    jv v, x;
    *gate = -1;
    if (json_parse(json, n, &v) || json_type(v) != J_OBJ)
        return -1;
    if (json_get(v, "permissionGate", &x))
        *gate = json_type(x) == J_TRUE ? 1 : json_type(x) == J_FALSE ? 0 : -1;
    return cfg_merge(&g->cfg, CFG_USER, json, n, name);
}

int pi_gate_project_file(const char *data_dir, const char *cwd, char *out, long cap)
{
    char stem[300];
    long k = 2;
    const char *c = cwd;
    stem[0] = stem[1] = '-';
    if (*c == '/' || *c == '\\')
        c++;                    /* pi: the leading separator dropped */
    for (; *c && k < (long)sizeof(stem) - 3; c++)
        stem[k++] = *c == '/' || *c == '\\' || *c == ':' ? '-' : *c;
    if (*c)
        return -1;
    stem[k++] = '-';
    stem[k++] = '-';
    stem[k] = 0;
    /* an FFS name is 30 characters: ".json" leaves 25 for pi's "--<cwd>--", cut as C:Claude's session names are */
    sess_fit(stem, sess_hash(stem), 25);
    cl_copy(out, data_dir, cap);
    k = (long)strlen(out);
    if (k && out[k - 1] != ':' && out[k - 1] != '/')
        cl_cat(out, "/", cap);
    cl_cat(out, "permissions/", cap);
    cl_cat(out, stem, cap);
    cl_cat(out, ".json", cap);
    return (long)strlen(out) >= cap - 1 ? -1 : 0;
}

int pi_gate_project(pi_gate *g, const char *data_dir, const char *cwd)
{
    cl_sys *sys = g->tools->t.sys;
    char *b = 0;
    long n = 0;
    if (pi_gate_project_file(data_dir, cwd, g->proj, sizeof(g->proj))) {
        g->proj[0] = 0;
        return -1;
    }
    if (sys->kind(sys->u, g->proj) == 1 && sys->read(sys->u, g->proj, 256L * 1024, &b, &n) == 0) {
        cfg_merge(&g->cfg, CFG_LOCAL, b, n, g->proj);
        CL_FREE(b);
    }
    return 0;
}
