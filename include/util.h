#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

char *sql_strdup(const char *text);
char *sql_strndup(const char *text, size_t length);
int sql_case_equal(const char *left, const char *right);
void sql_free_string_array(char **items, size_t count);
int sql_string_array_contains(char *const *items, size_t count, const char *value);

#endif
