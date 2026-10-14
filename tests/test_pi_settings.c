/* The rows of /settings (settings_pi.c) and the one writer of settings.json (authstore.c pi_store_set_path / pi_store_set_sub), P12-B12 (owner
 * 2026-10-14: every knob moves the output or carries a written exemption).
 *
 * test_table: every row of the table has a `moves` (what it changes in amiga-pi) or an `exempt` (why it cannot), never neither and never both,
 *   and the rows are pi 1.2.0's own, in its order (the 32 of the screen golden tests/pi/tui/settings_list plus the two image rows).
 * test_values: each row's shown value, its next value and its write against a settings.json in a directory of its own; the file the write
 *   leaves is pi's layout (JSON.stringify(settings, null, 2): the golden tests/pi/tui/settings_*.settings are pi's own).
 * test_paths: nested members made, replaced where they stand, removed with the object they leave empty.
 * test_session: the rows that steer the session (autocompact, the queue modes, block images, the idle timeout) reach its fields. */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pi_test.h"
#include "claude/util.h"
#include "claude/sys_posix.h"
#include "../amiga-pi/settings_pi.h"
#include "../amiga-pi/authstore.h"
#include "../amiga-pi/agent.h"
#include "../amiga-pi/store.h"
#include "../amiga-pi/scoped.h"
#include "../amiga-pi/models.h"

static char dir[400];
static sys_posix sp;
static cl_sys sys;

static void put(const char *text)
{
    char p[460];
    FILE *f;
    strcpy(p, dir);
    strcat(p, "/settings.json");
    f = fopen(p, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get(void)
{
    long n = 0;
    char *t = pi_store_read(&sys, dir, "settings.json", &n);
    return t ? t : (char *)calloc(1, 1);
}

static void test_table(void)
{
    static const char *const order[] = {
        "autocompact", "show-images", "image-width-cells", "auto-resize-images", "block-images", "skill-commands", "show-hardware-cursor",
        "editor-padding", "output-padding", "autocomplete-max-visible", "clear-on-shrink", "terminal-progress", "steering-mode", "follow-up-mode",
        "transport", "http-idle-timeout", "cache-warming-mode", "hide-thinking", "mermaid-rendering", "cache-miss-notices", "collapse-changelog",
        "quiet-startup", "install-telemetry", "default-project-trust", "double-escape-action", "tree-filter-mode", "warnings", "model-thinking",
        "tui-mode", "fullscreen-exit-output", "fullscreen-scrollbar", "fullscreen-copy-on-select", "fullscreen-wheel-scroll-lines", "theme"
    };
    int i, n = pi_settings_count(), moving = 0, exempt = 0;
    CHECK_INT(n, (int)(sizeof(order) / sizeof(order[0])));
    for (i = 0; i < n && i < (int)(sizeof(order) / sizeof(order[0])); i++) {
        const pi_setting *s = pi_settings_at(i);
        int j;
        CHECK_STR(s->id, order[i]);
        CHECK(s->label[0] && s->path[0] && s->moves && s->moves[0]);
        for (j = 0; j < i; j++)
            CHECK(strcmp(pi_settings_at(j)->id, s->id) != 0);
        if (s->exempt) {                       /* an exemption says why, in words, and the row says it moves nothing */
            CHECK(strlen(s->exempt) > 30);
            CHECK_STR(s->moves, "none");
            exempt++;
        } else {
            CHECK(strcmp(s->moves, "none") != 0 && (!strncmp(s->moves, "live:", 5) || !strncmp(s->moves, "next start:", 11)));
            moving++;
        }
        CHECK(s->kind == PSK_MENU || s->vals[0] != 0);
    }
    CHECK(moving >= 20 && exempt >= 10);
    CHECK(pi_settings_find("theme") != 0 && pi_settings_find("nonsense") == 0);
}

static void test_values(void)
{
    char shown[100], next[100], *t;
    const pi_setting *s;
    int i;
    put("");
    for (i = 0; i < pi_settings_count(); i++) {
        s = pi_settings_at(i);
        if (s->kind == PSK_MENU)
            continue;
        pi_setting_shown(s, 0, 0, shown, sizeof(shown));
        CHECK_STR(shown, s->dflt);                       /* no file: the default of settings-defaults.js */
        pi_setting_next(s, shown, next, sizeof(next));
        CHECK(strcmp(shown, next) != 0);                 /* a row always moves to another value */
    }
    /* getters of settings-manager.ts: a value outside the list is the default; the wheel row is clamped; the idle timeout is spelled */
    put("{\"tuiMode\":\"bogus\",\"fullscreenWheelScrollLines\":150,\"httpIdleTimeoutMs\":45000,\"outputPad\":3,\"quietStartup\":\"header\","
        "\"defaultProjectTrust\":\"never\"}\n");
    t = get();
    pi_setting_shown(pi_settings_find("tui-mode"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "fullscreen");
    pi_setting_shown(pi_settings_find("fullscreen-wheel-scroll-lines"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "100");
    pi_setting_shown(pi_settings_find("http-idle-timeout"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "45 sec");
    pi_setting_shown(pi_settings_find("output-padding"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "1");
    pi_setting_shown(pi_settings_find("quiet-startup"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "header");
    pi_setting_shown(pi_settings_find("default-project-trust"), t, (long)strlen(t), shown, sizeof(shown));
    CHECK_STR(shown, "Never trust");
    free(t);
    /* the wheel row's list holds the current number among 1, 2, 3, 5, 10 (SettingsSelectorComponent) */
    pi_setting_next(pi_settings_find("fullscreen-wheel-scroll-lines"), "7", next, sizeof(next));
    CHECK_STR(next, "10");
    pi_setting_next(pi_settings_find("fullscreen-wheel-scroll-lines"), "auto", next, sizeof(next));
    CHECK_STR(next, "1");
    /* writes: pi's layout, only the changed member */
    put("{\"theme\":\"dark\"}\n");
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("autocompact"), "false"), 0);
    t = get();
    CHECK_STR(t, "{\n  \"theme\": \"dark\",\n  \"compaction\": {\n    \"enabled\": false\n  }\n}");
    free(t);
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("http-idle-timeout"), "disabled"), 0);
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("quiet-startup"), "header"), 0);
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("default-project-trust"), "Always trust"), 0);
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("fullscreen-wheel-scroll-lines"), "auto"), 0);
    CHECK_INT(pi_setting_write(&sys, dir, pi_settings_find("editor-padding"), "2"), 0);
    t = get();
    CHECK(strstr(t, "\"httpIdleTimeoutMs\": 0") && strstr(t, "\"quietStartup\": \"header\"") && strstr(t, "\"defaultProjectTrust\": \"always\"") &&
          strstr(t, "\"fullscreenWheelScrollLines\": \"auto\"") && strstr(t, "\"editorPaddingX\": 2") && strstr(t, "\"enabled\": false"));
    free(t);
    /* a file that is no JSON object is not written over */
    put("not json");
    CHECK(pi_setting_write(&sys, dir, pi_settings_find("autocompact"), "false") != 0);
    t = get();
    CHECK_STR(t, "not json");
    free(t);
}

static void test_paths(void)
{
    char *t;
    put("{\"a\":1,\"terminal\":{\"clearOnShrink\":true}}\n");
    CHECK_INT(pi_store_set_path(&sys, dir, "terminal.showImages", "false"), 0);
    t = get();
    CHECK_STR(t, "{\n  \"a\": 1,\n  \"terminal\": {\n    \"clearOnShrink\": true,\n    \"showImages\": false\n  }\n}");   /* appended inside the object */
    free(t);
    CHECK_INT(pi_store_set_path(&sys, dir, "terminal.clearOnShrink", "false"), 0);
    t = get();
    CHECK(strstr(t, "\"clearOnShrink\": false,\n    \"showImages\": false") != 0);                                       /* replaced where it stands */
    free(t);
    CHECK_INT(pi_store_set_path(&sys, dir, "images.autoResize", "false"), 0);                                         /* an object made */
    t = get();
    CHECK(strstr(t, "\"images\": {\n    \"autoResize\": false\n  }") != 0);
    free(t);
    /* modelThinkingLevels["provider/model.id"]: the key holds dots and a slash; the last one removed takes the object with it */
    CHECK_INT(pi_store_set_sub(&sys, dir, "modelThinkingLevels", "ollama/qwen2.5-coder:7b", "\"low\""), 0);
    CHECK_INT(pi_store_set_sub(&sys, dir, "modelThinkingLevels", "fake/fake-model", "\"high\""), 0);
    t = get();
    CHECK(strstr(t, "\"modelThinkingLevels\": {\n    \"ollama/qwen2.5-coder:7b\": \"low\",\n    \"fake/fake-model\": \"high\"\n  }") != 0);
    free(t);
    CHECK_INT(pi_store_set_sub(&sys, dir, "modelThinkingLevels", "ollama/qwen2.5-coder:7b", 0), 0);
    CHECK_INT(pi_store_set_sub(&sys, dir, "modelThinkingLevels", "fake/fake-model", 0), 0);
    t = get();
    CHECK(strstr(t, "modelThinkingLevels") == 0);
    free(t);
    CHECK_INT(pi_set_model_level(&sys, dir, "fake", "fake-model"), -1);
    CHECK_INT(pi_store_set_sub(&sys, dir, "modelThinkingLevels", "fake/fake-model", "\"medium\""), 0);
    CHECK(pi_set_model_level(&sys, dir, "fake", "fake-model") >= 0);
    CHECK_INT(pi_set_model_level(&sys, dir, "fake", "other"), -1);
}

static pi_session ses;
static pi_store st;

static void test_session(void)
{
    memset(&ses, 0, sizeof(ses));
    memset(&st, 0, sizeof(st));
    put("{\"compaction\":{\"enabled\":false},\"steeringMode\":\"all\",\"followUpMode\":\"one-at-a-time\",\"images\":{\"blockImages\":true},"
        "\"httpIdleTimeoutMs\":60000}\n");
    pi_settings_session(&sys, dir, &ses, &st, 0);
    CHECK_INT(st.cp_enabled, 0);
    CHECK_INT(ses.steer_all, 1);
    CHECK_INT(ses.follow_all, 0);
    CHECK_INT(ses.block_images, 1);
    CHECK_INT((int)ses.idle_ms, 60000);
    /* one row at a time: the others stay */
    put("{\"steeringMode\":\"one-at-a-time\",\"followUpMode\":\"all\",\"httpIdleTimeoutMs\":0}\n");
    pi_settings_session(&sys, dir, &ses, &st, "steering-mode");
    CHECK_INT(ses.steer_all, 0);
    CHECK_INT(ses.follow_all, 0);
    pi_settings_session(&sys, dir, &ses, &st, "follow-up-mode");
    CHECK_INT(ses.follow_all, 1);
    CHECK_INT((int)ses.idle_ms, 60000);
    pi_settings_session(&sys, dir, &ses, &st, "http-idle-timeout");
    CHECK(ses.idle_ms > 1000000000L);                    /* disabled: no limit worth the name */
    put("{}\n");
    pi_settings_session(&sys, dir, &ses, &st, 0);
    CHECK_INT(st.cp_enabled, 1);
    CHECK_INT(ses.block_images, 0);
    CHECK_INT((int)ses.idle_ms, 0);                      /* the default: llm_req.idle_ms 0 is 300 s */
}

static long key_env(const char *name, char *o, long cap)
{
    const char *v = !strcmp(name, "ANTHROPIC_API_KEY") ? "sk-ant-test" : 0;
    if (!v || (long)strlen(v) >= cap)
        return -1;
    strcpy(o, v);
    return (long)strlen(v);
}

/* the scope behind /scoped-models: enabledModels is the session's scope; a default model chosen with Ctrl+S joins a scope that is not empty
 * (agent-session.ts _addPersistedDefaultToNonEmptyScope), in the session and in settings.json when it holds patterns; the list a save writes is
 * pi's (null or every model: no member) */
static void test_scope(void)
{
    static pi_models ms;
    pi_scope sc;
    const llm_model *opus = 0, *haiku = 0, *sonnet = 0;
    int i;
    char *t;
    put("{\"enabledModels\":[\"anthropic/claude-opus-5-5\"]}\n");
    pi_models_init(&ms, &sys, dir, key_env);
    for (i = 0; i < ms.reg.n; i++) {
        const llm_model *m = &ms.reg.m[i];
        if (!strcmp(m->provider, "anthropic") && !strcmp(m->id, "claude-opus-5-5"))
            opus = m;
        if (!strcmp(m->provider, "anthropic") && !strcmp(m->id, "claude-haiku-4-5"))
            haiku = m;
        if (!strcmp(m->provider, "anthropic") && !strcmp(m->id, "claude-sonnet-5-5"))
            sonnet = m;
    }
    CHECK(opus && haiku && sonnet);
    if (!opus || !haiku || !sonnet)
        return;
    pi_models_set_scope(&ms, "anthropic/claude-opus-5-5");
    CHECK_INT(pi_scope_add_default(&ms, haiku, &sys, dir), 0);
    CHECK_STR(ms.scope, "anthropic/claude-opus-5-5\nanthropic/claude-haiku-4-5");
    t = get();
    CHECK(strstr(t, "\"enabledModels\": [\n    \"anthropic/claude-opus-5-5\",\n    \"anthropic/claude-haiku-4-5\"\n  ]") != 0);
    free(t);
    CHECK_INT(pi_scope_add_default(&ms, haiku, &sys, dir), 0);                 /* already there: nothing again */
    t = get();
    CHECK(strstr(t, "claude-haiku-4-5\"\n  ]") != 0 && strstr(t, "haiku-4-5\",\n    \"anthropic/claude-haiku") == 0);
    free(t);
    /* a scope from --models alone (no member in settings.json): the session's only */
    put("{}\n");
    pi_models_set_scope(&ms, "anthropic/claude-opus-5-5");
    CHECK_INT(pi_scope_add_default(&ms, sonnet, &sys, dir), 0);
    CHECK_STR(ms.scope, "anthropic/claude-opus-5-5\nanthropic/claude-sonnet-5-5");
    t = get();
    CHECK(strstr(t, "enabledModels") == 0);
    free(t);
    /* no scope: nothing joins */
    pi_models_set_scope(&ms, "");
    CHECK_INT(pi_scope_add_default(&ms, sonnet, &sys, dir), 0);
    CHECK(ms.scope == 0 || ms.scope[0] == 0);
    /* the selector's list over this scope: the scoped models first, in the order of the scope, then the rest; a save writes the member, and
     * enabling every model removes it (pi: undefined) */
    put("{\"enabledModels\":[\"anthropic/claude-sonnet-5-5\",\"anthropic/claude-opus-5-5\"]}\n");
    pi_models_set_scope(&ms, "anthropic/claude-sonnet-5-5\nanthropic/claude-opus-5-5");
    CHECK_INT(pi_scope_open(&sc, &ms), 0);
    CHECK(!sc.en_null && sc.nen == 2 && sc.nitem > 2);
    CHECK(sc.item[0].m == sonnet && sc.item[1].m == opus && sc.item[0].enabled && !sc.item[2].enabled);
    pi_scope_move(&sc, "anthropic/claude-sonnet-5-5", 1);
    CHECK(sc.item[0].m == opus && sc.item[1].m == sonnet);
    CHECK_INT(pi_scope_save(&sc, &sys, dir), 0);
    t = get();
    CHECK(strstr(t, "\"anthropic/claude-opus-5-5\",\n    \"anthropic/claude-sonnet-5-5\"") != 0);
    free(t);
    pi_scope_enable(&sc, 0, -1);
    CHECK(sc.en_null);
    CHECK_INT(pi_scope_save(&sc, &sys, dir), 0);
    t = get();
    CHECK(strstr(t, "enabledModels") == 0);
    free(t);
    pi_scope_free(&sc);
    pi_models_free(&ms);
}

void suite_pi_settings(void)
{
    char tmpl[] = "/tmp/pi_settings_XXXXXX";
    char cmd[460];
    if (!mkdtemp(tmpl)) {
        CHECK(0);
        return;
    }
    strcpy(dir, tmpl);
    sys_posix_init(&sp, &sys);
    test_table();
    test_values();
    test_paths();
    test_session();
    test_scope();
    strcpy(cmd, "rm -rf '");
    strcat(cmd, tmpl);
    strcat(cmd, "'");
    if (system(cmd))
        CHECK(0);
}
