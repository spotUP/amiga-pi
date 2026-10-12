/* amiga-pi on the host (build/amiga-pi-host): app.c over stdin/stdout and
 * POSIX TCP (claude/net_posix.c: plain http, no TLS), for manual runs
 * against tools/claude_fixture.py:
 *   build/amiga-pi-host --model fixture/qwen2.5-coder:7b --base-url http://127.0.0.1:8080/v1 -p hello
 * pi_host_run() is the whole platform (main_posix.c is its main); the host test drives it with another transport. */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "app.h"
#include "host_posix.h"
#include "tools_pi.h"
#include "claude/net_posix.h"
#include "claude/sys_posix.h"

static volatile sig_atomic_t got_int;

static void on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

static long p_read(void *u, char *buf, long cap)
{
    long n;
    (void)u;
    if (!fgets(buf, (int)cap, stdin))
        return -1;
    n = (long)strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    return n;
}

/* bytes from stdin (rpc mode): up to cap, waiting timeout_ms for the first (0: do not wait); 0 none came, -1 the end. The
 * stdio buffer is not used: fgets (p_read) and this are not mixed in one run. */
static long p_readbytes(void *u, char *buf, long cap, long timeout_ms)
{
    struct pollfd pf;
    ssize_t n;
    (void)u;
    pf.fd = 0;
    pf.events = POLLIN;
    pf.revents = 0;
    if (poll(&pf, 1, (int)timeout_ms) <= 0)
        return 0;
    n = read(0, buf, (size_t)cap);
    return n > 0 ? (long)n : n == 0 ? -1 : 0;
}

static void p_diag(const char *line)
{
    fputs(line, stderr);
    fputc('\n', stderr);
}

static void p_write(void *u, const char *s, long n)
{
    (void)u;
    fwrite(s, 1, (size_t)n, stdout);
    fflush(stdout);
}

static int p_brk(void *u)
{
    int b = got_int;
    (void)u;
    got_int = 0;
    return b;
}

static int p_sleep(void *u, long ms)
{
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&t, 0);
    return p_brk(u);
}

static unsigned long p_ms(void *u)
{
    struct timeval tv;
    (void)u;
    gettimeofday(&tv, 0);
    return (unsigned long)tv.tv_sec * 1000UL + (unsigned long)tv.tv_usec / 1000UL;
}

static llm_ts p_now(void *u)
{
    struct timeval tv;
    llm_ts t;
    (void)u;
    gettimeofday(&tv, 0);
    t.sec = (long)tv.tv_sec;
    t.ms = (int)(tv.tv_usec / 1000);
    return t;
}

static int p_resolve(void *u, const char *host, unsigned char ip[4])
{
    struct addrinfo hints, *res = 0;
    (void)u;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, 0, &hints, &res) || !res)
        return -1;
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4);
    freeaddrinfo(res);
    return 0;
}

static long p_getenv(const char *name, char *out, long cap)
{
    const char *v = getenv(name);
    if (!v)
        return -1;
    strncpy(out, v, (size_t)cap - 1);
    out[cap - 1] = 0;
    return (long)strlen(out);
}

int pi_host_run(int argc, char **argv, const pi_platform *over)
{
    static net_posix np;
    static cl_net net;
    static llm_pool pool;
    static cl_io io;
    static pi_platform pl;
    static sys_posix sp;
    static cl_sys sys;
    static char tmp[300], agent[300];
    cl_net *nets[1];
    char cwd[512];
    const char *t = getenv("TMPDIR"), *home = getenv("HOME");
    signal(SIGINT, on_int);
    net_posix_init(&np, &net);
    nets[0] = &net;
    llm_pool_init(&pool, nets, 1);
    io.read_line = p_read;
    io.read = p_readbytes;      /* rpc mode reads commands while a run goes on; no raw mode, so the screen is not offered */
    io.write = p_write;
    io.brk = p_brk;
    io.sleep = p_sleep;
    io.ms = p_ms;
    pl.io = &io;
    pl.pool = &pool;
    pl.h.poll = p_brk;
    pl.h.sleep = p_sleep;
    pl.h.ms = p_ms;
    pl.h.resolve = p_resolve;
    if (over) {                 /* the host test's transport: the pool and the names it answers */
        pl.pool = over->pool;
        pl.h.resolve = over->h.resolve;
        pl.h.u = over->h.u;
    }
    pl.now = p_now;
    pl.diag = p_diag;
    pl.getenv = p_getenv;
    pl.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    /* the tools over C:Claude's host sys (commands through /bin/sh) */
    sys_posix_init(&sp, &sys);
    pl.sys = &sys;
    strncpy(tmp, t && *t ? t : "/tmp", sizeof(tmp) - 2);
    if (tmp[strlen(tmp) - 1] != '/')
        strcat(tmp, "/");
    pl.tmp = tmp;
    pl.shell = PI_SH_HOST;
    pl.utc = 1;
    if (getenv("PI_CODING_AGENT_DIR"))
        strncpy(agent, getenv("PI_CODING_AGENT_DIR"), sizeof(agent) - 1);
    else if (home && strlen(home) < 280) {
        strcpy(agent, home);
        strcat(agent, "/.pi/agent");
    }
    pl.agent_dir = agent;
    pl.data_dir = agent;        /* pi keeps what grows in its agent dir */
    return pi_app(argc, argv, &pl);
}
