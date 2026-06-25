/*
 * @pwngh/unas
 *
 * Copyright (c) Preston Neal
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE.md file in the root directory of this source tree.
 *
 * @license MIT
 */

/* tests/api/test_jsonw.c — the JSON builder's string escaping, especially
 * UTF-8 handling: valid sequences pass through, invalid bytes become U+FFFD
 * so a listing of a badly-named file is still well-formed JSON. Links
 * libunas.a. */
#include "jsonw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

/* jb_str at top level yields just the quoted/escaped string literal. */
static void chk(const char *name, const char *in, const char *want)
{
    json_buf b;
    char *got;
    jb_init(&b);
    jb_str(&b, in);
    got = jb_take(&b, NULL);
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s = %s (want %s)\n", name, got ? got : "(null)", want);
        fails++;
    }
    free(got);
}

int main(void)
{
    chk("plain",      "hello",        "\"hello\"");
    chk("quote",      "a\"b",         "\"a\\\"b\"");
    chk("backslash",  "a\\b",         "\"a\\\\b\"");
    chk("tab",        "x\ty",         "\"x\\ty\"");
    chk("ctrl",       "\x01",         "\"\\u0001\"");
    chk("del-ok",     "\x7f",         "\"\x7f\"");           /* DEL is legal in a JSON string */
    chk("utf8-2byte", "caf\xc3\xa9",  "\"caf\xc3\xa9\"");    /* é passes through */
    chk("utf8-3byte", "\xe2\x82\xac", "\"\xe2\x82\xac\"");   /* € passes through */
    chk("bad-lead",   "\xff",         "\"\\ufffd\"");        /* 0xFF is never a UTF-8 lead */
    chk("lone-cont",  "\x80",         "\"\\ufffd\"");        /* continuation with no lead */
    chk("truncated",  "\xc3",         "\"\\ufffd\"");        /* 2-byte lead, no continuation */
    chk("overlong",   "\xc0\xaf",     "\"\\ufffd\\ufffd\""); /* 0xc0 invalid lead, 0xaf lone continuation: two replacements */

    if (fails) { fprintf(stderr, "test_jsonw: %d failure(s)\n", fails); return 1; }
    printf("test_jsonw: OK\n");
    return 0;
}
