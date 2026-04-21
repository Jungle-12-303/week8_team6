#ifndef TRACE_H
#define TRACE_H

#include "ast.h"
#include "tokenizer.h"

#include <stdio.h>

void trace_write_json_string(FILE *out, const char *text);
void trace_write_token_array_json(FILE *out, const TokenArray *tokens);
void trace_write_statement_json(FILE *out, const Statement *statement);

#endif
