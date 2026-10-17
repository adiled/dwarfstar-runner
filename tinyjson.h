#ifndef TINYJSON_H
#define TINYJSON_H

/*
 * Minimal read-only JSON path lookup, enough to pull a few fields out of
 * `orch parse` output. Not a general parser: it resolves one path against a
 * document already in memory.
 *
 *   json_get_string("{\"a\":[{\"b\":\"hi\"}]}", "a[0].b")  ->  "hi"
 */

/* Returns a malloc'd, unescaped string (caller frees) or NULL. */
char *json_get_string(const char *json, const char *path);

/* Returns 0 and stores the integer, or -1 if the path is missing/not numeric. */
int json_get_int(const char *json, const char *path, long *out);

#endif /* TINYJSON_H */
