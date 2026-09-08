//
// Created by nhp on 14-05-24.
//

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#endif

#include "glib.h"

// Some C23 compilers still ship an older C library (notably Apple toolchains).
static bool size_add(gsize* out, gsize a, gsize b) {
#ifdef ckd_add
    return ckd_add(out, a, b);
#else
    if (a > SIZE_MAX - b) return true;
    *out = a + b;
    return false;
#endif
}

static bool size_double(gsize* out, gsize value) {
#ifdef ckd_mul
    return ckd_mul(out, value, 2);
#else
    return size_add(out, value, value);
#endif
}

GString* g_string_new(gchar* initial) {
    GString* str = g_new0(GString, 1);
    if (!str) return NULL;
    str->len = initial ? strlen(initial) : 0;
    if (size_add(&str->allocated_len, str->len, 1)) { free(str); return NULL; }
    str->str = malloc(str->allocated_len);
    if (!str->str) { free(str); return NULL; }
    if (str->len) memcpy(str->str, initial, str->len);
    str->str[str->len] = '\0';
    return str;
}

gchar* g_string_free(GString* str, gboolean free_segment) {
    char* seg = str->str;
    free(str);
    if (free_segment) {
        free(seg);
        return NULL;
    }
    return seg;
}

void g_string_append_printf(GString* str, const gchar* format, ...) {
    va_list args;
    va_start(args, format);
    int need_len = vsnprintf(NULL, 0, format, args);
    va_end(args);
    gsize required;
    if (need_len < 0 || size_add(&required, str->len, (gsize)need_len) ||
        size_add(&required, required, 1)) return;
    // Format before realloc: callers may pass the current string as an argument.
    gchar* temp = malloc((gsize)need_len + 1);
    if (!temp) return;
    va_start(args, format);
    int written = vsnprintf(temp, (gsize)need_len + 1, format, args);
    va_end(args);
    if (written != need_len) { free(temp); return; }
    if (required > str->allocated_len) {
        gsize capacity;
        if (size_double(&capacity, str->allocated_len) || capacity < required)
            capacity = required;
        gchar* newp = realloc(str->str, capacity);
        if (!newp) { free(temp); return; }
        str->str = newp;
        str->allocated_len = capacity;
    }
    memcpy(str->str + str->len, temp, (gsize)need_len + 1);
    str->len = required - 1;
    free(temp);
}

gchar* g_strstr_len(const gchar* haystack, int len, const gchar* needle) {
    if (len == -1) return strstr(haystack, needle);
    size_t needle_len = strlen(needle);
    for (int i = 0; i < len; i++) {
        size_t found = 0;
        for (int j = i; j < len; j++) {
            if (haystack[j] == needle[j - i]) found++;
            else break;
            if (found == needle_len) return (gchar*) haystack + j;
        }
    }
    return NULL;
}

gchar* g_strdup(const gchar* str) {
    if (str == NULL) return NULL;
    else return strdup(str);
}

int g_strv_length(GStrv strings) {
    gint count = 0;
    while (strings[count])
        count++;
    return count;
}

void g_strfreev(GStrv strings) {
    for (int i = 0; strings[i] != NULL; i++) {
        free(strings[i]);
    }
}

// This is not good but all we're using slirp for is beaming pokemans over the internet so it's probably okay
gint g_rand_int_range(GRand* grand, gint min, gint max) {
    double r = (double) rand();
    double range = (double) (max - min);
    double r2 = (r / (double) RAND_MAX) * range;
    return MIN(max, ((int) r2) + min);
}

GRand* g_rand_new() {
    return malloc(sizeof(GRand));
}

void g_rand_free(GRand* rand) {
    free(rand);
}

void g_error_free(GError* error) {
    free(error);
}

gboolean g_shell_parse_argv(const gchar* command_line, gint* argcp, gchar*** argvp, GError** error) {
    const gchar* message = "Unimplemented.";
    GError* err = malloc(sizeof(GError));
    err->message = message;
    *error = err;
    return false;
}

gboolean g_spawn_async_with_fds(const gchar *working_directory, gchar **argv,
                                gchar **envp, GSpawnFlags flags,
                                GSpawnChildSetupFunc child_setup,
                                gpointer user_data, GPid *child_pid, gint stdin_fd,
                                gint stdout_fd, gint stderr_fd, GError **error)
{
    return false;
}
