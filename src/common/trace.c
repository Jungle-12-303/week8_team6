#include "trace.h"

#include <stdio.h>

static const char *literal_type_name(LiteralType type) {
    return type == LITERAL_NUMBER ? "number" : "string";
}

void trace_write_json_string(FILE *out, const char *text) {
    const unsigned char *cursor = (const unsigned char *)(text == NULL ? "" : text);

    fputc('"', out);
    while (*cursor != '\0') {
        switch (*cursor) {
            case '\\':
                fputs("\\\\", out);
                break;
            case '"':
                fputs("\\\"", out);
                break;
            case '\n':
                fputs("\\n", out);
                break;
            case '\r':
                fputs("\\r", out);
                break;
            case '\t':
                fputs("\\t", out);
                break;
            default:
                if (*cursor < 32) {
                    fprintf(out, "\\u%04x", *cursor);
                } else {
                    fputc(*cursor, out);
                }
                break;
        }
        cursor++;
    }
    fputc('"', out);
}

static void trace_write_literal_json(FILE *out, const Literal *literal) {
    fputs("{\"type\":", out);
    trace_write_json_string(out, literal_type_name(literal->type));
    fputs(",\"text\":", out);
    trace_write_json_string(out, literal->text);
    fputc('}', out);
}

static void trace_write_column_list_json(FILE *out, const ColumnList *columns) {
    size_t index;

    fputs("{\"is_star\":", out);
    fputs(columns->is_star ? "true" : "false", out);
    fputs(",\"count\":", out);
    fprintf(out, "%zu", columns->count);
    fputs(",\"items\":[", out);
    for (index = 0; index < columns->count; ++index) {
        if (index > 0) {
            fputc(',', out);
        }
        trace_write_json_string(out, columns->items[index]);
    }
    fputs("]}", out);
}

void trace_write_token_array_json(FILE *out, const TokenArray *tokens) {
    size_t index;

    fputc('[', out);
    if (tokens != NULL) {
        for (index = 0; index < tokens->count; ++index) {
            if (index > 0) {
                fputc(',', out);
            }
            fputs("{\"index\":", out);
            fprintf(out, "%zu", index);
            fputs(",\"type\":", out);
            trace_write_json_string(out, token_type_name(tokens->items[index].type));
            fputs(",\"lexeme\":", out);
            trace_write_json_string(out, tokens->items[index].lexeme);
            fputs(",\"position\":", out);
            fprintf(out, "%zu", tokens->items[index].position);
            fputc('}', out);
        }
    }
    fputc(']', out);
}

void trace_write_statement_json(FILE *out, const Statement *statement) {
    if (statement == NULL) {
        fputs("null", out);
        return;
    }

    if (statement->type == AST_SELECT_STATEMENT) {
        fputs("{\"type\":\"select\",\"table_name\":", out);
        trace_write_json_string(out, statement->as.select_stmt.table_name);
        fputs(",\"columns\":", out);
        trace_write_column_list_json(out, &statement->as.select_stmt.columns);
        fputs(",\"has_where\":", out);
        fputs(statement->as.select_stmt.has_where ? "true" : "false", out);
        fputs(",\"where\":", out);
        if (statement->as.select_stmt.has_where) {
            fputs("{\"column_name\":", out);
            trace_write_json_string(out, statement->as.select_stmt.where.column_name);
            fputs(",\"value\":", out);
            trace_write_literal_json(out, &statement->as.select_stmt.where.value);
            fputc('}', out);
        } else {
            fputs("null", out);
        }
        fputc('}', out);
        return;
    }

    fputs("{\"type\":\"insert\",\"table_name\":", out);
    trace_write_json_string(out, statement->as.insert_stmt.table_name);
    fputs(",\"columns\":", out);
    trace_write_column_list_json(out, &statement->as.insert_stmt.columns);
    fputs(",\"value_count\":", out);
    fprintf(out, "%zu", statement->as.insert_stmt.value_count);
    fputs(",\"values\":[", out);
    for (size_t index = 0; index < statement->as.insert_stmt.value_count; ++index) {
        if (index > 0) {
            fputc(',', out);
        }
        trace_write_literal_json(out, &statement->as.insert_stmt.values[index]);
    }
    fputs("]}", out);
}
