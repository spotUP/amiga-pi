/* see resreport.h */
#include <stdlib.h>
#include <string.h>
#include "resreport.h"
#include "tools_pi.h"
#include "claude/util.h"
#include "claude/json.h"

static char home_dir[300];

void rr_set_home(const char *home)
{
    cl_copy(home_dir, home ? home : "", sizeof(home_dir));
}

void rr_label(int scope, int kind, const char *psrc, char *out, long cap)
{
    if (kind == PMK_PACKAGE) {
        /* a package: its source string, accent coloured, with the scope after it */
        cl_copy(out, psrc, cap);
        cl_cat(out, scope == PMS_USER ? " (user)" : scope == PMS_PROJECT ? " (project)" : " (temp)", cap);
    } else if (kind == PMK_AUTO)
        cl_copy(out, scope == PMS_USER ? "auto (user)" : "auto (project)", cap);
    else if (scope == PMS_USER)
        cl_copy(out, "user", cap);
    else if (scope == PMS_PROJECT)
        cl_copy(out, "project", cap);
    else
        cl_copy(out, "path (temp)", cap);
}

/* formatDisplayPath: the home directory written as ~ */
static void display_path(const char *p, char *out, long cap)
{
    size_t hl = strlen(home_dir);
    if (hl && !strncmp(p, home_dir, hl)) {
        cl_copy(out, "~", cap);
        cl_cat(out, p + hl, cap);
    } else
        cl_copy(out, p, cap);
}

static int name_cmp(const void *a, const void *b)
{
    return pi_locale_compare(*(const char *const *)a, *(const char *const *)b);
}

static const rr_item *g_items;

static int path_cmp(const void *a, const void *b)
{
    return pi_locale_compare(g_items[*(const int *)a].path, g_items[*(const int *)b].path);
}

void rr_listing(const rr_item *it, int n, rr_fn fn, void *u)
{
    const char **names;
    int *idx, i, g;
    jw w;
    static const int order[3] = { PMS_PROJECT, PMS_USER, PMS_TEMP };
    static const char *const gname[3] = { "project", "user", "path" };
    if (!n)
        return;
    names = (const char **)malloc(sizeof(char *) * (size_t)n);
    idx = (int *)malloc(sizeof(int) * (size_t)n);
    if (!names || !idx) {
        free(names);
        free(idx);
        return;
    }
    for (i = 0; i < n; i++)
        names[i] = it[i].name;
    qsort(names, (size_t)n, sizeof(char *), name_cmp);
    jw_init(&w);
    jw_rawz(&w, "  ");
    for (i = 0; i < n; i++) {
        if (i)
            jw_rawz(&w, ", ");
        jw_rawz(&w, names[i]);
    }
    if (!w.oom)
        fn(u, RRL_ITEM, RRM_COLLAPSED, w.p);
    jw_free(&w);
    /* the grouped list: project, user, then the paths given by hand; a group of its scope's items, by path */
    g_items = it;
    for (g = 0; g < 3; g++) {
        int m = 0;
        char line[900];
        for (i = 0; i < n; i++) {
            int grp = it[i].scope == PMS_PROJECT ? 0 : it[i].scope == PMS_USER ? 1 : 2;
            if (grp == g)
                idx[m++] = i;
        }
        if (!m)
            continue;
        cl_copy(line, "  ", sizeof(line));
        cl_cat(line, gname[g], sizeof(line));
        fn(u, RRL_SCOPE, RRM_EXPANDED, line);
        qsort(idx, (size_t)m, sizeof(int), path_cmp);
        for (i = 0; i < m; i++) {
            char shown[800];
            const rr_item *r = &it[idx[i]];
            if (r->shown)
                cl_copy(shown, r->shown, sizeof(shown));
            else
                display_path(r->path, shown, sizeof(shown));
            cl_copy(line, "    ", sizeof(line));
            cl_cat(line, shown, sizeof(line));
            fn(u, RRL_ITEM, RRM_EXPANDED, line);
        }
    }
    (void)order;
    free(names);
    free(idx);
}

/* pi's formatPathWithSource: "<label> <path>" for a loaded resource's file, else the path */
static void path_with_source(const rr_item *it, int n, const char *path, char *out, long cap)
{
    int i;
    char shown[800];
    display_path(path, shown, sizeof(shown));
    for (i = 0; i < n; i++)
        if (!strcmp(it[i].path, path)) {
            rr_label(it[i].scope, it[i].kind, it[i].psrc ? it[i].psrc : "", out, cap);
            cl_cat(out, " ", cap);
            cl_cat(out, shown, cap);
            return;
        }
    cl_copy(out, shown, cap);
}

int rr_diagnostics(const rr_diag *d, int nd, const rr_item *it, int n, rr_fn fn, void *u)
{
    int i, lines = 0;
    char buf[1800], p[1500];
    /* the collisions grouped by name, in the order the names first collided */
    for (i = 0; i < nd; i++) {
        int j, seen = 0;
        if (d[i].type != RRD_COLLISION)
            continue;
        for (j = 0; j < i; j++)
            if (d[j].type == RRD_COLLISION && !strcmp(d[j].name, d[i].name))
                seen = 1;
        if (seen)
            continue;
        cl_copy(buf, "  \"", sizeof(buf));
        cl_cat(buf, d[i].name, sizeof(buf));
        cl_cat(buf, "\" collision:", sizeof(buf));
        fn(u, RRL_COLL_HEAD, RRM_BOTH, buf);
        lines++;
        path_with_source(it, n, d[i].winner, p, sizeof(p));
        cl_copy(buf, "    \342\234\223 ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, RRL_COLL_WIN, RRM_BOTH, buf);
        lines++;
        for (j = i; j < nd; j++)
            if (d[j].type == RRD_COLLISION && !strcmp(d[j].name, d[i].name)) {
                path_with_source(it, n, d[j].path, p, sizeof(p));
                cl_copy(buf, "    \342\234\227 ", sizeof(buf));
                cl_cat(buf, p, sizeof(buf));
                cl_cat(buf, " (skipped)", sizeof(buf));
                fn(u, RRL_COLL_LOSE, RRM_BOTH, buf);
                lines++;
            }
    }
    for (i = 0; i < nd; i++) {
        if (d[i].type == RRD_COLLISION)
            continue;
        path_with_source(it, n, d[i].path, p, sizeof(p));
        cl_copy(buf, "  ", sizeof(buf));
        cl_cat(buf, p, sizeof(buf));
        fn(u, d[i].type == RRD_ERROR ? RRL_ERROR : RRL_WARN, RRM_BOTH, buf);
        cl_copy(buf, "    ", sizeof(buf));
        cl_cat(buf, d[i].msg, sizeof(buf));
        fn(u, d[i].type == RRD_ERROR ? RRL_ERROR : RRL_WARN, RRM_BOTH, buf);
        lines += 2;
    }
    return lines;
}
