#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include "ast.h"

#include <stddef.h>

int optimize_statement(Statement *statement, char *error_buf, size_t error_buf_size);

#endif
