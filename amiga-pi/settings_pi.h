/* settings_pi -- the settings /settings lists (pi 1.2.0 settings-selector.ts), as one table: what each is called, the words under it, the values
 * its row cycles through, where settings.json keeps it and what it changes in amiga-pi. The screen (ui_pi.c) draws the table; nothing else
 * writes these keys: a row's change goes through authstore.h pi_store_set_path, the one writer of settings.json (SettingsManager's setX: only the
 * modified key changes, every other member stays as it is).
 *
 * Every row either moves amiga-pi's output or carries a written exemption (`exempt`: why it cannot here); tests/test_pi_settings.c holds both
 * to account (a row with neither fails). Portable C89. */
#ifndef PI_SETTINGS_H
#define PI_SETTINGS_H

#include "claude/sys.h"

struct pi_session;
struct pi_store;

/* how a row's value is read from and written to settings.json */
enum {
    PSK_BOOL,                   /* true | false */
    PSK_ENUM,                   /* a string member, one of vals */
    PSK_NUM,                    /* a number member, one of vals (text of the digits) */
    PSK_QUIET,                  /* quietStartup: true | "header" | false */
    PSK_TRUST,                  /* defaultProjectTrust: "ask" | "always" | "never", shown as Ask | Always trust | Never trust */
    PSK_IDLE,                   /* httpIdleTimeoutMs: milliseconds, shown as 30 sec | 1 min | 2 min | 5 min | disabled */
    PSK_WHEEL,                  /* fullscreenWheelScrollLines: "auto" or 1..100 */
    PSK_MENU                    /* a submenu: the theme, the warnings, the per-model thinking levels */
};

#define PSF_IMAGES 1u           /* listed only where the terminal draws images (getCapabilities().images) */
#define PS_MAX_VALS 8

typedef struct pi_setting {
    const char *id;             /* pi's item id ("autocompact") */
    const char *label;
    const char *desc;           /* "%F" stands for the follow-up key, "%T" for the thinking-cycle key */
    const char *path;           /* settings.json member, dotted ("compaction.enabled") */
    int kind;
    const char *vals[PS_MAX_VALS];
    const char *dflt;           /* the shown value when settings.json has none */
    unsigned flags;
    const char *moves;          /* what it changes in amiga-pi, and when ("live", "next start") */
    const char *exempt;         /* 0 when it moves the output; else why it cannot here */
} pi_setting;

int pi_settings_count(void);
const pi_setting *pi_settings_at(int i);
const pi_setting *pi_settings_find(const char *id);

/* the value the row shows, from settings.json's text (n bytes; 0: no file) */
void pi_setting_shown(const pi_setting *s, const char *text, long n, char *out, long cap);
/* the value after the row's next press (cycling vals; the wheel row's list holds the current number too) */
void pi_setting_next(const pi_setting *s, const char *shown, char *out, long cap);
/* `shown` written to settings.json (the one writer); 0, -1 failed */
int pi_setting_write(cl_sys *sys, const char *dir, const pi_setting *s, const char *shown);

/* the settings that steer the session itself, read from settings.json into a session (and its store) -- at the start, and again after /settings
 * changed one: autocompact, steering and follow-up modes, block images, auto-resize, the HTTP idle timeout. `id` names the one row that changed
 * (the table's ids), 0 applies all of them */
void pi_settings_session(cl_sys *sys, const char *dir, struct pi_session *s, struct pi_store *st, const char *id);

/* settings.json's modelThinkingLevels["provider/id"] as a thinking level (LT_*), -1 when the model has none */
int pi_set_model_level(cl_sys *sys, const char *dir, const char *provider, const char *id);

/* typed readers for the code a setting steers: the global settings.json of `dir` (the user's, as pi's setX writes it), `dflt` when absent or of
 * another type */
int pi_set_bool(cl_sys *sys, const char *dir, const char *path, int dflt);
long pi_set_num(cl_sys *sys, const char *dir, const char *path, long dflt);
/* a string member into out (cap bytes); 0 when found, -1 absent or not a string (out then holds dflt) */
int pi_set_str(cl_sys *sys, const char *dir, const char *path, const char *dflt, char *out, long cap);

#endif
