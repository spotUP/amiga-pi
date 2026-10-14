/* see settings_pi.h */
#include <stdlib.h>
#include <string.h>
#include "settings_pi.h"
#include "authstore.h"
#include "agent.h"
#include "image.h"
#include "store.h"
#include "claude/json.h"
#include "claude/util.h"

#define NOEX 0

/* pi 1.2.0 settings-selector.ts, in the order its list shows them with a terminal that draws no images (the two image rows come after
 * autocompact where pi puts them; ui_pi.c leaves them out unless the terminal draws images). Defaults: core/settings-defaults.js */
static const pi_setting tab[] = {
    { "autocompact", "Auto-compact", "Automatically compact context when it gets too large", "compaction.enabled", PSK_BOOL,
      { "true", "false" }, "true", 0, "live: the agent compacts the context when it nears the window (store.c cp_enabled) or never does", NOEX },
    { "show-images", "Show images", "Render images inline in terminal", "terminal.showImages", PSK_BOOL,
      { "true", "false" }, "true", PSF_IMAGES, "none", "the Amiga terminal draws no images: an image is the text placeholder either way. The row is listed only "
      "where PI_IMAGE_PROTOCOL says the terminal draws images (pi's own override), and then it only records the value" },
    { "image-width-cells", "Image width", "Preferred inline image width in terminal cells", "terminal.imageWidthCells", PSK_NUM,
      { "60", "80", "120" }, "60", PSF_IMAGES, "none", "no inline images are drawn, so there is no width to prefer" },
    { "auto-resize-images", "Auto-resize images", "Resize large images to 2000x2000 max for better model compatibility", "images.autoResize", PSK_BOOL,
      { "true", "false" }, "true", 0, "live: read sends an image resized to 2000x2000 at most, or as it is", NOEX },
    { "block-images", "Block images", "Prevent images from being sent to LLM providers", "images.blockImages", PSK_BOOL,
      { "true", "false" }, "false", 0, "live: an image in a request is the text \"Image reading is disabled.\"", NOEX },
    { "skill-commands", "Skill commands", "Register skills as /skill:name commands", "enableSkillCommands", PSK_BOOL,
      { "true", "false" }, "true", 0, "live: the /skill:name rows of the slash menu", NOEX },
    { "show-hardware-cursor", "Show hardware cursor", "Use the terminal cursor instead of Pi's drawn cursor", "showHardwareCursor", PSK_BOOL,
      { "true", "false" }, "false", 0, "live: the editor's cursor is the window's cursor", NOEX },
    { "editor-padding", "Editor padding", "Horizontal padding for input editor (0-3)", "editorPaddingX", PSK_NUM,
      { "0", "1", "2", "3" }, "0", 0, "live: the editor's and the slash menu's side padding", NOEX },
    { "output-padding", "Output padding", "Horizontal padding for messages, tool output, and command output", "outputPad", PSK_NUM,
      { "0", "1" }, "1", 0, "next start: the transcript's side padding", NOEX },
    { "autocomplete-max-visible", "Autocomplete max items", "Max visible items in autocomplete dropdown (3-20)", "autocompleteMaxVisible", PSK_NUM,
      { "3", "5", "7", "10", "15", "20" }, "5", 0, "live: the rows of the slash menu", NOEX },
    { "clear-on-shrink", "Clear on shrink", "Clear empty rows when content shrinks (may cause flicker)", "terminal.clearOnShrink", PSK_BOOL,
      { "true", "false" }, "false", 0, "none", "pi's renderer keeps the rows below a shrinking transcript until this clears them; amiga-pi's screen "
      "redraws the whole footer area every frame, so it has no stale rows to clear" },
    { "terminal-progress", "Terminal progress", "Show OSC 9;4 progress indicators in the terminal tab bar", "terminal.showTerminalProgress", PSK_BOOL,
      { "true", "false" }, "false", 0, "none", "the OSC 9;4 progress report goes to a terminal's tab bar or taskbar; UP-Term's windows have neither" },
    { "steering-mode", "Steering mode", "Enter while streaming queues steering messages. 'one-at-a-time': deliver one, wait for response. 'all': deliver all at once.",
      "steeringMode", PSK_ENUM, { "one-at-a-time", "all" }, "one-at-a-time", 0, "live: how many queued steering messages the agent takes at once", NOEX },
    { "follow-up-mode", "Follow-up mode", "%F queues follow-up messages until agent stops. 'one-at-a-time': deliver one, wait for response. 'all': deliver all at once.",
      "followUpMode", PSK_ENUM, { "one-at-a-time", "all" }, "one-at-a-time", 0, "live: how many queued follow-ups the agent takes at once", NOEX },
    { "transport", "Transport", "Preferred transport for providers that support multiple transports", "transport", PSK_ENUM,
      { "sse", "websocket", "websocket-cached", "auto" }, "auto", 0, "none", "amiga-pi speaks server-sent events only, so every value sends the same request" },
    { "http-idle-timeout", "HTTP idle timeout", "Maximum idle gap while waiting for HTTP headers or body chunks. Disable for local models that pause longer than five minutes.",
      "httpIdleTimeoutMs", PSK_IDLE, { "30 sec", "1 min", "2 min", "5 min", "disabled" }, "5 min", 0, "live: how long a request waits for the next bytes before it fails", NOEX },
    { "cache-warming-mode", "Cache warming", "off; streaming while the agent runs; idle also between runs while continuation stays profitable", "cacheWarming", PSK_ENUM,
      { "off", "streaming", "idle" }, "streaming", 0, "none", "amiga-pi sends no cache-warming requests in any mode" },
    { "hide-thinking", "Hide thinking", "Hide thinking blocks in assistant responses", "hideThinkingBlock", PSK_BOOL,
      { "true", "false" }, "false", 0, "none", "amiga-pi draws no thinking blocks (the reasoning is sent and kept in the session file, not shown), so there is nothing to hide" },
    { "mermaid-rendering", "Mermaid diagrams", "Render Mermaid code blocks as Unicode diagrams", "markdown.mermaid", PSK_ENUM,
      { "off", "final", "streaming" }, "streaming", 0, "none", "amiga-pi has no Mermaid renderer: a Mermaid block is a code block in every mode" },
    { "cache-miss-notices", "Cache miss notices", "Show transcript notices for cache costs and provider recovery diagnostics", "showCacheMissNotices", PSK_BOOL,
      { "true", "false" }, "false", 0, "none", "amiga-pi raises no cache-cost or recovery notices, so there is none to show" },
    { "collapse-changelog", "Collapse changelog", "Show condensed changelog after updates", "collapseChangelog", PSK_BOOL,
      { "true", "false" }, "false", 0, "next start: What's New is one line", NOEX },
    { "quiet-startup", "Quiet startup", "Disable verbose printing at startup (header: keep only the startup header)", "quietStartup", PSK_QUIET,
      { "true", "header", "false" }, "false", 0, "next start: the start header and the loaded resources", NOEX },
    { "install-telemetry", "Install telemetry", "Send an anonymous version/update ping after changelog-detected updates", "enableInstallTelemetry", PSK_BOOL,
      { "true", "false" }, "true", 0, "none", "amiga-pi sends no telemetry, whatever this says" },
    { "default-project-trust", "Default project trust", "Fallback behavior when no extension or saved trust decision decides project trust",
      "defaultProjectTrust", PSK_TRUST, { "Ask", "Always trust", "Never trust" }, "Ask", 0, "next start: the project trust question", NOEX },
    { "double-escape-action", "Double-escape action", "Action when pressing Escape twice with empty editor", "doubleEscapeAction", PSK_ENUM,
      { "tree", "fork", "none" }, "tree", 0, "live: Escape twice on an empty editor opens /tree, /fork or nothing", NOEX },
    { "tree-filter-mode", "Tree filter mode", "Default filter when opening /tree", "treeFilterMode", PSK_ENUM,
      { "default", "no-tools", "user-only", "labeled-only", "all" }, "default", 0, "live: the filter /tree starts with", NOEX },
    { "warnings", "Warnings", "Enable or disable individual warnings", "warnings", PSK_MENU, { 0 }, "configure", 0,
      "none", "the one warning, \"Anthropic extra usage\", is for Anthropic subscription logins, which amiga-pi does not offer" },
    { "model-thinking", "Default thinking level per model", "Override the default thinking level for specific models. %T cycles in-session.", "modelThinkingLevels", PSK_MENU, { 0 }, "none", 0,
      "live: switching to a model with a level set here applies that level", NOEX },
    { "tui-mode", "TUI mode", "Interface layout; regular mode uses the terminal's normal scrollback", "tuiMode", PSK_ENUM,
      { "regular", "fullscreen" }, "fullscreen", 0, "next start: the alternate screen or the main one", NOEX },
    { "fullscreen-exit-output", "Fullscreen exit output", "Print the transcript or only a session resume hint when exiting fullscreen mode",
      "fullscreenExitOutput", PSK_ENUM, { "transcript", "resume-hint" }, "transcript", 0, "next start: what is left on the screen at exit", NOEX },
    { "fullscreen-scrollbar", "Fullscreen scrollbar", "Scrollbar behavior in fullscreen mode; has no effect in regular mode",
      "fullscreenScrollbar", PSK_ENUM, { "auto", "always", "hidden" }, "auto", 0, "next start: the scrollbar", NOEX },
    { "fullscreen-copy-on-select", "Fullscreen copy on select", "Automatically copy selected text in fullscreen mode; disable to copy selections with Ctrl+X",
      "fullscreenCopyOnSelect", PSK_BOOL, { "true", "false" }, "true", 0, "none", "amiga-pi takes no mouse (pi-tui group of COMPATIBILITY.md), "
      "so there is no selection to copy" },
    { "fullscreen-wheel-scroll-lines", "Fullscreen wheel scrolling", "Lines per mouse-wheel event in fullscreen mode; 'auto' speeds up fast wheel spins where the terminal does not",
      "fullscreenWheelScrollLines", PSK_WHEEL, { "auto", "1", "2", "3", "5", "10" }, "auto", 0, "none", "amiga-pi takes no mouse, so there is no wheel" },
    { "theme", "Theme", "Color theme for the interface", "theme", PSK_MENU, { 0 }, "system", 0,
      "live: the screen's colours from the next frame", NOEX }
};

int pi_settings_count(void)
{
    return (int)(sizeof(tab) / sizeof(tab[0]));
}

const pi_setting *pi_settings_at(int i)
{
    return i >= 0 && i < pi_settings_count() ? &tab[i] : 0;
}

const pi_setting *pi_settings_find(const char *id)
{
    int i;
    for (i = 0; i < pi_settings_count(); i++)
        if (!strcmp(tab[i].id, id))
            return &tab[i];
    return 0;
}

/* the value at the dotted path, or a type-0 jv */
static int get_path(const char *text, long n, const char *path, jv *out)
{
    jv cur;
    char key[80];
    const char *p = path;
    if (!text || n <= 0 || json_parse(text, n, &cur) || json_type(cur) != J_OBJ)
        return 0;
    for (;;) {
        const char *dot = strchr(p, '.');
        long l = dot ? (long)(dot - p) : (long)strlen(p);
        jv next;
        if (l >= (long)sizeof(key))
            l = (long)sizeof(key) - 1;
        memcpy(key, p, (size_t)l);
        key[l] = 0;
        if (json_type(cur) != J_OBJ || !json_get(cur, key, &next))
            return 0;
        cur = next;
        if (!dot)
            break;
        p = dot + 1;
    }
    *out = cur;
    return 1;
}

static const char *const idle_label[] = { "30 sec", "1 min", "2 min", "5 min", "disabled" };
static const long idle_ms[] = { 30000L, 60000L, 120000L, 300000L, 0L };

/* formatHttpIdleTimeoutMs */
static void idle_text(long ms, char *out, long cap)
{
    int i;
    char num[24];
    for (i = 0; i < 5; i++)
        if (idle_ms[i] == ms) {
            cl_copy(out, idle_label[i], cap);
            return;
        }
    cl_ltoa(ms / 1000, num);
    cl_copy(out, num, cap);
    cl_cat(out, " sec", cap);
}

static int in_vals(const pi_setting *s, const char *v)
{
    int i;
    for (i = 0; i < PS_MAX_VALS && s->vals[i]; i++)
        if (!strcmp(s->vals[i], v))
            return 1;
    return 0;
}

void pi_setting_shown(const pi_setting *s, const char *text, long n, char *out, long cap)
{
    jv v;
    char buf[80];
    int have = get_path(text, n, s->path, &v);
    cl_copy(out, s->dflt, cap);
    if (!have)
        return;
    switch (s->kind) {
    case PSK_BOOL:
        if (json_type(v) == J_TRUE || json_type(v) == J_FALSE)
            cl_copy(out, json_type(v) == J_TRUE ? "true" : "false", cap);
        break;
    case PSK_ENUM:
        /* the getters of settings-manager.ts: a value outside the list is the default */
        if (json_type(v) == J_STR) {
            json_str(v, buf, sizeof(buf));
            if (in_vals(s, buf))
                cl_copy(out, buf, cap);
        }
        break;
    case PSK_NUM:
        if (json_type(v) == J_NUM) {
            long x = json_long(v, 0);
            if (!strcmp(s->id, "output-padding"))
                x = x == 0 ? 0 : 1;            /* getOutputPad */
            cl_ltoa(x, buf);
            cl_copy(out, buf, cap);
        }
        break;
    case PSK_QUIET:
        if (json_type(v) == J_TRUE)
            cl_copy(out, "true", cap);
        else if (json_type(v) == J_STR && json_streq(v, "header"))
            cl_copy(out, "header", cap);
        break;
    case PSK_TRUST:
        if (json_type(v) == J_STR) {
            json_str(v, buf, sizeof(buf));
            if (!strcmp(buf, "always"))
                cl_copy(out, "Always trust", cap);
            else if (!strcmp(buf, "never"))
                cl_copy(out, "Never trust", cap);
        }
        break;
    case PSK_IDLE:
        if (json_type(v) == J_NUM) {
            long ms = json_long(v, -1);
            if (ms >= 0)
                idle_text(ms, out, cap);
        } else if (json_type(v) == J_STR && json_streq(v, "disabled"))
            cl_copy(out, "disabled", cap);
        break;
    case PSK_WHEEL:
        if (json_type(v) == J_NUM) {
            long x = json_long(v, 0);
            if (x < 1)
                x = 1;
            if (x > 100)
                x = 100;
            cl_ltoa(x, buf);
            cl_copy(out, buf, cap);
        }
        break;
    default:
        break;
    }
}

void pi_setting_next(const pi_setting *s, const char *shown, char *out, long cap)
{
    const char *list[PS_MAX_VALS + 1];
    char cur[24];
    int n = 0, i, at = -1;
    for (i = 0; i < PS_MAX_VALS && s->vals[i]; i++)
        list[n++] = s->vals[i];
    if (s->kind == PSK_WHEEL && strcmp(shown, "auto")) {
        /* ["auto", ...new Set([1, 2, 3, 5, 10, current])] sorted: the current value joins the list */
        long c = strtol(shown, 0, 10), vals[8];
        int k = 0, j, dup = 0;
        for (i = 1; i < n; i++)
            vals[k++] = strtol(list[i], 0, 10);
        for (j = 0; j < k; j++)
            if (vals[j] == c)
                dup = 1;
        if (!dup) {
            for (j = k; j > 0 && vals[j - 1] > c; j--)
                vals[j] = vals[j - 1];
            vals[j] = c;
            k++;
            for (i = 1; i <= k; i++) {
                static char store[9][12];
                cl_ltoa(vals[i - 1], store[i]);
                list[i] = store[i];
            }
            n = k + 1;
        }
    }
    for (i = 0; i < n; i++)
        if (!strcmp(list[i], shown))
            at = i;
    cl_copy(cur, list[(at + 1) % n], sizeof(cur));
    cl_copy(out, cur, cap);
}

int pi_setting_write(cl_sys *sys, const char *dir, const pi_setting *s, const char *shown)
{
    jw v;
    int rc;
    jw_init(&v);
    switch (s->kind) {
    case PSK_BOOL:
        jw_rawz(&v, !strcmp(shown, "true") ? "true" : "false");
        break;
    case PSK_QUIET:
        if (!strcmp(shown, "header"))
            jw_strz(&v, "header");
        else
            jw_rawz(&v, !strcmp(shown, "true") ? "true" : "false");
        break;
    case PSK_NUM:
        jw_rawz(&v, shown);
        break;
    case PSK_TRUST:
        jw_strz(&v, !strcmp(shown, "Always trust") ? "always" : !strcmp(shown, "Never trust") ? "never" : "ask");
        break;
    case PSK_IDLE: {
        int i;
        char num[24];
        for (i = 0; i < 5; i++)
            if (!strcmp(shown, idle_label[i]))
                break;
        cl_ltoa(i < 5 ? idle_ms[i] : 300000L, num);
        jw_rawz(&v, num);
        break;
    }
    case PSK_WHEEL:
        if (!strcmp(shown, "auto"))
            jw_strz(&v, "auto");
        else
            jw_rawz(&v, shown);
        break;
    default:
        jw_strz(&v, shown);
        break;
    }
    rc = v.oom ? -1 : pi_store_set_path(sys, dir, s->path, v.p);
    jw_free(&v);
    return rc;
}

static char *settings_file(cl_sys *sys, const char *dir, long *n)
{
    *n = 0;
    return dir && dir[0] && sys ? pi_store_read(sys, dir, "settings.json", n) : 0;
}

int pi_set_bool(cl_sys *sys, const char *dir, const char *path, int dflt)
{
    long n;
    char *t = settings_file(sys, dir, &n);
    jv v;
    int r = dflt;
    if (t && get_path(t, n, path, &v) && (json_type(v) == J_TRUE || json_type(v) == J_FALSE))
        r = json_type(v) == J_TRUE;
    free(t);
    return r;
}

long pi_set_num(cl_sys *sys, const char *dir, const char *path, long dflt)
{
    long n, r = dflt;
    char *t = settings_file(sys, dir, &n);
    jv v;
    if (t && get_path(t, n, path, &v) && json_type(v) == J_NUM)
        r = json_long(v, dflt);
    free(t);
    return r;
}

int pi_set_str(cl_sys *sys, const char *dir, const char *path, const char *dflt, char *out, long cap)
{
    long n;
    char *t = settings_file(sys, dir, &n);
    jv v;
    int rc = -1;
    cl_copy(out, dflt, cap);
    if (t && get_path(t, n, path, &v) && json_type(v) == J_STR) {
        json_str(v, out, cap);
        rc = 0;
    }
    free(t);
    return rc;
}

void pi_settings_session(cl_sys *sys, const char *dir, struct pi_session *s, struct pi_store *st, const char *id)
{
    char v[32];
    if (!id || !strcmp(id, "autocompact"))
        if (st)
            st->cp_enabled = pi_set_bool(sys, dir, "compaction.enabled", 1);
    if (!id || !strcmp(id, "steering-mode")) {
        pi_set_str(sys, dir, "steeringMode", "one-at-a-time", v, sizeof(v));
        s->steer_all = !strcmp(v, "all");
    }
    if (!id || !strcmp(id, "follow-up-mode")) {
        pi_set_str(sys, dir, "followUpMode", "one-at-a-time", v, sizeof(v));
        s->follow_all = !strcmp(v, "all");
    }
    if (!id || !strcmp(id, "block-images"))
        s->block_images = pi_set_bool(sys, dir, "images.blockImages", 0);
    if (!id || !strcmp(id, "auto-resize-images"))
        pi_image_set_resize(pi_set_bool(sys, dir, "images.autoResize", 1));
    if (!id || !strcmp(id, "http-idle-timeout")) {
        long ms = pi_set_num(sys, dir, "httpIdleTimeoutMs", 300000L);
        if (ms < 0)
            ms = 300000L;
        s->idle_ms = ms == 300000L ? 0 : ms == 0 ? 2147483647L : ms;     /* disabled: no limit worth the name */
    }
}

int pi_set_model_level(cl_sys *sys, const char *dir, const char *provider, const char *id)
{
    long n;
    char *t = settings_file(sys, dir, &n), key[300], val[40];
    jv root, sub, v;
    jit it;
    jv k;
    int lv = -1;
    cl_copy(key, provider, sizeof(key));
    cl_cat(key, "/", sizeof(key));
    cl_cat(key, id, sizeof(key));
    if (t && !json_parse(t, n, &root) && json_get(root, "modelThinkingLevels", &sub) && json_type(sub) == J_OBJ) {
        json_iter(sub, &it);
        while (json_next(&it, &k, &v)) {
            char kk[300];
            json_str(k, kk, sizeof(kk));
            if (!strcmp(kk, key) && json_type(v) == J_STR) {
                json_str(v, val, sizeof(val));
                lv = llm_think_of(val);
            }
        }
    }
    free(t);
    return lv;
}
