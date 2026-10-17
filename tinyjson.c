#include "tinyjson.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* p at opening quote -> one past the closing quote */
static const char *skip_string(const char *p) {
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

/* one past the value starting at p */
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_string(p); continue; }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') { depth--; p++; if (depth == 0) return p; continue; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

/* value start for `key` in the object at p, or NULL */
static const char *obj_get(const char *p, const char *key, size_t klen) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p++;
    for (;;) {
        p = skip_ws(p);
        if (*p == '}') return NULL;
        if (*p != '"') return NULL;
        const char *ks = p + 1;
        const char *ke = skip_string(p) - 1; /* closing quote */
        size_t len = (size_t)(ke - ks);
        p = skip_ws(ke + 1);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        if (len == klen && strncmp(ks, key, klen) == 0) return p;
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        return NULL;
    }
}

/* value start for element idx of the array at p, or NULL */
static const char *arr_get(const char *p, long idx) {
    p = skip_ws(p);
    if (*p != '[') return NULL;
    p++;
    long i = 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') return NULL;
        if (i == idx) return p;
        p = skip_value(p);
        p = skip_ws(p);
        if (*p == ',') { p++; i++; continue; }
        return NULL;
    }
}

static const char *walk(const char *p, const char *path) {
    while (*path) {
        if (*path == '.') { path++; continue; }
        if (*path == '[') {
            char *end;
            long idx = strtol(path + 1, &end, 10);
            if (*end != ']') return NULL;
            p = arr_get(p, idx);
            if (!p) return NULL;
            path = end + 1;
        } else {
            const char *dot = strpbrk(path, ".[");
            size_t len = dot ? (size_t)(dot - path) : strlen(path);
            p = obj_get(p, path, len);
            if (!p) return NULL;
            path += len;
        }
    }
    return p;
}

static char *decode_string(const char *p) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    p++;
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        char c;
        if (*p == '\\') {
            p++;
            if (*p == 'u') { p += 5; continue; } /* \uXXXX: none in our specs */
            switch (*p) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                default:  c = *p;   break;
            }
            p++;
        } else {
            c = *p++;
        }
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(out, cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
        out[len++] = c;
    }
    out[len] = '\0';
    return out;
}

char *json_get_string(const char *json, const char *path) {
    const char *v = walk(json, path);
    if (!v) return NULL;
    return decode_string(v);
}

int json_get_int(const char *json, const char *path, long *out) {
    const char *v = walk(json, path);
    if (!v) return -1;
    v = skip_ws(v);
    char *end;
    long n = strtol(v, &end, 10);
    if (end == v) return -1;
    *out = n;
    return 0;
}
