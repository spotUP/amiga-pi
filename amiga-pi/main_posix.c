/* amiga-pi on the host (build/amiga-pi-host): app.c over stdin/stdout and
 * POSIX TCP (claude/net_posix.c: plain http, no TLS), for manual runs
 * against tools/claude_fixture.py:
 *   build/amiga-pi-host --model fixture/qwen2.5-coder:7b --base-url http://127.0.0.1:8080/v1 -p hello */
#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include "app.h"
#include "claude/net_posix.h"

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

int main(int argc, char **argv)
{
    static net_posix np;
    static cl_net net;
    static llm_pool pool;
    static cl_io io;
    static pi_platform pl;
    cl_net *nets[1];
    char cwd[512];
    signal(SIGINT, on_int);
    net_posix_init(&np, &net);
    nets[0] = &net;
    llm_pool_init(&pool, nets, 1);
    io.read_line = p_read;
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
    pl.now = p_now;
    pl.getenv = p_getenv;
    pl.cwd = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    return pi_app(argc, argv, &pl);
}
