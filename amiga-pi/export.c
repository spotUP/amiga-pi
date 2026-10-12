/* see export.h */
#include <stdlib.h>
#include <string.h>
#include "export.h"
#include "claude/util.h"
#include "claude/path.h"
#include "claude/json.h"

static void esc(jw *w, const char *s, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': jw_rawz(w, "&amp;"); break;
        case '<': jw_rawz(w, "&lt;"); break;
        case '>': jw_rawz(w, "&gt;"); break;
        case '"': jw_rawz(w, "&quot;"); break;
        default: jw_raw(w, s + i, 1); break;
        }
    }
}

static void esc_json_str(jw *w, jv v)
{
    long n;
    char *z = json_type(v) == J_STR ? json_strdup(v, &n) : 0;
    if (z)
        esc(w, z, n);
    free(z);
}

/* the text blocks of a content value, escaped, as <pre> blocks */
static void content_pre(jw *w, jv c, const char *cls)
{
    jw_rawz(w, "<pre class=\"");
    jw_rawz(w, cls);
    jw_rawz(w, "\">");
    if (json_type(c) == J_STR)
        esc_json_str(w, c);
    else if (json_type(c) == J_ARR) {
        jit it;
        jv b, t, x;
        int any = 0;
        json_iter(c, &it);
        while (json_next(&it, 0, &b))
            if (json_type(b) == J_OBJ && json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                if (any)
                    jw_raw(w, "\n", 1);
                esc_json_str(w, x);
                any = 1;
            }
    }
    jw_rawz(w, "</pre>\n");
}

static const char *const style_a[] = {
    "<style>\n",
    ":root{--bg:#fff;--fg:#222;--dim:#666;--user:#e8eefc;--tool:#f3f3f3;--err:#b00020;--accent:#6a3fb5}\n",
    "@media (prefers-color-scheme:dark){:root{--bg:#1b1c22;--fg:#e2e2e6;--dim:#9a9ca6;--user:#27304a;--tool:#26272e;--err:#f28b82;--accent:#c39bf2}}\n",
    "body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.5 sans-serif}\n",
    "main{max-width:60em;margin:0 auto}h1{font-size:1.2em}.meta{color:var(--dim);margin-bottom:1em}\n",
    ".msg{margin:.8em 0;padding:.5em .8em;border-radius:4px}.user{background:var(--user)}.tool{background:var(--tool)}\n",
    ".role{color:var(--accent);font-weight:bold;font-size:.85em}pre{white-space:pre-wrap;word-wrap:break-word;margin:.3em 0;font-family:monospace}\n",
    ".err{color:var(--err)}.dim{color:var(--dim)}\n",
    "</style>\n",
    0
};

static void entry_html(pi_store *st, int i, jw *w)
{
    pi_s3 *s = &st->s3;
    long n;
    const char *tx = s3_text(s, i, &n);
    jv o, m, c, v, b, t, nm, ar;
    jit it;
    if (!tx || json_parse(tx, n, &o))
        return;
    switch (s->e[i].type) {
    case S3_MESSAGE:
        if (!json_get(o, "message", &m))
            return;
        if (s->e[i].role == S3R_USER) {
            jw_rawz(w, "<div class=\"msg user\"><div class=\"role\">user</div>");
            if (json_get(m, "content", &c))
                content_pre(w, c, "text");
            jw_rawz(w, "</div>\n");
        } else if (s->e[i].role == S3R_ASSISTANT) {
            jw_rawz(w, "<div class=\"msg\"><div class=\"role\">assistant</div>");
            if (json_get(m, "content", &c) && json_type(c) == J_ARR) {
                json_iter(c, &it);
                while (json_next(&it, 0, &b)) {
                    if (!json_get(b, "type", &t))
                        continue;
                    if (json_streq(t, "text") && json_get(b, "text", &v)) {
                        jw_rawz(w, "<pre class=\"text\">");
                        esc_json_str(w, v);
                        jw_rawz(w, "</pre>\n");
                    } else if (json_streq(t, "toolCall")) {
                        jw_rawz(w, "<div class=\"tool\"><b>");
                        if (json_get(b, "name", &nm))
                            esc_json_str(w, nm);
                        jw_rawz(w, "</b> <code>");
                        if (json_get(b, "arguments", &ar))
                            esc(w, ar.p, ar.n);
                        jw_rawz(w, "</code></div>\n");
                    }
                }
            }
            if (json_get(m, "errorMessage", &v) && json_type(v) == J_STR) {
                jw_rawz(w, "<pre class=\"err\">");
                esc_json_str(w, v);
                jw_rawz(w, "</pre>\n");
            }
            jw_rawz(w, "</div>\n");
        } else if (s->e[i].role == S3R_TOOLRESULT) {
            int err = json_get(m, "isError", &v) && v.n >= 4 && !memcmp(v.p, "true", 4);
            jw_rawz(w, "<div class=\"msg tool\"><div class=\"role\">");
            if (json_get(m, "toolName", &nm))
                esc_json_str(w, nm);
            jw_rawz(w, " result</div>");
            if (json_get(m, "content", &c))
                content_pre(w, c, err ? "err" : "dim");
            jw_rawz(w, "</div>\n");
        } else if (s->e[i].role == S3R_BASH) {
            jw_rawz(w, "<div class=\"msg tool\"><div class=\"role\">bash</div><pre>$ ");
            if (json_get(m, "command", &v))
                esc_json_str(w, v);
            jw_rawz(w, "\n");
            if (json_get(m, "output", &v))
                esc_json_str(w, v);
            jw_rawz(w, "</pre></div>\n");
        }
        break;
    case S3_BRANCH_SUMMARY:
    case S3_COMPACTION:
        jw_rawz(w, "<div class=\"msg tool\"><div class=\"role\">");
        jw_rawz(w, s->e[i].type == S3_COMPACTION ? "compaction" : "branch summary");
        jw_rawz(w, "</div><pre class=\"dim\">");
        if (json_get(o, "summary", &v))
            esc_json_str(w, v);
        jw_rawz(w, "</pre></div>\n");
        break;
    default:
        break;
    }
}

static int write_file(pi_store *st, const char *path, const jw *w, char *err, long cap)
{
    cl_sys *sys = st->s3.sys;
    if (!sys || !sys->write || sys->write(sys->u, path, w->p, w->n)) {
        cl_copy(err, "the file could not be written", cap);
        return -1;
    }
    return 0;
}

static const char *base_of(const char *p)
{
    const char *b = p, *q;
    for (q = p; *q; q++)
        if (*q == '/' || *q == ':')
            b = q + 1;
    return b;
}

int pi_export(pi_store *st, const cl_pitheme *th, const char *cwd, const char *arg, char *done, long cap, char *err, long ecap)
{
    pi_s3 *s = &st->s3;
    jw w;
    char name[300], path[600], iso[40];
    long al = arg ? (long)strlen(arg) : 0;
    int is_jsonl, *p = 0, np, i, rc = 0;
    (void)th;
    if (!s->persist) {
        cl_copy(err, "Cannot export in-memory session to HTML", ecap);
        return -1;
    }
    /* the argument: quotes around a path are taken off */
    name[0] = 0;
    if (al) {
        cl_copy(name, arg, sizeof(name));
        al = (long)strlen(name);
        if (al >= 2 && (name[0] == '"' || name[0] == '\'') && name[al - 1] == name[0]) {
            memmove(name, name + 1, (size_t)al - 2);
            name[al - 2] = 0;
        }
    }
    is_jsonl = (int)strlen(name) > 6 && !strcmp(name + strlen(name) - 6, ".jsonl");
    if (!is_jsonl && !s->flushed) {
        cl_copy(err, "Nothing to export yet - start a conversation first", ecap);
        return -1;
    }
    if (!name[0]) {
        char b[200], *dot;
        cl_copy(b, base_of(s->file), sizeof(b));
        dot = strrchr(b, '.');
        if (dot && !strcmp(dot, ".jsonl"))
            *dot = 0;
        cl_copy(name, "amiga-pi-session-", sizeof(name));
        cl_cat(name, b, sizeof(name));
        cl_cat(name, ".html", sizeof(name));
    }
    if (strchr(name, ':') || name[0] == '/')
        cl_copy(path, name, sizeof(path));
    else if (path_join(cwd, name, path, sizeof(path)))
        cl_copy(path, name, sizeof(path));
    jw_init(&w);
    np = s3_path(s, s->leaf, &p);
    if (np < 0) {
        cl_copy(err, "out of memory", ecap);
        jw_free(&w);
        return -1;
    }
    if (is_jsonl) {
        /* serializeSessionBranch: the header with the time now, the branch's entries in a chain */
        llm_ts t;
        char prev[80];
        t.sec = 0;
        t.ms = 0;
        if (s->now)
            t = s->now(s->u);
        s3_iso(t, s->utc, iso);
        jw_rawz(&w, "{\"type\":\"session\",\"version\":3,\"id\":\"");
        jw_rawz(&w, s->id);
        jw_rawz(&w, "\",\"timestamp\":\"");
        jw_rawz(&w, iso);
        jw_rawz(&w, "\",\"cwd\":");
        jw_strz(&w, s->cwd);
        jw_rawz(&w, "}\n");
        strcpy(prev, "null");
        for (i = 0; i < np; i++) {
            long n;
            const char *tx = s3_text(s, p[i], &n);
            jv o;
            char pj[100];
            if (!tx || json_parse(tx, n, &o))
                continue;
            s3_obj_put(o, &w, "parentId", prev, 0);
            jw_raw(&w, "\n", 1);
            cl_copy(pj, "\"", sizeof(pj));
            cl_cat(pj, s3_id(s, p[i]), sizeof(pj));
            cl_cat(pj, "\"", sizeof(pj));
            cl_copy(prev, pj, sizeof(prev));
        }
    } else {
        char sn[200];
        jw_rawz(&w, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>amiga-pi session ");
        if (s3_name(s, sn, sizeof(sn)) > 0)
            esc(&w, sn, (long)strlen(sn));
        else
            esc(&w, s->id, (long)strlen(s->id));
        jw_rawz(&w, "</title>\n");
        for (i = 0; style_a[i]; i++)
            jw_rawz(&w, style_a[i]);
        jw_rawz(&w, "</head><body><main>\n<h1>amiga-pi session</h1>\n<div class=\"meta\">id ");
        esc(&w, s->id, (long)strlen(s->id));
        jw_rawz(&w, " &middot; ");
        esc(&w, s->cwd, (long)strlen(s->cwd));
        jw_rawz(&w, "</div>\n");
        for (i = 0; i < np; i++)
            entry_html(st, p[i], &w);
        jw_rawz(&w, "</main></body></html>\n");
    }
    free(p);
    if (w.oom)
        rc = -1, cl_copy(err, "out of memory", ecap);
    else
        rc = write_file(st, path, &w, err, ecap);
    jw_free(&w);
    if (!rc) {
        cl_copy(done, "Session exported to: ", cap);
        cl_cat(done, path, cap);
    }
    return rc;
}
