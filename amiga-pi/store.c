/* see store.h */
#include <stdlib.h>
#include <string.h>
#include "store.h"
#include "compact.h"
#include "cp_prompts.h"
#include "claude/util.h"
#include "llm/estimate.h"

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
    st->keep = CP_KEEP;
    st->reserve = CP_RESERVE;
    st->cp_enabled = 1;
}

void pi_store_free(pi_store *st)
{
    if (st->s && st->s->compact_u == st) {
        st->s->compact = 0;
        st->s->compact_u = 0;
        st->s->declare = 0;
        st->s->declare_u = 0;
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

int pi_store_level(pi_store *st, char *out, long cap)
{
    pi_ctx c;
    int ok;
    ctx_init(&c);
    if (ctx_build(&st->s3, st->s3.leaf, &c))
        return 0;
    ok = c.has_thinking;
    if (ok)
        cl_copy(out, c.thinking, cap);
    ctx_free(&c);
    return ok;
}

int pi_store_model_change(pi_store *st, const char *provider, const char *id)
{
    return s3_model(&st->s3, provider, id) < 0 ? -1 : 0;
}

int pi_store_thinking(pi_store *st, const char *level)
{
    return s3_thinking(&st->s3, level) < 0 ? -1 : 0;
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
    /* declarationsEqual compares the whole declaration, constrainedSampling included */
    if (json_get(a, "constrainedSampling", &x) != json_get(b, "constrainedSampling", &y))
        return 0;
    if (json_get(a, "constrainedSampling", &x) && json_get(b, "constrainedSampling", &y) &&
        (x.n != y.n || memcmp(x.p, y.p, (size_t)x.n)))
        return 0;
    return 1;
}

/* the system message to append now, built into out; 0 nothing to add, 1 appended into out, -1 */
static int sys_patch(pi_store *st, const pi_psec *cur, const char *tools_json, long tools_n, const pi_ctx *c, jw *out,
                     int sections)
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
    if (!have_prev && !sections) {
        goto done;                      /* tools only, nothing declared yet */
    }
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
    for (i = 0; sections && i < cur->n; i++) {
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
    if (sections && json_get(pm, "sections", &ps) && json_type(ps) == J_OBJ) {
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
        rc = sys_patch(st, &cur, tools.p, tools.n, &c, &msg, 1);
        if (rc == 1) {
            if (s3_message(&st->s3, msg.p, msg.n) >= 0 && !pi_session_push_system(st->s, msg.p, msg.n)) {
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

/* pi's agent-loop declareToolChanges, before each request to a model that takes tool changes in the transcript: the
 * tools the agent can run against the tools the transcript declares, the difference appended as a system message */
static void declare_tools(void *u, pi_session *s)
{
    pi_store *st = (pi_store *)u;
    jw tools, msg;
    pi_ctx c;
    jw_init(&tools);
    jw_init(&msg);
    pi_tools_json(s, &tools);
    ctx_init(&c);
    if (!ctx_build(&st->s3, st->s3.leaf, &c) && sys_patch(st, 0, tools.p, tools.n, &c, &msg, 0) == 1 &&
        s3_message(&st->s3, msg.p, msg.n) >= 0 && !pi_session_push_system(s, msg.p, msg.n)) {
        st->syncs++;
        if (!replay(st))
            s->system = st->system.p;
    }
    ctx_free(&c);
    jw_free(&tools);
    jw_free(&msg);
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
            if (pi_session_push_system(st->s, c.msg[i], c.msg_n[i]))
                rc = -1;
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

/* ---- compaction ---- */

static void cp_event(pi_store *st, int type, int reason, int is_error, const char *text)
{
    pi_event e;
    pi_event_init(&e, type);
    e.stop = reason;
    e.is_error = is_error;
    e.msg = text;
    e.msg_n = text ? (long)strlen(text) : 0;
    pi_emit(&st->s->em, &e);
}

/* one summarization request to the session's model: the answer's pieces in r */
int pi_store_complete(void *u, const char *prompt, long max_tokens, cp_reply *r)
{
    pi_store *st = (pi_store *)u;
    pi_session *s = st->s;
    llm_acc acc;
    llm_sink k;
    llm_ctx c;
    llm_req q;
    jw blocks, um, msg, sys, hdr;
    const char *msgs[1];
    long mn[1];
    llm_info inf;
    llm_ts ts = store_now(st);
    jv m, v, b, t, x;
    jit it;
    int any = 0;
    jw_init(&blocks);
    jw_init(&um);
    jw_init(&msg);
    jw_init(&sys);
    jw_rawz(&blocks, "[");
    llm_b_text(&blocks, prompt, (long)strlen(prompt), 0);
    jw_rawz(&blocks, "]");
    llm_m_user(&um, blocks.p, blocks.n, ts);
    {
        const char *const *l;
        for (l = cp_summarization_system_prompt; *l; l++)
            jw_rawz(&sys, *l);
    }
    msgs[0] = um.p;
    mn[0] = um.n;
    memset(&c, 0, sizeof(c));
    {
        /* pi's completeSimple normalizes the context: the prompt becomes the leading system message, which
         * buildBaseOptions' estimate counts */
        jw sm;
        const char *est_m[2];
        long est_n[2];
        jw_init(&sm);
        jw_rawz(&sm, "{\"role\":\"system\",\"content\":");
        jw_strz(&sm, sys.p ? sys.p : "");
        jw_rawz(&sm, ",\"timestamp\":0}");
        est_m[0] = sm.p;
        est_n[0] = sm.n;
        est_m[1] = um.p;
        est_n[1] = um.n;
        c.est_tokens = llm_estimate_context(est_m, est_n, 2);
        jw_free(&sm);
    }
    c.model = &s->model;
    c.system = sys.p;
    c.msgs = msgs;
    c.msg_n = mn;
    c.nmsg = 1;
    c.max_tokens = max_tokens;
    c.thinking = s->thinking;
    llm_acc_init(&acc, s->model.api, s->model.provider);
    llm_acc_model(&acc, s->model.id, s->model.cost);
    k.u = &acc;
    k.ev = llm_acc_event;
    memset(&q, 0, sizeof(q));
    q.pool = s->pool;
    q.model = &s->model;
    q.be = llm_backend_of(s->model.api);
    q.ctx = &c;
    q.user_agent = s->user_agent;
    q.h = s->h;
    q.sink = &k;
    jw_init(&hdr);
    if (pi_req_auth(s, &q, &hdr)) {
        llm_event e;
        char why[256];
        cl_copy(why, s->err, sizeof(why));
        llm_event_init(&e, LE_ERROR);
        e.stop = LS_ERROR;
        e.err_type = "error";
        e.p = why;
        e.n = (long)strlen(why);
        llm_acc_event(&acc, &e);
    } else if (!q.be) {
        llm_event e;
        llm_event_init(&e, LE_ERROR);
        e.stop = LS_ERROR;
        e.err_type = "error";
        e.p = "No API provider registered for this model's api";
        e.n = (long)strlen(e.p);
        llm_acc_event(&acc, &e);
    } else
        llm_request(&q);
    jw_free(&hdr);
    llm_acc_message(&acc, ts, &msg);
    r->stop = LS_NONE;
    if (!msg.oom && msg.p && llm_msg_info(msg.p, msg.n, &inf) >= 0 && !json_parse(msg.p, msg.n, &m)) {
        r->stop = inf.stop;
        r->tool_call = inf.ntools > 0;
        if (json_get(m, "usage", &v))
            llm_r_usage(v, &r->usage);
        if (json_get(m, "errorMessage", &v) && json_type(v) == J_STR)
            json_str(v, r->error, sizeof(r->error));
        if (json_get(m, "content", &v) && json_type(v) == J_ARR) {
            json_iter(v, &it);
            while (json_next(&it, 0, &b))
                if (json_get(b, "type", &t) && json_streq(t, "text") && json_get(b, "text", &x)) {
                    long zn;
                    char *z = json_strdup(x, &zn);
                    if (any++)
                        jw_rawz(&r->text, "\n");
                    if (z)
                        jw_raw(&r->text, z, zn);
                    free(z);
                }
        }
    } else {
        r->stop = LS_ERROR;
        cl_copy(r->error, "out of memory", sizeof(r->error));
    }
    llm_acc_free(&acc);
    jw_free(&blocks);
    jw_free(&um);
    jw_free(&msg);
    jw_free(&sys);
    return 0;
}

/* the model's messages for the branch loaded again and the replay refreshed (after a compaction or a tree move) */
int pi_store_reload(pi_store *st)
{
    if (load_messages(st) || replay(st))
        return -1;
    st->s->system = st->system.n ? st->system.p : st->s->system;
    return 0;
}

static int run_compaction(pi_store *st, const char *instr, int reason)
{
    pi_session *s = st->s;
    pi_prep p;
    cp_result res;
    pi_ctx c;
    jw usage, sysm, info;
    int rc, ok = -1, i;
    long after = 0;
    cp_event(st, PE_COMPACTION_START, reason, 0, 0);
    st->err[0] = 0;
    rc = cp_prepare(&st->s3, st->s3.leaf, st->keep, &p);
    if (rc < 0) {
        cl_copy(st->err, "out of memory", sizeof(st->err));
    } else if (rc == 0) {
        cl_copy(st->err, "Compaction failed: ", sizeof(st->err));
        cl_cat(st->err, st->s3.leaf >= 0 && st->s3.e[st->s3.leaf].type == S3_COMPACTION
                            ? "Already compacted"
                            : "Nothing to compact (session too small)",
               sizeof(st->err));
    } else {
        char err[200];
        if (cp_compact(&p, st->reserve, s->model.max_tokens, instr, pi_store_complete, st, &res, err, sizeof(err))) {
            if (s->h.poll && s->h.poll(s->h.u))
                cl_copy(st->err, "Compaction cancelled", sizeof(st->err));
            else {
                cl_copy(st->err, "Compaction failed: ", sizeof(st->err));
                cl_cat(st->err, err, sizeof(st->err));
            }
        } else {
            jw_init(&usage);
            jw_init(&sysm);
            llm_w_usage(&usage, &res.usage);
            ctx_init(&c);
            if (!ctx_build(&st->s3, st->s3.leaf, &c)) {
                ctx_system(&c, &sysm);
                if (s3_compaction(&st->s3, res.summary.p, p.first_kept, res.tokens_before, res.details.p, usage.p, 0,
                                  sysm.n ? sysm.p : 0) >= 0)
                    ok = 0;
                ctx_free(&c);
            }
            jw_free(&usage);
            jw_free(&sysm);
            if (ok) {
                cl_copy(st->err, st->s3.err[0] ? st->s3.err : "the compaction could not be recorded", sizeof(st->err));
            } else {
                st->compactions++;
                if (pi_store_reload(st)) {
                    cl_copy(st->err, "the session could not be reloaded", sizeof(st->err));
                    ok = -1;
                } else {
                    ctx_init(&c);
                    if (!ctx_build(&st->s3, st->s3.leaf, &c)) {
                        for (i = 0; i < c.n; i++)
                            after += cp_estimate(c.msg[i], c.msg_n[i]);
                        ctx_free(&c);
                    }
                    jw_init(&info);
                    jw_rawz(&info, "Compacted: about ");
                    jw_long(&info, res.tokens_before);
                    jw_rawz(&info, " tokens before, ");
                    jw_long(&info, after);
                    jw_rawz(&info, " after");
                    cp_event(st, PE_COMPACTION_END, reason, 0, info.p);
                    jw_free(&info);
                }
            }
            cp_result_free(&res);
        }
        cp_prep_free(&p);
    }
    if (st->err[0] && ok) {
        cp_event(st, PE_COMPACTION_END, reason, 1, st->err);
        return -1;
    }
    return ok ? -1 : 0;
}

int pi_store_compact(pi_store *st, const char *instructions)
{
    return run_compaction(st, instructions, 0);
}

void pi_store_settings(pi_store *st, const char *text, long n)
{
    jv o, c, v;
    if (json_parse(text, n, &o) || !json_get(o, "compaction", &c) || json_type(c) != J_OBJ)
        return;
    if (json_get(c, "enabled", &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
        st->cp_enabled = json_type(v) == J_TRUE;
    if (json_get(c, "reserveTokens", &v) && json_type(v) == J_NUM)
        st->reserve = json_long(v, CP_RESERVE);
    if (json_get(c, "keepRecentTokens", &v) && json_type(v) == J_NUM)
        st->keep = json_long(v, CP_KEEP);
}

static int ts_le(llm_ts a, llm_ts b)
{
    return a.sec < b.sec || (a.sec == b.sec && a.ms <= b.ms);
}

/* the time of the latest compaction on the branch: 1, 0 none (or no time pi can read) */
static int compaction_time(pi_store *st, llm_ts *t)
{
    int j;
    for (j = st->s3.leaf; j >= 0; j = st->s3.e[j].parent)
        if (st->s3.e[j].type == S3_COMPACTION) {
            long n;
            const char *x = s3_text(&st->s3, j, &n);
            jv o, v;
            char iso[64];
            if (!x || json_parse(x, n, &o) || !json_get(o, "timestamp", &v) || json_type(v) != J_STR ||
                json_str(v, iso, sizeof(iso)) < 0)
                return 0;
            return !s3_iso_read(iso, t);
        }
    return 0;
}

/* pi's _checkCompaction (threshold case) for the last answer: the context's tokens, -1 when pi does not check */
static long answer_tokens(pi_store *st, pi_session *s, int when)
{
    int a, i, last, edits = 0, has_ct;
    llm_info inf;
    llm_ts ct;
    pi_ctx c;
    long tokens, direct;
    for (a = s->nmsg - 1; a >= 0; a--)
        if (llm_msg_info(s->msg[a], s->msg_n[a], &inf) >= 0 && inf.role == LR_ASSISTANT)
            break;
    /* after the run an aborted answer is not checked; before a prompt it is */
    if (a < 0 || (when == PI_CP_END && inf.stop == LS_ABORTED))
        return -1;
    /* an answer from before the latest compaction: its usage is stale */
    has_ct = compaction_time(st, &ct);
    if (has_ct && ts_le(inf.ts, ct))
        return -1;
    ctx_init(&c);
    if (ctx_build(&st->s3, st->s3.leaf, &c))
        return -1;
    for (i = 0; i < c.ne && !edits; i++)
        edits = st->s3.e[c.esrc[i]].type == S3_CONTEXT_EDIT;
    direct = cp_usage_total(s->msg[a], s->msg_n[a]);
    if (edits)
        tokens = cp_context_tokens(&st->s3, st->s3.leaf, &c);
    else if (inf.stop == LS_ERROR || !direct) {
        /* the last usable usage plus an estimate of the rest, unless that usage is from before a compaction */
        llm_info li;
        tokens = cp_estimate_context(s->msg, s->msg_n, s->nmsg, &last);
        if (last >= 0 && has_ct && llm_msg_info(s->msg[last], s->msg_n[last], &li) >= 0 && ts_le(li.ts, ct))
            tokens = -1;
    } else
        tokens = direct;
    ctx_free(&c);
    return tokens;
}

/* threshold compaction at pi's points (PI_CP_*): before a prompt and after a run on the last answer's usage,
 * between turns on the projected estimate; a window of 0 (unknown) never compacts by itself. Overflow recovery
 * (compact and retry) is not built. A failure is shown by its event; the prompt goes on */
static void compact_check(void *u, pi_session *s, int when)
{
    pi_store *st = (pi_store *)u;
    pi_ctx c;
    long tokens;
    if (!st->cp_enabled || s->model.context_window <= 0)
        return;
    if (when == PI_CP_TURN) {
        ctx_init(&c);
        if (ctx_build(&st->s3, st->s3.leaf, &c))
            return;
        tokens = cp_context_tokens(&st->s3, st->s3.leaf, &c);
        ctx_free(&c);
    } else
        tokens = answer_tokens(st, s, when);
    if (tokens >= 0 && cp_should(tokens, s->model.context_window, st->reserve))
        run_compaction(st, 0, 1);
}

/* ---- slash commands ---- */

static int command(void *u, pi_session *s, const char *line, jw *reply, jw *editor)
{
    pi_store *st = (pi_store *)u;
    (void)s;
    if (!strncmp(line, "/compact", 8) && (line[8] == 0 || line[8] == ' ')) {
        const char *instr = line + 8;
        while (*instr == ' ')
            instr++;
        run_compaction(st, *instr ? instr : 0, 0);      /* its events tell how it went */
        (void)reply;
        return 1;
    }
    return pi_cmd(st, line, reply, editor);
}

int pi_store_attach(pi_store *st, pi_session *s)
{
    st->s = s;
    if (!st->resumed) {
        /* a new session records what it starts with, so it can be restored on resume (sdk.ts) */
        if (s3_model(&st->s3, s->model.provider, s->model.id) < 0 || s3_thinking(&st->s3, llm_think_name(s->thinking)) < 0)
            return -1;
    } else {
        /* an older session without a level: pi records the one it starts with (sdk.ts) */
        char lv[32];
        if (!pi_store_level(st, lv, sizeof(lv)) && s3_thinking(&st->s3, llm_think_name(s->thinking)) < 0)
            return -1;
    }
    if (load_messages(st))
        return -1;
    if (replay(st))
        return -1;
    if (st->system.n)
        s->system = st->system.p;
    s->session_id = st->s3.id[0] ? st->s3.id : 0;
    s->compact = compact_check;
    s->compact_u = st;
    s->declare = declare_tools;
    s->declare_u = st;
    s->command = command;
    s->command_u = st;
    return pi_subscribe(&s->em, on_event, st);
}
