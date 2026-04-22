#include "database.h"

/*
 * ============================================================================
 * [Database 실행 계층 코드 리뷰용 흐름 지도]
 * ============================================================================
 *
 * 이 파일의 역할:
 * - CSV 테이블 파일(.tbl)을 읽고 쓴다.
 * - id 컬럼 기준 B+ tree 인덱스(.idx)를 열거나 재생성한다.
 * - SELECT/INSERT 실행 시 실제 row 탐색과 저장을 담당한다.
 *
 * API 요청에서 여기까지 오는 흐름:
 *
 * POST /query
 *      |
 *      v
 * db_api_execute_sql()
 *      |
 *      v
 * sql_engine_execute_sql()
 *      |
 *      +--> SELECT 이면 database_execute_select()
 *      |       |
 *      |       +--> load_table()
 *      |       +--> id WHERE 가능하면 execute_indexed_select()
 *      |       +--> 아니면 execute_linear_select()
 *      |
 *      +--> INSERT 이면 database_execute_insert()
 *              |
 *              +--> load_table()
 *              +--> build_insert_row()
 *              +--> 파일 append
 *              +--> bptree_insert()
 *
 * 인덱스 재생성 분기:
 * - users.idx 가 없거나 비어 있음
 * - users.tbl row 수와 users.idx key 수가 다름
 * - users.tbl 이 users.idx 보다 최근에 수정됨
 *
 * 위 조건이면 load_table() 에서 rebuild_index() 를 호출한다.
 * 이 로직 덕분에 기존 데이터 파일을 복사해 오거나 INSERT 테스트 후에도
 * WHERE id 검색이 오래된 offset 을 보지 않는다.
 * ============================================================================
 */

#include "bptree.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct DatabaseTable {
    char *table_name;
    char *path;
    char *index_path;
    char **columns;
    size_t column_count;
    int id_column_index;
    int next_id;
    size_t row_count;
    BPlusTree index;
};

typedef struct {
    FILE *file;
    DatabaseTable *table;
    FILE *output;
    size_t *output_indices;
    size_t output_count;
    QueryStats *stats;
    char *error_buf;
    size_t error_buf_size;
    int failed;
} IndexVisitContext;

static void reset_query_stats(QueryStats *stats) {
    if (stats == NULL) {
        return;
    }

    stats->used_index = 0;
    stats->scanned_rows = 0;
    stats->matched_rows = 0;
    stats->elapsed_ms = 0.0;
}

static int set_error(char *error_buf, size_t error_buf_size, const char *message) {
    snprintf(error_buf, error_buf_size, "%s", message);
    return 0;
}

static int make_table_path(const char *data_dir, const char *table_name, char *buffer, size_t buffer_size) {
    int written = snprintf(buffer, buffer_size, "%s/%s.tbl", data_dir, table_name);
    return written > 0 && (size_t)written < buffer_size;
}

static int make_index_path(const char *data_dir, const char *table_name, char *buffer, size_t buffer_size) {
    int written = snprintf(buffer, buffer_size, "%s/%s.idx", data_dir, table_name);
    return written > 0 && (size_t)written < buffer_size;
}

static void trim_newline(char *line) {
    size_t length = strlen(line);

    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[length - 1] = '\0';
        --length;
    }
}

static int split_csv_line(const char *line,
                          char ***out_fields,
                          size_t *out_count,
                          char *error_buf,
                          size_t error_buf_size) {
    const char *start = line;
    const char *cursor = line;
    char **fields = NULL;
    size_t count = 0;

    while (1) {
        char **new_fields;
        char *field;

        if (*cursor != ',' && *cursor != '\0') {
            ++cursor;
            continue;
        }

        new_fields = (char **)realloc(fields, (count + 1) * sizeof(char *));
        if (new_fields == NULL) {
            sql_free_string_array(fields, count);
            return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
        }
        fields = new_fields;

        field = sql_strndup(start, (size_t)(cursor - start));
        if (field == NULL) {
            sql_free_string_array(fields, count);
            return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
        }
        fields[count++] = field;

        if (*cursor == '\0') {
            break;
        }

        start = cursor + 1;
        cursor = start;
    }

    *out_fields = fields;
    *out_count = count;
    return 1;
}

static int parse_int_strict(const char *text, int *out_value) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    if (value < -2147483648L || value > 2147483647L) {
        return 0;
    }

    *out_value = (int)value;
    return 1;
}

static int find_column_index(char *const *columns, size_t column_count, const char *column_name) {
    size_t index;

    for (index = 0; index < column_count; ++index) {
        if (sql_case_equal(columns[index], column_name)) {
            return (int)index;
        }
    }

    return -1;
}

static void free_table(DatabaseTable *table) {
    if (table == NULL) {
        return;
    }

    free(table->table_name);
    free(table->path);
    free(table->index_path);
    sql_free_string_array(table->columns, table->column_count);
    bptree_destroy(&table->index);

    table->table_name = NULL;
    table->path = NULL;
    table->index_path = NULL;
    table->columns = NULL;
    table->column_count = 0;
    table->id_column_index = -1;
    table->next_id = 1;
    table->row_count = 0;
}

int database_init(Database *database, const char *data_dir, char *error_buf, size_t error_buf_size) {
    int written;

    memset(database, 0, sizeof(*database));

    written = snprintf(database->data_dir, sizeof(database->data_dir), "%s", data_dir);
    if (written <= 0 || (size_t)written >= sizeof(database->data_dir)) {
        return set_error(error_buf, error_buf_size, "data_dir 경로가 너무 깁니다.");
    }

    return 1;
}

void database_free(Database *database) {
    size_t index;

    if (database == NULL) {
        return;
    }

    for (index = 0; index < database->table_count; ++index) {
        free_table(&database->tables[index]);
    }

    free(database->tables);
    database->tables = NULL;
    database->table_count = 0;
    database->data_dir[0] = '\0';
}

static int append_table_slot(Database *database, DatabaseTable **out_table, char *error_buf, size_t error_buf_size) {
    DatabaseTable *new_tables;

    new_tables = (DatabaseTable *)realloc(database->tables, (database->table_count + 1) * sizeof(DatabaseTable));
    if (new_tables == NULL) {
        return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
    }

    database->tables = new_tables;
    memset(&database->tables[database->table_count], 0, sizeof(DatabaseTable));
    database->tables[database->table_count].id_column_index = -1;
    database->tables[database->table_count].next_id = 1;
    bptree_init(&database->tables[database->table_count].index);

    *out_table = &database->tables[database->table_count];
    ++database->table_count;
    return 1;
}

static int load_schema(FILE *file,
                       DatabaseTable *table,
                       char *error_buf,
                       size_t error_buf_size) {
    char line[8192];

    if (fgets(line, sizeof(line), file) == NULL) {
        return set_error(error_buf, error_buf_size, "테이블 파일이 비어 있습니다.");
    }

    trim_newline(line);
    if (!split_csv_line(line, &table->columns, &table->column_count, error_buf, error_buf_size)) {
        return 0;
    }

    table->id_column_index = find_column_index(table->columns, table->column_count, "id");
    if (table->id_column_index < 0) {
        return set_error(error_buf, error_buf_size, "id 컬럼이 없는 테이블에는 B+ 트리 인덱스를 만들 수 없습니다.");
    }

    return 1;
}

/*
 * .tbl 파일을 처음부터 끝까지 읽으며 id -> 파일 offset 인덱스를 만든다.
 *
 * row_offset:
 * - 해당 row 가 파일의 몇 byte 위치에서 시작하는지 나타낸다.
 * - B+ tree 에 저장해두면 WHERE id = n 검색 때 파일 전체를 스캔하지 않고
 *   그 위치로 바로 fseek 할 수 있다.
 */
static int build_index(DatabaseTable *table, FILE *file, char *error_buf, size_t error_buf_size) {
    char line[8192];

    while (1) {
        long row_offset;
        char **fields = NULL;
        size_t field_count = 0;
        int row_id;

        row_offset = ftell(file);
        if (row_offset < 0) {
            return set_error(error_buf, error_buf_size, "파일 offset을 읽는 데 실패했습니다.");
        }

        if (fgets(line, sizeof(line), file) == NULL) {
            break;
        }

        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        if (!split_csv_line(line, &fields, &field_count, error_buf, error_buf_size)) {
            return 0;
        }

        if (field_count != table->column_count) {
            sql_free_string_array(fields, field_count);
            return set_error(error_buf, error_buf_size, "행의 컬럼 수가 헤더와 다릅니다.");
        }

        if (!parse_int_strict(fields[table->id_column_index], &row_id)) {
            sql_free_string_array(fields, field_count);
            return set_error(error_buf, error_buf_size, "id 컬럼 값은 정수여야 합니다.");
        }

        if (!bptree_insert(&table->index, row_id, row_offset, error_buf, error_buf_size)) {
            sql_free_string_array(fields, field_count);
            return 0;
        }
        sql_free_string_array(fields, field_count);
    }

    table->row_count = bptree_size(&table->index);
    table->next_id = bptree_next_id(&table->index);

    return 1;
}

/*
 * 테이블 파일의 실제 row 수를 센다.
 * 기존 인덱스 key 수와 비교해서 인덱스가 최신인지 판단하기 위한 검증 단계다.
 */
static int count_table_rows(FILE *file, long data_start, size_t *out_row_count, char *error_buf, size_t error_buf_size) {
    char line[8192];
    size_t row_count = 0;

    if (fseek(file, data_start, SEEK_SET) != 0) {
        return set_error(error_buf, error_buf_size, "테이블 데이터 시작 위치로 이동하는 데 실패했습니다.");
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        trim_newline(line);
        if (line[0] != '\0') {
            ++row_count;
        }
    }

    if (ferror(file)) {
        return set_error(error_buf, error_buf_size, "테이블 row 수를 확인하는 데 실패했습니다.");
    }

    *out_row_count = row_count;
    return 1;
}

/*
 * .tbl 파일이 .idx 파일보다 나중에 수정되었는지 확인한다.
 * 테이블만 복사/수정되고 인덱스가 그대로면 offset 정보가 틀릴 수 있기 때문이다.
 */
static int table_file_newer_than_index(const char *table_path, const char *index_path) {
    struct stat table_stat;
    struct stat index_stat;

    if (stat(table_path, &table_stat) != 0 || stat(index_path, &index_stat) != 0) {
        return 1;
    }

    return table_stat.st_mtime > index_stat.st_mtime;
}

/*
 * 오래되었거나 비어 있는 인덱스를 버리고 .tbl 기준으로 다시 만든다.
 * 시연 중 "SELECT * FROM users WHERE id = 777777" 같은 검색이 실제 row 를
 * 정확히 읽어오게 만드는 안전장치다.
 */
static int rebuild_index(DatabaseTable *table, FILE *file, long data_start, char *error_buf, size_t error_buf_size) {
    /*
     * If users.tbl was copied, edited, or seeded separately from users.idx, the
     * index can point at old file offsets. Rebuild makes demos deterministic.
     */
    bptree_destroy(&table->index);
    remove(table->index_path);
    bptree_init(&table->index);

    if (!bptree_open(&table->index, table->index_path, error_buf, error_buf_size)) {
        return 0;
    }

    if (fseek(file, data_start, SEEK_SET) != 0) {
        return set_error(error_buf, error_buf_size, "인덱스 재생성을 위해 테이블 시작 위치로 이동하는 데 실패했습니다.");
    }

    return build_index(table, file, error_buf, error_buf_size);
}

/*
 * SELECT/INSERT 전에 테이블 메타데이터와 B+ tree 인덱스를 준비한다.
 *
 * 이미 로드된 테이블:
 * - database->tables 에 캐시되어 있으므로 바로 반환한다.
 *
 * 처음 보는 테이블:
 * - .tbl/.idx 경로 생성
 * - schema(header) 로드
 * - 실제 row 수와 인덱스 상태 비교
 * - 필요하면 rebuild_index()
 */
static int load_table(Database *database,
                      const char *table_name,
                      DatabaseTable **out_table,
                      char *error_buf,
                      size_t error_buf_size) {
    size_t index;
    DatabaseTable *table;
    FILE *file;
    char path[1024];
    char index_path[1024];
    long data_start;
    size_t table_row_count = 0;
    int should_rebuild_index = 0;

    for (index = 0; index < database->table_count; ++index) {
        if (sql_case_equal(database->tables[index].table_name, table_name)) {
            *out_table = &database->tables[index];
            return 1;
        }
    }

    if (!append_table_slot(database, &table, error_buf, error_buf_size)) {
        return 0;
    }

    if (!make_table_path(database->data_dir, table_name, path, sizeof(path))) {
        --database->table_count;
        return set_error(error_buf, error_buf_size, "테이블 파일 경로 조합에 실패했습니다.");
    }
    if (!make_index_path(database->data_dir, table_name, index_path, sizeof(index_path))) {
        --database->table_count;
        return set_error(error_buf, error_buf_size, "인덱스 파일 경로 조합에 실패했습니다.");
    }

    table->table_name = sql_strdup(table_name);
    table->path = sql_strdup(path);
    table->index_path = sql_strdup(index_path);
    if (table->table_name == NULL || table->path == NULL || table->index_path == NULL) {
        free_table(table);
        --database->table_count;
        return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        free_table(table);
        --database->table_count;
        snprintf(error_buf, error_buf_size, "테이블 파일을 열 수 없습니다: %s", path);
        return 0;
    }

    if (!bptree_open(&table->index, table->index_path, error_buf, error_buf_size) ||
        !load_schema(file, table, error_buf, error_buf_size)) {
        fclose(file);
        free_table(table);
        --database->table_count;
        return 0;
    }

    data_start = ftell(file);
    if (data_start < 0 || !count_table_rows(file, data_start, &table_row_count, error_buf, error_buf_size)) {
        fclose(file);
        free_table(table);
        --database->table_count;
        return 0;
    }

    should_rebuild_index = bptree_size(&table->index) == 0 ||
                           bptree_size(&table->index) != table_row_count ||
                           table_file_newer_than_index(table->path, table->index_path);

    if (should_rebuild_index) {
        if (!rebuild_index(table, file, data_start, error_buf, error_buf_size)) {
            fclose(file);
            free_table(table);
            --database->table_count;
            return 0;
        }
    } else {
        table->row_count = table_row_count;
        table->next_id = bptree_next_id(&table->index);
    }

    fclose(file);
    *out_table = table;
    return 1;
}

static int build_output_indices(const DatabaseTable *table,
                                const SelectStatement *select_stmt,
                                size_t **out_indices,
                                size_t *out_count,
                                char *error_buf,
                                size_t error_buf_size) {
    size_t *indices;
    size_t count;
    size_t index;

    if (select_stmt->columns.is_star) {
        indices = (size_t *)calloc(table->column_count, sizeof(size_t));
        if (indices == NULL) {
            return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
        }
        for (index = 0; index < table->column_count; ++index) {
            indices[index] = index;
        }
        *out_indices = indices;
        *out_count = table->column_count;
        return 1;
    }

    count = select_stmt->columns.count;
    indices = (size_t *)calloc(count, sizeof(size_t));
    if (indices == NULL) {
        return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
    }

    for (index = 0; index < count; ++index) {
        int column_index = find_column_index(table->columns, table->column_count, select_stmt->columns.items[index]);
        if (column_index < 0) {
            free(indices);
            snprintf(error_buf, error_buf_size, "존재하지 않는 컬럼입니다: %s", select_stmt->columns.items[index]);
            return 0;
        }
        indices[index] = (size_t)column_index;
    }

    *out_indices = indices;
    *out_count = count;
    return 1;
}

static void write_csv_projection(FILE *output,
                                 char *const *fields,
                                 const DatabaseTable *table,
                                 const size_t *indices,
                                 size_t index_count) {
    size_t index;

    if (output == NULL) {
        return;
    }

    for (index = 0; index < index_count; ++index) {
        if (index > 0) {
            fputc(',', output);
        }
        fputs(fields[indices[index]], output);
    }
    fputc('\n', output);

    (void)table;
}

static void write_header(FILE *output, const DatabaseTable *table, const size_t *indices, size_t index_count) {
    size_t index;

    if (output == NULL) {
        return;
    }

    for (index = 0; index < index_count; ++index) {
        if (index > 0) {
            fputc(',', output);
        }
        fputs(table->columns[indices[index]], output);
    }
    fputc('\n', output);
}

static int compare_ordered(int comparison, CompareOperator op) {
    switch (op) {
        case COMPARE_EQUALS:
            return comparison == 0;
        case COMPARE_NOT_EQUALS:
            return comparison != 0;
        case COMPARE_LESS_THAN:
            return comparison < 0;
        case COMPARE_LESS_THAN_OR_EQUAL:
            return comparison <= 0;
        case COMPARE_GREATER_THAN:
            return comparison > 0;
        case COMPARE_GREATER_THAN_OR_EQUAL:
            return comparison >= 0;
    }
    return 0;
}

static int row_matches_filter(char *const *fields,
                              const DatabaseTable *table,
                              const WhereClause *where,
                              char *error_buf,
                              size_t error_buf_size) {
    int column_index;

    column_index = find_column_index(table->columns, table->column_count, where->column_name);
    if (column_index < 0) {
        snprintf(error_buf, error_buf_size, "존재하지 않는 컬럼입니다: %s", where->column_name);
        return -1;
    }

    if (where->value.type == LITERAL_NUMBER) {
        int left_value;
        int right_value;
        if (!parse_int_strict(fields[column_index], &left_value) ||
            !parse_int_strict(where->value.text, &right_value)) {
            snprintf(error_buf, error_buf_size, "숫자 비교에는 정수 값이 필요합니다.");
            return -1;
        }
        return compare_ordered((left_value > right_value) - (left_value < right_value), where->op);
    }

    return compare_ordered(strcmp(fields[column_index], where->value.text), where->op);
}

static int read_row_at_offset(FILE *file,
                              long offset,
                              char ***out_fields,
                              size_t *out_count,
                              char *error_buf,
                              size_t error_buf_size) {
    char line[8192];

    if (fseek(file, offset, SEEK_SET) != 0) {
        return set_error(error_buf, error_buf_size, "인덱스가 가리키는 위치로 이동하는 데 실패했습니다.");
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        return set_error(error_buf, error_buf_size, "인덱스가 가리키는 행을 읽는 데 실패했습니다.");
    }

    trim_newline(line);
    return split_csv_line(line, out_fields, out_count, error_buf, error_buf_size);
}

static int index_visit_callback(int key, long value, void *context) {
    IndexVisitContext *visit_context = (IndexVisitContext *)context;
    char **fields = NULL;
    size_t field_count = 0;

    (void)key;

    if (!read_row_at_offset(visit_context->file,
                            value,
                            &fields,
                            &field_count,
                            visit_context->error_buf,
                            visit_context->error_buf_size)) {
        visit_context->failed = 1;
        return 0;
    }

    if (field_count != visit_context->table->column_count) {
        sql_free_string_array(fields, field_count);
        snprintf(visit_context->error_buf, visit_context->error_buf_size, "인덱스가 읽어온 행의 컬럼 수가 잘못되었습니다.");
        visit_context->failed = 1;
        return 0;
    }

    write_csv_projection(visit_context->output,
                         fields,
                         visit_context->table,
                         visit_context->output_indices,
                         visit_context->output_count);

    if (visit_context->stats != NULL) {
        ++visit_context->stats->scanned_rows;
        ++visit_context->stats->matched_rows;
    }

    sql_free_string_array(fields, field_count);
    return 1;
}

/*
 * WHERE id 조건이 있을 때 B+ tree 로 빠르게 찾는 SELECT 경로.
 *
 * 흐름:
 * 1. WHERE 값(search_key)을 정수로 파싱
 * 2. B+ tree 에서 조건에 맞는 id key 탐색
 * 3. 저장된 파일 offset 으로 fseek
 * 4. row 를 읽어 필요한 컬럼만 CSV 로 출력
 */
static int execute_indexed_select(DatabaseTable *table,
                                  const SelectStatement *select_stmt,
                                  const size_t *output_indices,
                                  size_t output_count,
                                  FILE *output,
                                  QueryStats *stats,
                                  char *error_buf,
                                  size_t error_buf_size) {
    FILE *file;
    int search_key;
    IndexVisitContext context;

    if (!parse_int_strict(select_stmt->where.value.text, &search_key)) {
        return set_error(error_buf, error_buf_size, "id 비교 값은 정수여야 합니다.");
    }

    file = fopen(table->path, "rb");
    if (file == NULL) {
        snprintf(error_buf, error_buf_size, "테이블 파일을 열 수 없습니다: %s", table->path);
        return 0;
    }

    memset(&context, 0, sizeof(context));
    context.file = file;
    context.table = table;
    context.output = output;
    context.output_indices = (size_t *)output_indices;
    context.output_count = output_count;
    context.stats = stats;
    context.error_buf = error_buf;
    context.error_buf_size = error_buf_size;

    if (stats != NULL) {
        stats->used_index = 1;
    }

    write_header(output, table, output_indices, output_count);
    if (!bptree_visit(&table->index,
                      select_stmt->where.op,
                      search_key,
                      index_visit_callback,
                      &context,
                      error_buf,
                      error_buf_size) ||
        context.failed) {
        fclose(file);
        return 0;
    }

    fclose(file);
    return 1;
}

/*
 * 인덱스를 쓸 수 없는 SELECT 경로.
 * 예: WHERE name = 'CHOI', WHERE age > 20, WHERE 없음
 *
 * 이 경우 .tbl 파일을 처음부터 끝까지 읽으며 row_matches_filter() 로 조건을 검사한다.
 */
static int execute_linear_select(DatabaseTable *table,
                                 const SelectStatement *select_stmt,
                                 const size_t *output_indices,
                                 size_t output_count,
                                 FILE *output,
                                 QueryStats *stats,
                                 char *error_buf,
                                 size_t error_buf_size) {
    FILE *file;
    char line[8192];

    file = fopen(table->path, "rb");
    if (file == NULL) {
        snprintf(error_buf, error_buf_size, "테이블 파일을 열 수 없습니다: %s", table->path);
        return 0;
    }

    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return set_error(error_buf, error_buf_size, "테이블 파일이 비어 있습니다.");
    }

    write_header(output, table, output_indices, output_count);

    while (fgets(line, sizeof(line), file) != NULL) {
        char **fields = NULL;
        size_t field_count = 0;
        int matches = 1;

        trim_newline(line);
        if (line[0] == '\0') {
            continue;
        }

        if (!split_csv_line(line, &fields, &field_count, error_buf, error_buf_size)) {
            fclose(file);
            return 0;
        }

        if (field_count != table->column_count) {
            sql_free_string_array(fields, field_count);
            fclose(file);
            return set_error(error_buf, error_buf_size, "행의 컬럼 수가 헤더와 다릅니다.");
        }

        if (stats != NULL) {
            ++stats->scanned_rows;
        }

        if (select_stmt->has_where) {
            matches = row_matches_filter(fields, table, &select_stmt->where, error_buf, error_buf_size);
            if (matches < 0) {
                sql_free_string_array(fields, field_count);
                fclose(file);
                return 0;
            }
        }

        if (matches) {
            write_csv_projection(output, fields, table, output_indices, output_count);
            if (stats != NULL) {
                ++stats->matched_rows;
            }
        }

        sql_free_string_array(fields, field_count);
    }

    fclose(file);
    return 1;
}

/*
 * SELECT 문의 최상위 실행 함수.
 *
 * 분기:
 * - WHERE id ... 형태이면 B+ tree 인덱스 사용
 * - 그 외 조건이면 파일 전체 scan
 *
 * stats->used_index/scanned_rows/matched_rows 값은 API 응답의 stats 로 전달된다.
 */
int database_execute_select(Database *database,
                            const SelectStatement *select_stmt,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size) {
    DatabaseTable *table;
    size_t *output_indices = NULL;
    size_t output_count = 0;
    int use_index = 0;

    reset_query_stats(stats);

    if (!load_table(database, select_stmt->table_name, &table, error_buf, error_buf_size)) {
        return 0;
    }

    if (!build_output_indices(table, select_stmt, &output_indices, &output_count, error_buf, error_buf_size)) {
        return 0;
    }

    if (select_stmt->has_where &&
        sql_case_equal(select_stmt->where.column_name, "id") &&
        table->id_column_index >= 0) {
        use_index = 1;
    }

    if (use_index) {
        int ok = execute_indexed_select(table,
                                        select_stmt,
                                        output_indices,
                                        output_count,
                                        output,
                                        stats,
                                        error_buf,
                                        error_buf_size);
        free(output_indices);
        return ok;
    }

    if (stats != NULL) {
        stats->used_index = 0;
    }
    {
        int ok = execute_linear_select(table,
                                       select_stmt,
                                       output_indices,
                                       output_count,
                                       output,
                                       stats,
                                       error_buf,
                                       error_buf_size);
        free(output_indices);
        return ok;
    }
}

static int validate_storage_value(const char *value, char *error_buf, size_t error_buf_size) {
    if (strchr(value, ',') != NULL || strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
        return set_error(error_buf, error_buf_size, "현재 저장 포맷에서는 값 안에 콤마나 줄바꿈을 넣을 수 없습니다.");
    }
    return 1;
}

static int assign_auto_id_literal(Literal *literal, int id_value, char *error_buf, size_t error_buf_size) {
    char buffer[32];

    literal->type = LITERAL_NUMBER;
    snprintf(buffer, sizeof(buffer), "%d", id_value);
    literal->text = sql_strdup(buffer);
    if (literal->text == NULL) {
        return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
    }
    return 1;
}

static void free_literal_array(Literal *values, size_t count) {
    size_t index;

    if (values == NULL) {
        return;
    }

    for (index = 0; index < count; ++index) {
        free(values[index].text);
    }
    free(values);
}

/*
 * INSERT 문에 들어온 column/value 목록을 실제 테이블 컬럼 순서에 맞게 재배열한다.
 *
 * 지원하는 형태:
 * - INSERT INTO users VALUES (...)
 * - INSERT INTO users (name, email, age) VALUES (...)
 *
 * id 가 생략되면 table->next_id 로 자동 부여한다.
 */
static int build_insert_row(const DatabaseTable *table,
                            const InsertStatement *insert_stmt,
                            Literal **out_values,
                            int *out_id_value,
                            char *error_buf,
                            size_t error_buf_size) {
    Literal *values;
    int provided_id = 0;
    int id_value = table->next_id;
    size_t index;
    size_t value_index = 0;

    values = (Literal *)calloc(table->column_count, sizeof(Literal));
    if (values == NULL) {
        return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
    }

    if (insert_stmt->columns.count == 0) {
        if (insert_stmt->value_count == table->column_count) {
            for (index = 0; index < table->column_count; ++index) {
                values[index] = clone_literal(&insert_stmt->values[index]);
                if (values[index].text == NULL) {
                    free_literal_array(values, table->column_count);
                    return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                }
            }
            provided_id = 1;
        } else if (insert_stmt->value_count == table->column_count - 1) {
            for (index = 0; index < table->column_count; ++index) {
                if ((int)index == table->id_column_index) {
                    if (!assign_auto_id_literal(&values[index], id_value, error_buf, error_buf_size)) {
                        free_literal_array(values, table->column_count);
                        return 0;
                    }
                    continue;
                }

                values[index] = clone_literal(&insert_stmt->values[value_index++]);
                if (values[index].text == NULL) {
                    free_literal_array(values, table->column_count);
                    return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                }
            }
        } else {
            free(values);
            snprintf(error_buf,
                     error_buf_size,
                     "INSERT 값 개수(%zu)가 테이블 컬럼 수(%zu) 또는 자동 id 제외 수(%zu)와 맞지 않습니다.",
                     insert_stmt->value_count,
                     table->column_count,
                     table->column_count - 1);
            return 0;
        }
    } else {
        if (insert_stmt->columns.count != insert_stmt->value_count) {
            free(values);
            return set_error(error_buf, error_buf_size, "INSERT 컬럼 목록과 값 개수가 다릅니다.");
        }

        for (index = 0; index < insert_stmt->columns.count; ++index) {
            int column_index = find_column_index(table->columns, table->column_count, insert_stmt->columns.items[index]);
            if (column_index < 0) {
                free_literal_array(values, table->column_count);
                snprintf(error_buf, error_buf_size, "존재하지 않는 컬럼입니다: %s", insert_stmt->columns.items[index]);
                return 0;
            }

            values[column_index] = clone_literal(&insert_stmt->values[index]);
            if (values[column_index].text == NULL) {
                free_literal_array(values, table->column_count);
                return set_error(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
            }
        }

        if (values[table->id_column_index].text != NULL) {
            provided_id = 1;
        } else if (!assign_auto_id_literal(&values[table->id_column_index], id_value, error_buf, error_buf_size)) {
            free_literal_array(values, table->column_count);
            return 0;
        }

        for (index = 0; index < table->column_count; ++index) {
            if (values[index].text == NULL) {
                free_literal_array(values, table->column_count);
                snprintf(error_buf, error_buf_size, "INSERT 에 필요한 컬럼이 누락되었습니다: %s", table->columns[index]);
                return 0;
            }
        }
    }

    if (!parse_int_strict(values[table->id_column_index].text, &id_value)) {
        free_literal_array(values, table->column_count);
        return set_error(error_buf, error_buf_size, "id 값은 정수여야 합니다.");
    }

    if (provided_id && id_value >= table->next_id) {
        /* next_id 는 insert 성공 뒤에 갱신 */
    }

    *out_values = values;
    *out_id_value = id_value;
    return 1;
}

/*
 * INSERT 문의 최상위 실행 함수.
 *
 * 흐름:
 * 1. load_table() 로 테이블/인덱스 준비
 * 2. build_insert_row() 로 저장할 row 완성
 * 3. id 중복 여부를 B+ tree 에서 확인
 * 4. .tbl 파일 끝에 row append
 * 5. 새 id -> 파일 offset 을 B+ tree 에 추가
 * 6. next_id/row_count 갱신
 */
int database_execute_insert(Database *database,
                            const InsertStatement *insert_stmt,
                            FILE *output,
                            QueryStats *stats,
                            char *error_buf,
                            size_t error_buf_size) {
    DatabaseTable *table;
    Literal *ordered_values = NULL;
    int inserted_id;
    FILE *file;
    long row_offset;
    size_t index;

    reset_query_stats(stats);

    if (!load_table(database, insert_stmt->table_name, &table, error_buf, error_buf_size)) {
        return 0;
    }

    if (!build_insert_row(table, insert_stmt, &ordered_values, &inserted_id, error_buf, error_buf_size)) {
        return 0;
    }

    if (bptree_search(&table->index, inserted_id, NULL, error_buf, error_buf_size)) {
        free_literal_array(ordered_values, table->column_count);
        snprintf(error_buf, error_buf_size, "중복된 id 키입니다: %d", inserted_id);
        return 0;
    }

    for (index = 0; index < table->column_count; ++index) {
        if (!validate_storage_value(ordered_values[index].text, error_buf, error_buf_size)) {
            free_literal_array(ordered_values, table->column_count);
            return 0;
        }
    }

    file = fopen(table->path, "ab");
    if (file == NULL) {
        free_literal_array(ordered_values, table->column_count);
        snprintf(error_buf, error_buf_size, "테이블 파일을 append 모드로 열 수 없습니다: %s", table->path);
        return 0;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        free_literal_array(ordered_values, table->column_count);
        return set_error(error_buf, error_buf_size, "파일 끝으로 이동하는 데 실패했습니다.");
    }

    row_offset = ftell(file);
    if (row_offset < 0) {
        fclose(file);
        free_literal_array(ordered_values, table->column_count);
        return set_error(error_buf, error_buf_size, "삽입 위치를 기록하는 데 실패했습니다.");
    }

    for (index = 0; index < table->column_count; ++index) {
        if (index > 0) {
            fputc(',', file);
        }
        fputs(ordered_values[index].text, file);
    }
    fputc('\n', file);
    fclose(file);

    if (!bptree_insert(&table->index, inserted_id, row_offset, error_buf, error_buf_size)) {
        free_literal_array(ordered_values, table->column_count);
        return 0;
    }

    ++table->row_count;
    if (inserted_id >= table->next_id) {
        table->next_id = inserted_id + 1;
    }

    if (output != NULL) {
        fprintf(output, "Inserted 1 row into %s (id=%d)\n", table->table_name, inserted_id);
    }

    free_literal_array(ordered_values, table->column_count);
    return 1;
}
