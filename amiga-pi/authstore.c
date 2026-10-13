/* authstore -- see authstore.h */
#include <stdlib.h>
#include <string.h>
#include "authstore.h"
#include "claude/util.h"

static void join(const char *dir, const char *name, char *out, long cap)
{
    cl_copy(out, dir, cap);
    if (out[0] && out[strlen(out) - 1] != ':' && out[strlen(out) - 1] != '/')
        cl_cat(out, "/", cap);
    cl_cat(out, name, cap);
}

static char *read_path(cl_sys *sys, const char *path, long *n)
{
    char *b = 0, *c;
    long len = 0;
    if (sys->kind(sys->u, path) != 1 || sys->read(sys->u, path, 4L * 1024 * 1024, &b, &len))
        return 0;
    c = (char *)malloc((size_t)len + 1);
    if (c) {
        memcpy(c, b, (size_t)len);
        c[len] = 0;
        *n = len;
    }
    CL_FREE(b);
    return c;
}

char *pi_store_read(cl_sys *sys, const char *dir, const char *name, long *n)
{
    char path[320], alt[330];
    char *t;
    *n = 0;
    if (!sys || !dir || !dir[0])
        return 0;
    join(dir, name, path, sizeof(path));
    t = read_path(sys, path, n);
    if (!t && sys->kind(sys->u, path) == 0) {
        /* an interrupted replace: the complete new file, else the old one */
        cl_copy(alt, path, sizeof(alt));
        cl_cat(alt, ".new", sizeof(alt));
        t = read_path(sys, alt, n);
        if (!t) {
            cl_copy(alt, path, sizeof(alt));
            cl_cat(alt, ".old", sizeof(alt));
            t = read_path(sys, alt, n);
        }
    }
    return t;
}

int pi_store_write(cl_sys *sys, const char *dir, const char *name, const char *text, long n, char *err, long cap)
{
    char path[320], nw[330], old[330];
    if (!sys || !sys->write || !sys->rename || !dir || !dir[0]) {
        cl_copy(err, "cannot write the credentials file here", cap);
        return -1;
    }
    join(dir, name, path, sizeof(path));
    cl_copy(nw, path, sizeof(nw));
    cl_cat(nw, ".new", sizeof(nw));
    cl_copy(old, path, sizeof(old));
    cl_cat(old, ".old", sizeof(old));
    if (sys->mkdir)
        sys->mkdir(sys->u, dir);
    if (sys->write(sys->u, nw, text, n)) {
        cl_copy(err, "cannot write ", cap);
        cl_cat(err, nw, cap);
        return -1;
    }
    if (sys->rename(sys->u, nw, path) == 0)
        return 0;                       /* nothing was there, or the host's rename replaced it */
    /* the old file is in the way (AmigaDOS): it steps aside, the new one takes its place, the old one goes */
    if (sys->kind(sys->u, old) && sys->remove)
        sys->remove(sys->u, old);
    if (sys->rename(sys->u, path, old)) {
        cl_copy(err, "cannot replace ", cap);
        cl_cat(err, path, cap);
        return -1;
    }
    if (sys->rename(sys->u, nw, path)) {
        sys->rename(sys->u, old, path);
        cl_copy(err, "cannot replace ", cap);
        cl_cat(err, path, cap);
        return -1;
    }
    if (sys->remove)
        sys->remove(sys->u, old);
    return 0;
}

static void ind(jw *w, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        jw_raw(w, "  ", 2);
}

/* JSON.stringify(v, null, 2) with every scalar kept as its text */
static void pp(jw *w, jv v, int depth)
{
    jit it;
    jv k, e;
    long n, i = 0;
    int type = json_type(v);
    if (type != J_OBJ && type != J_ARR) {
        jw_raw(w, v.p, v.n);
        return;
    }
    n = json_count(v);
    if (!n) {
        jw_rawz(w, type == J_OBJ ? "{}" : "[]");
        return;
    }
    jw_rawz(w, type == J_OBJ ? "{\n" : "[\n");
    json_iter(v, &it);
    while (json_next(&it, type == J_OBJ ? &k : 0, &e)) {
        ind(w, depth + 1);
        if (type == J_OBJ) {
            jw_raw(w, k.p, k.n);
            jw_rawz(w, ": ");
        }
        pp(w, e, depth + 1);
        jw_rawz(w, ++i < n ? ",\n" : "\n");
    }
    ind(w, depth);
    jw_rawz(w, type == J_OBJ ? "}" : "]");
}

int pi_jobj_set(const char *text, long n, const char *key, const char *value, jw *out, char *err, long cap)
{
    jv root, k, v, nv;
    jit it;
    long i = 0, cnt = 0, total;
    int found = 0, blank = 1;
    for (i = 0; i < n; i++)
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\n' && text[i] != '\r')
            blank = 0;
    if (!blank && (json_parse(text, n, &root) || json_type(root) != J_OBJ)) {
        cl_copy(err, "the file is not a JSON object", cap);
        return -1;
    }
    if (value && json_parse(value, (long)strlen(value), &nv)) {
        cl_copy(err, "the new value is not JSON", cap);
        return -1;
    }
    /* the members that stay, counted (to know where the last one is) */
    total = 0;
    if (!blank) {
        json_iter(root, &it);
        while (json_next(&it, &k, &v)) {
            int same = json_streq(k, key);
            if (same)
                found = 1;
            if (!same || value)
                total++;
        }
    }
    if (value && !found)
        total++;
    if (!total) {
        jw_rawz(out, "{}");
        return out->oom ? -2 : 0;
    }
    jw_rawz(out, "{\n");
    if (!blank) {
        json_iter(root, &it);
        while (json_next(&it, &k, &v)) {
            int same = json_streq(k, key);
            if (same && !value)
                continue;
            ind(out, 1);
            jw_raw(out, k.p, k.n);
            jw_rawz(out, ": ");
            pp(out, same ? nv : v, 1);
            jw_rawz(out, ++cnt < total ? ",\n" : "\n");
        }
    }
    if (value && !found) {
        ind(out, 1);
        jw_strz(out, key);
        jw_rawz(out, ": ");
        pp(out, nv, 1);
        jw_rawz(out, "\n");
    }
    jw_rawz(out, "}");
    return out->oom ? -2 : 0;
}

char *pi_jobj_str(const char *text, long n, const char *key)
{
    jv root, v;
    if (!text || json_parse(text, n, &root) || json_type(root) != J_OBJ || !json_get(root, key, &v) ||
        json_type(v) != J_STR)
        return 0;
    return json_strdup(v, 0);
}
