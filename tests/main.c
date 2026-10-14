/* amiga-pi's host suites: all, or the one argv[1] names (make test ONLY=<name>). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pi_test.h"

void suite_pi_agent(void);
void suite_pi_tools(void);
void suite_pi_session(void);
void suite_pi_store(void);
void suite_pi_model(void);
void suite_pi_mcp(void);
void suite_pi_resources(void);
void suite_pi_tui(void);
void suite_pi_templates(void);
void suite_pi_modes(void);
void suite_pi_hooks(void);
void suite_pi_lua(void);
void suite_pi_login(void);
void suite_pi_yaml(void);
void suite_pi_diff(void);
void suite_pi_radius(void);
void suite_pi_settings(void);

static const h_suite suites[] = {
    { "pi_agent", suite_pi_agent },
    { "pi_tools", suite_pi_tools },
    { "pi_session", suite_pi_session },
    { "pi_store", suite_pi_store },
    { "pi_model", suite_pi_model },
    { "pi_mcp", suite_pi_mcp },
    { "pi_resources", suite_pi_resources },
    { "pi_tui", suite_pi_tui },
    { "pi_templates", suite_pi_templates },
    { "pi_modes", suite_pi_modes },
    { "pi_hooks", suite_pi_hooks },
    { "pi_lua", suite_pi_lua },
    { "pi_login", suite_pi_login },
    { "pi_yaml", suite_pi_yaml },
    { "pi_diff", suite_pi_diff },
    { "pi_radius", suite_pi_radius },
    { "pi_settings", suite_pi_settings }
};

int pi_golden(const char *name, const char *got)
{
    char path[512];
    FILE *f;
    char *want;
    long n;
    int ok;
    strcpy(path, PI_TESTS "/");
    strncat(path, name, sizeof(path) - strlen(path) - 1);
    if (getenv("PI_UPDATE_GOLDEN")) {
        f = fopen(path, "wb");
        if (f) {
            fputs(got, f);
            fclose(f);
        }
    }
    f = fopen(path, "rb");
    if (!f) {
        h_checks++;
        h_failures++;
        printf("  FAIL golden %s missing (PI_UPDATE_GOLDEN=1 writes it)\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    want = (char *)malloc((size_t)n + 1);
    n = want ? (long)fread(want, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!want)
        return 0;
    want[n] = 0;
    ok = !strcmp(want, got);
    h_checks++;
    if (!ok) {
        h_failures++;
        printf("  FAIL golden %s differs:\n--- want\n%s\n--- got\n%s\n", name, want, got);
    }
    free(want);
    return ok;
}

int main(int argc, char **argv)
{
    int i, ran = 0;
    for (i = 0; i < (int)(sizeof(suites) / sizeof(suites[0])); i++) {
        int f0 = h_failures, c0 = h_checks;
        if (argc > 1 && strcmp(argv[1], suites[i].name))
            continue;
        suites[i].fn();
        ran++;
        printf("[%s] %s: %d checks, %d failed\n", h_failures == f0 ? "OK" : "FAIL", suites[i].name, h_checks - c0,
               h_failures - f0);
    }
    if (!ran)
        printf("[ERROR] no suite named %s\n", argc > 1 ? argv[1] : "");
    return h_failures || !ran ? 1 : 0;
}
