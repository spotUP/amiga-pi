/* see prompt.h */
#include <string.h>
#include "prompt.h"
#include "claude/amiga_text.h"
#include "claude/util.h"

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

/* a section's text: pi wraps every one but the preamble in a tag of its name */
static void open_tag(jw *w, const char *name)
{
    jw_rawz(w, "<");
    jw_rawz(w, name);
    jw_rawz(w, ">\n");
}

static void close_tag(jw *w, const char *name)
{
    jw_rawz(w, "\n</");
    jw_rawz(w, name);
    jw_rawz(w, ">");
}

/* the section just written (from at to the end of text) recorded under name */
static void section(pi_psec *o, const char *name, long at)
{
    if (o->n < PI_PSECS) {
        cl_copy(o->name[o->n], name, sizeof(o->name[0]));
        o->off[o->n] = at;
        o->len[o->n] = o->text.n - at;
        o->n++;
    }
}

int pi_prompt_sections(const pi_session *s, const char *cwd, pi_psec *out)
{
    int i, k = 0;
    long at;
    memset(out, 0, sizeof(*out));
    jw_init(&out->text);
    at = out->text.n;
    jw_rawz(&out->text, preamble);
    section(out, "preamble", at);
    /* tools: "- name: snippet" for the tools that have one */
    at = out->text.n;
    open_tag(&out->text, "tools");
    for (i = 0; i < s->ntools; i++)
        if (s->tools[i]->snippet) {
            jw_rawz(&out->text, k++ ? "\n- " : "- ");
            jw_rawz(&out->text, s->tools[i]->name);
            jw_rawz(&out->text, ": ");
            jw_rawz(&out->text, s->tools[i]->snippet);
        }
    if (!k)
        jw_rawz(&out->text, "(none)");
    jw_rawz(&out->text, "\n\nIn addition to the tools above, you may have access to other custom tools depending on "
                        "the project.");
    close_tag(&out->text, "tools");
    section(out, "tools", at);
    /* rules: buildRules */
    at = out->text.n;
    open_tag(&out->text, "rules");
    if (has(s, "bash") && !has(s, "grep") && !has(s, "find") && !has(s, "ls"))
        jw_rawz(&out->text, "- Use bash for file operations like ls, rg, find\n");
    /* each tool's guidelines, in tool order, each once (pi's addRule) */
    for (i = 0; i < s->ntools; i++) {
        const char *const *g = s->tools[i]->guide;
        for (; g && *g; g++) {
            int j, seen = 0;
            for (j = 0; j < i && !seen; j++) {
                const char *const *h = s->tools[j]->guide;
                for (; h && *h && !seen; h++)
                    seen = !strcmp(*h, *g);
            }
            if (seen)
                continue;
            jw_rawz(&out->text, "- ");
            jw_rawz(&out->text, *g);
            jw_rawz(&out->text, "\n");
        }
    }
    jw_rawz(&out->text, "- Be concise in your responses\n- Show file paths clearly when working with files");
    close_tag(&out->text, "rules");
    section(out, "rules", at);
    /* amiga: the machine, as C:Claude tells it (claude/amiga_text.h) */
    at = out->text.n;
    open_tag(&out->text, "amiga");
    jw_rawz(&out->text, "You run as a native program on " CL_AMIGA_MACHINE ". " CL_AMIGA_PATHS
                        "Relative paths start from the working directory (cwd). The machine is slow and has little "
                        "memory: prefer targeted reads and searches to reading whole trees.");
    if (s->shell_note) {
        jw_rawz(&out->text, " ");
        jw_rawz(&out->text, s->shell_note);
    }
    close_tag(&out->text, "amiga");
    section(out, "amiga", at);
    at = out->text.n;
    open_tag(&out->text, "cwd");
    jw_rawz(&out->text, cwd ? cwd : "");
    close_tag(&out->text, "cwd");
    section(out, "cwd", at);
    return out->text.oom ? -1 : 0;
}

void pi_psec_free(pi_psec *p)
{
    jw_free(&p->text);
    p->n = 0;
}

/* getSystemMessageText of a message that is all sections: the texts joined by a blank line */
int pi_system_prompt(const pi_session *s, const char *cwd, jw *out)
{
    pi_psec p;
    int i;
    if (pi_prompt_sections(s, cwd, &p)) {
        pi_psec_free(&p);
        return -1;
    }
    for (i = 0; i < p.n; i++) {
        if (i)
            jw_rawz(out, "\n\n");
        jw_raw(out, p.text.p + p.off[i], p.len[i]);
    }
    pi_psec_free(&p);
    return out->oom ? -1 : 0;
}
