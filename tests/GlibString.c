// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdlib.h>
#include "glib.h"

int main(void)
{
    GString* text = g_string_new("abc");
    if (!text || text->allocated_len <= text->len) return 1;
    if (strcmp(text->str, "abc") || text->len != 3) return 2;
    for (int i = 0; i < 100; ++i) g_string_append_printf(text, "%s:%d;", "한글", i);
    if (text->len != strlen(text->str) || text->allocated_len <= text->len) return 3;
    if (!strstr(text->str, "한글:99;")) return 4;
    char* detached = g_string_free(text, false);
    if (strncmp(detached, "abc", 3)) return 5;
    free(detached);
    text = g_string_new(NULL);
    if (!text || text->len || text->str[0]) return 6;
    g_string_append_printf(text, "%s", "");
    if (text->len || text->str[0]) return 7;
    g_string_append_printf(text, "%s-%d", "ok", 42);
    if (strcmp(text->str, "ok-42")) return 8;
    // Reject arithmetic overflow before touching the short allocation.
    char* old = text->str;
    text->len = SIZE_MAX;
    g_string_append_printf(text, "%s", "x");
    if (text->str != old || text->len != SIZE_MAX || strcmp(text->str, "ok-42")) return 9;
    text->len = 5;
    if (g_string_free(text, true) != NULL) return 10;
    puts("GLib shim: construction, growth, detach/free, UTF-8, overflow PASS");
    return 0;
}
