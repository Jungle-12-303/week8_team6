#include "util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *sql_strdup(const char *text) {
    size_t length;
    char *copy;

    if (text == NULL) {
        return NULL;
    }

    length = strlen(text);
    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length + 1);
    return copy;
}

char *sql_strndup(const char *text, size_t length) {
    char *copy;

    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

int sql_case_equal(const char *left, const char *right) {
    size_t index = 0;

    if (left == NULL || right == NULL) {
        return 0;
    }

    while (left[index] != '\0' && right[index] != '\0') {
        if (tolower((unsigned char)left[index]) != tolower((unsigned char)right[index])) {
            return 0;
        }
        index++;
    }

    return left[index] == '\0' && right[index] == '\0';
}

void sql_free_string_array(char **items, size_t count) {
    size_t index;

    if (items == NULL) {
        return;
    }

    for (index = 0; index < count; ++index) {
        free(items[index]);
    }
    free(items);
}

int sql_string_array_contains(char *const *items, size_t count, const char *value) {
    size_t index;

    for (index = 0; index < count; ++index) {
        if (sql_case_equal(items[index], value)) {
            return 1;
        }
    }

    return 0;
}
