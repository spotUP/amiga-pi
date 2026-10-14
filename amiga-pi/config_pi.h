/* config_pi -- `amiga-pi config`, pi 1.2.0's resource configuration screen (modes/interactive/components/config-selector.ts, cli/config-selector.ts,
 * package-manager-cli.ts handleConfigCommand): the skills and prompt templates that pkgres.h finds for the user (the agent directory's, the
 * settings.json entries, the packages), grouped as pi groups them (a package's source, "User (<directory>)", "User settings"), each with a box
 * that switches it on or off. A change is written to settings.json at once, the way pi writes it: a top-level resource is an entry
 * `-<path>` / `+<path>` of the "skills" or "prompts" array (the path relative to the group's directory), a package's resource an entry of the
 * array in the package's object. pkgres.c reads those entries back, so the box is the resource's state at the next start.
 *
 * Where it differs from pi: pi lists extensions and themes too, and its built-in extensions; amiga-pi's extensions are Lua files and its themes
 * JSON files that no pattern in settings.json switches, so only the two resource types pkgres.c resolves are listed. The project mode (Tab,
 * `--local`) is not built: it needs the project overrides of the same file layout and a trusted project; `config --local` says so.
 *
 * With a console that has a raw mode the screen is the selector; otherwise the same rows are printed numbered and a number toggles one.
 * Portable C89, host-tested (tests/test_pi_tui.c config_*, tests/test_pi_resources.c). */
#ifndef PI_CONFIG_H
#define PI_CONFIG_H

#include "claude/sys.h"
#include "claude/theme.h"
#include "claude/ui.h"

typedef struct pi_config_env {
    cl_io *io;
    cl_sys *sys;
    const cl_pitheme *th;
    const char *agent_dir;      /* settings.json and the user's skills/ and prompts/ below it */
    const char *data_dir;       /* skills/ below it too, 0 none */
    const char *cwd;
    const char *home;           /* what `~` is in the group labels, "" none */
    const char *kb;             /* keybindings.json text, 0 pi's defaults */
    long kbn;
} pi_config_env;

/* the screen, or the numbered list: 0 when closed, -1 out of memory */
int pi_config_run(const pi_config_env *e);

#endif
