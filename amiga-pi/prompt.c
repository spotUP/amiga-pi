/* see prompt.h */
#include <string.h>
#include "prompt.h"
#include "claude/amiga_text.h"

static const char preamble[] =
    "You are an expert coding assistant operating inside amiga-pi, a coding agent harness. You help users by "
    "reading files, executing commands, editing code, and writing new files.";

/* the tools pi's prompt names: amiga-pi's own, all declared (no hidden ones yet) */
static int has(const pi_session *s, const char *name)
{
    int i;
    for (i = 0; i < s->ntools; i++)
        if (!strcmp(s->tools[i]->name, name))
            return 1;
    return 0;
}

static void open_tag(jw *w, const char *name)
{
    jw_rawz(w, "\n\n<");
    jw_rawz(w, name);
    jw_rawz(w, ">\n");
}

static void close_tag(jw *w, const char *name)
{
    jw_rawz(w, "\n</");
    jw_rawz(w, name);
    jw_rawz(w, ">");
}

int pi_system_prompt(const pi_session *s, const char *cwd, jw *out)
{
    int i, k = 0;
    jw_rawz(out, preamble);
    /* tools: "- name: snippet" for the tools that have one */
    open_tag(out, "tools");
    for (i = 0; i < s->ntools; i++)
        if (s->tools[i]->snippet) {
            jw_rawz(out, k++ ? "\n- " : "- ");
            jw_rawz(out, s->tools[i]->name);
            jw_rawz(out, ": ");
            jw_rawz(out, s->tools[i]->snippet);
        }
    if (!k)
        jw_rawz(out, "(none)");
    jw_rawz(out, "\n\nIn addition to the tools above, you may have access to other custom tools depending on the "
                 "project.");
    close_tag(out, "tools");
    /* rules: buildRules */
    open_tag(out, "rules");
    if (has(s, "bash") && !has(s, "grep") && !has(s, "find") && !has(s, "ls"))
        jw_rawz(out, "- Use bash for file operations like ls, rg, find\n");
    jw_rawz(out, "- Be concise in your responses\n- Show file paths clearly when working with files");
    close_tag(out, "rules");
    /* amiga: the machine, as C:Claude tells it (claude/amiga_text.h) */
    open_tag(out, "amiga");
    jw_rawz(out, "You run as a native program on " CL_AMIGA_MACHINE ". " CL_AMIGA_PATHS
                 "Relative paths start from the working directory (cwd). The machine is slow and has little "
                 "memory: prefer targeted reads and searches to reading whole trees.");
    close_tag(out, "amiga");
    open_tag(out, "cwd");
    jw_rawz(out, cwd ? cwd : "");
    close_tag(out, "cwd");
    return out->oom ? -1 : 0;
}
