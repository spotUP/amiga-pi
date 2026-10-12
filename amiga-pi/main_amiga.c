/* amiga-pi -- pi's coding agent for AmigaOS 3.x (68020+), plan phase C.
 *
 *   amiga-pi [-p] [--model provider/id] [--base-url URL] [prompt]
 *   amiga-pi --model fixture/qwen2.5-coder:7b --base-url http://192.168.1.2:8080/v1 -p hello
 *
 * The key: ENV:<PROVIDER>_API_KEY (never sent over plain http: llm/ drops
 * it with a notice). ENV:PI_MEM set: the free memory at the start and the
 * least seen while it ran, printed at the end (the plan's heap check).
 * This file is only the wiring of app.c (host-tested) to the console
 * (claude/io_amiga.c, C:Claude's), bsdsocket (net_amiga.c) and AmigaDOS. */
#include <string.h>
#include <stdio.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "app.h"
#include "claude/datadir.h"
#include "claude/io_amiga.h"
#include "claude/net_amiga.h"
#include "claude/sys_amiga.h"
#include "claude/util.h"
#include "tools_pi.h"

static const char vers[] = "$VER: amiga-pi 0.1 (11.10.2026) UP-Term";
const char stack_cookie[] = "$STACK: 32768";

static unsigned long mem_start, mem_least;
static int mem_watch;

static void mem_sample(void)
{
    unsigned long f;
    if (!mem_watch)
        return;
    f = AvailMem(MEMF_ANY);
    if (f < mem_least)
        mem_least = f;
}

static int a_poll(void *u)
{
    mem_sample();
    return io_amiga_brk(u);
}

static int a_sleep(void *u, long ms)
{
    long ticks = ms / 20;
    while (ticks > 0) {
        long t = ticks > 5 ? 5 : ticks;
        if (io_amiga_brk(u))
            return 1;
        Delay(t);
        ticks -= t;
    }
    return io_amiga_brk(u);
}

static unsigned long a_ms(void *u)
{
    struct DateStamp d;
    (void)u;
    DateStamp(&d);
    return (unsigned long)d.ds_Days * 86400000UL + (unsigned long)d.ds_Minute * 60000UL +
           (unsigned long)d.ds_Tick * 20UL;
}

/* local time without an offset (plan E9), from 1978 to 1970 (2922 days) */
/* json and rpc mode: the program's own messages go to the process's error stream (pi: stderr), Output() when none is set */
static void a_diag(const char *line)
{
    struct Process *me = (struct Process *)FindTask(0);
    BPTR fh = me->pr_CES ? me->pr_CES : Output();
    if (!fh)
        return;
    Write(fh, (APTR)line, (LONG)strlen(line));
    Write(fh, (APTR)"\n", 1);
}

static llm_ts a_now(void *u)
{
    struct DateStamp d;
    llm_ts t;
    (void)u;
    DateStamp(&d);
    t.sec = (d.ds_Days + 2922L) * 86400L + d.ds_Minute * 60L + d.ds_Tick / 50;
    t.ms = (int)(d.ds_Tick % 50) * 20;
    return t;
}

static int a_resolve(void *u, const char *host, unsigned char ip[4])
{
    (void)u;
    return net_amiga_resolve(host, ip);
}

static long a_getenv(const char *name, char *out, long cap)
{
    LONG n = GetVar((STRPTR)name, (STRPTR)out, cap, 0);
    return n > 0 ? (long)n : -1;
}

int main(int argc, char **argv)
{
    static net_amiga na;
    static cl_net net;
    static llm_pool pool;
    static cl_io io;
    static pi_platform pl;
    static char cwd[512], agent[256], data[256];
    static sys_amiga sa;
    static cl_sys sys;
    cl_net *nets[1];
    char v[8], what[160];
    struct Process *me = (struct Process *)FindTask(0);
    int rc;
    (void)vers;
    mem_watch = GetVar((STRPTR)"PI_MEM", (STRPTR)v, sizeof(v), 0) >= 0;
    mem_start = mem_least = AvailMem(MEMF_ANY);
    io_amiga_args();
    io_amiga_line(&io);
    io_amiga_screen(&io);
    net_amiga_init(&na, &net);
    nets[0] = &net;
    llm_pool_init(&pool, nets, 1);
    if (!me->pr_CurrentDir || !NameFromLock(me->pr_CurrentDir, (STRPTR)cwd, sizeof(cwd)))
        cl_copy(cwd, "SYS:", sizeof(cwd));
    pl.io = &io;
    pl.pool = &pool;
    pl.h.poll = a_poll;
    pl.h.sleep = a_sleep;
    pl.h.ms = a_ms;
    pl.h.resolve = a_resolve;
    pl.now = a_now;
    pl.diag = a_diag;
    pl.getenv = a_getenv;
    pl.cwd = cwd;
    /* the tools: AmigaDOS through C:Claude's sys_amiga; bash runs vsh when it is installed */
    sys_amiga_init(&sa, &sys);
    pl.sys = &sys;
    pl.tmp = "T:";
    pl.shell = sys.info && sys.info(sys.u, "vsh", what, sizeof(what)) == 1 ? PI_SH_VSH : PI_SH_AMIGASHELL;
    /* settings.json: ENVARC:amiga-pi (pi's ~/.pi/agent), PI_CODING_AGENT_DIR elsewhere */
    if (a_getenv("PI_CODING_AGENT_DIR", agent, sizeof(agent)) <= 0)
        cl_copy(agent, "ENVARC:amiga-pi", sizeof(agent));
    pl.agent_dir = agent;
    if (dd_dir(&sys, "amiga-pi", data, sizeof(data)) >= 0)
        pl.data_dir = data;     /* the data-directory rule (claude/datadir.h) */
    rc = pi_app(argc, argv, &pl);
    io_amiga_end();
    llm_pool_close(&pool);
    net_amiga_exit(&na);
    if (mem_watch)
        printf("[INFO] memory: %lu bytes free at the start, %lu least while it ran (%lu used at most)\n", mem_start,
               mem_least, mem_start - mem_least);
    return rc == 2 ? 20 : rc ? 10 : 0;     /* bad arguments: fail; the answer failed: error */
}
