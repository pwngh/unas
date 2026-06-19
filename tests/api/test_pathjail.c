/* tests/api/test_pathjail.c — the security-critical bit: fsapi_resolve
 * must never let a request escape the share root. Links libunas.a. */
#include "fsapi.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

static int fails = 0;
#define ROOT "/srv/share"

/* A path that must resolve cleanly to ROOT + want, with the given is_dir. */
static void ok(const char *sub, const char *want, bool want_dir)
{
    char out[4096];
    bool dir = !want_dir;            /* seed with the wrong value so a no-write shows */
    if (fsapi_resolve(ROOT, sub, out, sizeof out, &dir) != 0) {
        fprintf(stderr, "FAIL: resolve(%s) errored, expected %s\n", sub, want); fails++; return;
    }
    if (strcmp(out, want) != 0) {
        fprintf(stderr, "FAIL: resolve(%s) = %s (want %s)\n", sub, out, want); fails++;
    }
    if (dir != want_dir) {
        fprintf(stderr, "FAIL: resolve(%s) is_dir=%d (want %d)\n", sub, (int)dir, (int)want_dir); fails++;
    }
}

/* A path that must be rejected with EINVAL (traversal / NUL / bad escape). */
static void bad(const char *sub)
{
    char out[4096];
    bool dir = false;
    errno = 0;
    if (fsapi_resolve(ROOT, sub, out, sizeof out, &dir) == 0) {
        fprintf(stderr, "FAIL: resolve(%s) accepted -> %s (should reject)\n", sub, out); fails++; return;
    }
    if (errno != EINVAL) {
        fprintf(stderr, "FAIL: resolve(%s) errno=%d (want EINVAL=%d)\n", sub, errno, EINVAL); fails++;
    }
}

int main(void)
{
    /* ordinary paths */
    ok("/Photos/cat.jpg", "/srv/share/Photos/cat.jpg", false);
    ok("/Photos/",        "/srv/share/Photos",         true);
    ok("/Photos",         "/srv/share/Photos",         false);
    ok("/",               "/srv/share",                true);
    ok("",                "/srv/share",                true);   /* bare /v1/fs */

    /* normalization: ".", "//", trailing slash */
    ok("/a/./b",  "/srv/share/a/b", false);
    ok("/a//b",   "/srv/share/a/b", false);
    ok("/a/b/",   "/srv/share/a/b", true);

    /* percent-decoding */
    ok("/a%20b.txt",  "/srv/share/a b.txt", false);   /* space */
    ok("/a%2Fb",      "/srv/share/a/b",     false);    /* encoded slash -> separator */

    /* traversal — rejected, plain and encoded */
    bad("/../etc/passwd");
    bad("/a/../../x");
    bad("/..");
    bad("/a/..");
    bad("/%2e%2e/x");          /* ".." after decode */
    bad("/a/%2E%2E/b");

    /* malformed percent-escapes and embedded NUL */
    bad("/a%zzb");
    bad("/a%2");               /* truncated escape */
    bad("/a%00b");             /* decoded NUL */

    if (fails) { fprintf(stderr, "test_pathjail: %d failure(s)\n", fails); return 1; }
    printf("test_pathjail: OK\n");
    return 0;
}
