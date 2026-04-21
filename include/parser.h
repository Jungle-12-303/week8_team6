#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "tokenizer.h"

#include <stddef.h>

int parse_statement(const TokenArray *tokens, Statement *out_statement, char *error_buf, size_t error_buf_size);

#endif
