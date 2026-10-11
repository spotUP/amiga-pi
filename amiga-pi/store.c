/* see store.h */
#include <stdlib.h>
#include <string.h>
#include "store.h"
#include "claude/util.h"

static llm_ts store_now(pi_store *st)
{
    llm_ts t;
    if (st->s && st->s->now)
        return st->s->now(st->s->h.u);
    if (st->s3.now)
        return st->s3.now(st->s3.u);
    t.sec = 0;
    t.ms = 0;
    return t;
}

void pi_store_init(pi_store *st, cl_sys *sys, llm_ts (*now)(void *u), void *u, int utc)
{
    llm_ts t;
    unsigned long seed;
    memset(st, 0, sizeof(*st));
    st->sys = sys;
    t.sec = 0;
    t.ms = 0;
    if (now)
        t = now(u);
    seed = (unsigned long)t.sec * 1000UL + (unsigned long)t.ms;
    seed ^= (unsigned long)(size_t)st << 4;
    s3_init(&st->s3, sys, now, u, utc, seed ? seed : 1UL);
    jw_init(&st->system);
}

void pi_store_free(pi_store *st)
{
    if (st->s && st->s->prepare_u == st) {
        st->s->prepare = 0;
        st->s->prepare_u = 0;
    }
    jw_free(&st->system);
    s3_free(&st->s3);
}

int pi_store_open(pi_store *st, int mode, const char *data_dir, const char *cwd, const char *arg)
{
    char dir[300], file[340];
    st->mode = mode;
    cl_copy(st->data_dir, data_dir ? data_dir : "", sizeof(st->data_dir));
    cl_copy(st->cwd, cwd ? cwd : "", sizeof(st->cwd));
    st->err[0] = 0;
    if (mode == PS_MODE_NONE || !st->data_dir[0])
        return s3_new(&st->s3, "", st->cwd, 0, 0, 0);
    if (s3_dir(st->data_dir, st->cwd, dir, sizeof(dir))) {
        cl_copy(st->err, "the session directory name is too long", sizeof(st->err));
        return -1;
    }
    if (mode == PS_MODE_CONTINUE) {
        if (!s3_recent(st->sys, dir, file, sizeof(file))) {
            st->resumed = 1;
            if (s3_open(&st->s3, file)) {
                cl_copy(st->err, st->s3.err, sizeof(st->err));
                return -1;
            }
            return 0;
        }
        return s3_new(&st->s3, dir, st->cwd, 0, 0, 1);
    }
    if (mode == PS_MODE_SESSION) {
        /* a path (it exists, or holds a separator), else an id prefix in this directory */
        if (arg && st->sys->kind(st->sys->u, arg) == 1)
            cl_copy(file, arg, sizeof(file));
        else if (!arg || !arg[0] || s3_by_id(st->sys, dir, arg, file, sizeof(file))) {
            if (arg && (strchr(arg, '/') || strchr(arg, ':'))) {
                /* pi: a path that does not exist is a new session kept there */
                cl_copy(file, arg, sizeof(file));
            } else {
                cl_copy(st->err, "no session matches ", sizeof(st->err));
                cl_cat(st->err, arg ? arg : "", sizeof(st->err));
                return -1;
            }
        }
        st->resumed = st->sys->kind(st->sys->u, file) == 1;
        cl_copy(st->s3.cwd, st->cwd, sizeof(st->s3.cwd));
        if (s3_open(&st->s3, file)) {
            cl_copy(st->err, st->s3.err, sizeof(st->err));
            return -1;
        }
        return 0;
    }
    return s3_new(&st->s3, dir, st->cwd, 0, 0, 1);
}

int pi_store_model(pi_store *st, char *provider, long pcap, char *id, long icap)
{
    pi_ctx c;
    int ok;
    ctx_init(&c);
    if (ctx_build(&st->s3, st->s3.leaf, &c))
        return 0;
    ok = c.has_model && c.provider[0] && c.model[0];
    if (ok) {
        cl_copy(provider, c.provider, pcap);
        cl_copy(id, c.model, icap);
    }
    ctx_free(&c);
    return ok;
}

/* ---- the system message ---- */

/* previous sections of the replayed message: name -> text, in order (strings in sec[], names in nm[]) */
#define SYS_MAX 16

static int same_decl(jv a, jv b)
{
    jv x, y;
    if (!json_get(a, "description", &x) != !json_get(b, "description", &y))
        return 0;
    if (json_get(a, "description", &x) && json_get(b, "description", &y) && (x.n != y.n || memcmp(x.p, y.p, (size_t)x.n)))
        return 0;
    if (json_get(a, "parameters", &x) != json_get(b, "parameters", &y))
        return 0;
    if (json_get(a, "parameters", &x) && json_get(b, "parameters", &y) && (x.n != y.n || memcmp(x.p, y.p, (size_t)x.n)))
        return 0;
    return 1;
}

/* the system message to append now, built into out; 0 nothing to add, 1 appended into out, -1 */
static int sys_patch(pi_store *st, const pi_psec *cur, const char *tools_json, long tools_n, const pi_ctx *c, jw *out)
{
    jw prev, secs, added, removed;
    jv pm, ps, pt, v, k, val, ct;
    jit it, it2;
    int have_prev, any_sec = 0, any_add = 0, any_rem = 0, i, rc = 0;
    llm_ts ts = store_now(st);
    jw_init(&prev);
    have_prev = ctx_system(c, &prev) == 1;
    jw_init(&secs);
    jw_init(&added);
    jw_init(&removed);
    if (!have_prev) {
        /* the first request: every section and every tool */
        jw_rawz(&secs, "{");
        for (i = 0; i < cur->n; i++) {
            if (i)
                jw_rawz(&secs, ",");
            jw_strz(&secs, cur->name[i]);
            jw_rawz(&secs, ":");
            jw_str(&secs, cur->text.p + cur->off[i], cur->len[i]);
        }
        jw_rawz(&secs, "}");
        llm_m_system_ex(out, "", 0, secs.p, secs.n, tools_n > 2 ? tools_json : 0, tools_n > 2 ? tools_n : 0, 0, 0, ts);
        rc = 1;
        goto done;
    }
    json_parse(prev.p, prev.n, &pm);
    /* diffSystemPromptSections */
    jw_rawz(&secs, "{");
    for (i = 0; i < cur->n; i++) {
        int same = 0;
        if (json_get(pm, "sections", &ps) && json_get(ps, cur->name[i], &v) && json_type(v) == J_STR) {
            char *z;
            long zn;
            z = json_strdup(v, &zn);
            same = z && zn == cur->len[i] && !memcmp(z, cur->text.p + cur->off[i], (size_t)zn);
            free(z);
        }
        if (!same) {
            if (any_sec)
                jw_rawz(&secs, ",");
            any_sec = 1;
            jw_strz(&secs, cur->name[i]);
            jw_rawz(&secs, ":");
            jw_str(&secs, cur->text.p + cur->off[i], cur->len[i]);
        }
    }
    if (json_get(pm, "sections", &ps) && json_type(ps) == J_OBJ) {
        json_iter(ps, &it);
        while (json_next(&it, &k, &val)) {
            char *nm = json_strdup(k, 0);
            int found = 0;
            for (i = 0; nm && i < cur->n; i++)
                found |= !strcmp(nm, cur->name[i]);
            if (nm && !found) {
                if (any_sec)
                    jw_rawz(&secs, ",");
                any_sec = 1;
                jw_strz(&secs, nm);
                jw_rawz(&secs, ":null");
            }
            free(nm);
        }
    }
    jw_rawz(&secs, "}");
    /* getToolStateChanges: a changed definition is a removal followed by an addition */
    jw_rawz(&added, "[");
    jw_rawz(&removed, "[");
    if (!json_parse(tools_json, tools_n, &ct)) {
        json_iter(ct, &it);
        while (json_next(&it, 0, &val)) {
            char *nm = json_get(val, "name", &v) ? json_strdup(v, 0) : 0;
            int same = 0;
            if (nm && json_get(pm, "toolsAdded", &pt) && json_type(pt) == J_ARR) {
                jv pv, pn;
                json_iter(pt, &it2);
                while (json_next(&it2, 0, &pv))
                    if (json_get(pv, "name", &pn) && json_streq(pn, nm))
                        same = same_decl(pv, val);
            }
            if (nm && !same) {
                if (any_add)
                    jw_rawz(&added, ",");
                any_add = 1;
                jw_raw(&added, val.p, val.n);
            }
            free(nm);
        }
    }
    if (json_get(pm, "toolsAdded", &pt) && json_type(pt) == J_ARR) {
        jv pv, pn;
        json_iter(pt, &it2);
        while (json_next(&it2, 0, &pv)) {
            char *nm = json_get(pv, "name", &pn) ? json_strdup(pn, 0) : 0;
            int same = 0;
            if (nm && !json_parse(tools_json, tools_n, &ct)) {
                jv cv, cn;
                json_iter(ct, &it);
                while (json_next(&it, 0, &cv))
                    if (json_get(cv, "name", &cn) && json_streq(cn, nm))
                        same = same_decl(pv, cv);
            }
            if (nm && !same) {
                if (any_rem)
                    jw_rawz(&removed, ",");
                any_rem = 1;
                jw_rawz(&removed, "{\"name\":");
                jw_strz(&removed, nm);
                jw_rawz(&removed, "}");
            }
            free(nm);
        }
    }
    jw_rawz(&added, "]");
    jw_rawz(&removed, "]");
    if (any_sec || any_add || any_rem) {
        llm_m_system_ex(out, "", 0, any_sec ? secs.p : 0, any_sec ? secs.n : 0, any_add ? added.p : 0,
                        any_add ? added.n : 0, any_rem ? removed.p : 0, any_rem ? removed.n : 0, ts);
        rc = 1;
    }
done:
    jw_free(&prev);
    jw_free(&secs);
    jw_free(&added);
    jw_free(&removed);
    return out->oom ? -1 : rc;
}

/* the replayed system prompt into st->system */
static int replay(pi_store *st)
{
    pi_ctx c;
    jw m;
    int rc = 0;
    ctx_init(&c);
    jw_reset(&st->system);
    if (ctx_build(&st->s3, st->s3.leaf, &c))
        return -1;
    jw_init(&m);
    if (ctx_system(&c, &m) == 1)
        rc = ctx_system_text(m.p, m.n, &st->system);
    jw_free(&m);
    ctx_free(&c);
    return rc;
}

/* the live prompt and tools against the transcript's: the patch appended, st->system the replay */
static int sync_system(pi_store *st)
{
    pi_psec cur;
    jw tools, msg;
    pi_ctx c;
    int rc, ok = -1;
    if (pi_prompt_sections(st->s, st->cwd, &cur)) {
        pi_psec_free(&cur);
        return -1;
    }
    jw_init(&tools);
    jw_init(&msg);
    pi_tools_json(st->s, &tools);
    ctx_init(&c);
    if (!ctx_build(&st->s3, st->s3.leaf, &c)) {
        rc = sys_patch(st, &cur, tools.p, tools.n, &c, &msg);
        if (rc == 1) {
            if (s3_message(&st->s3, msg.p, msg.n) >= 0) {
                st->syncs++;
                ok = 0;
            }
        } else if (rc == 0)
            ok = 0;
    }
    ctx_free(&c);
    if (!ok)
        ok = replay(st);
    jw_free(&tools);
    jw_free(&msg);
    pi_psec_free(&cur);
    st->s->system = st->system.p;
    return ok;
}

static void on_event(void *u, const pi_event *e)
{
    pi_store *st = (pi_store *)u;
    if (st->loading)
        return;
    if (e->type == PE_AGENT_START) {
        if (sync_system(st))
            cl_copy(st->err, "the system prompt could not be recorded", sizeof(st->err));
    } else if (e->type == PE_MESSAGE_END && e->msg && (e->role == LR_USER || e->role == LR_ASSISTANT || e->role == LR_TOOLRESULT)) {
        if (s3_message(&st->s3, e->msg, e->msg_n) >= 0)
            st->persisted++;
        else
            cl_copy(st->err, st->s3.err[0] ? st->s3.err : "the session could not be written", sizeof(st->err));
    }
}

/* the agent holds the branch's messages: from the context, system messages left to the replay */
static int load_messages(pi_store *st)
{
    pi_ctx c;
    int i, rc = 0;
    ctx_init(&c);
    pi_session_clear(st->s);
    if (ctx_build(&st->s3, st->s3.leaf, &c))
        return -1;
    st->loading = 1;
    for (i = 0; i < c.n && !rc; i++) {
        jw one;
        jv m, r;
        jw_init(&one);
        if (!json_parse(c.msg[i], c.msg_n[i], &m) && json_get(m, "role", &r) && json_streq(r, "system")) {
            jw_free(&one);
            continue;
        }
        if (ctx_msg_to_llm(c.msg[i], c.msg_n[i], &one) && pi_session_push(st->s, one.p, one.n))
            rc = -1;
        jw_free(&one);
    }
    st->loading = 0;
    ctx_free(&c);
    return rc;
}

static int prepare(void *u, pi_session *s)
{
    (void)u;
    (void)s;
    return 0;
}

int pi_store_attach(pi_store *st, pi_session *s)
{
    st->s = s;
    if (!st->resumed) {
        /* a new session records what it starts with, so it can be restored on resume (sdk.ts) */
        if (s3_model(&st->s3, s->model.provider, s->model.id) < 0 || s3_thinking(&st->s3, "off") < 0)
            return -1;
    }
    if (load_messages(st))
        return -1;
    if (replay(st))
        return -1;
    if (st->system.n)
        s->system = st->system.p;
    s->prepare = prepare;
    s->prepare_u = st;
    return pi_subscribe(&s->em, on_event, st);
}
