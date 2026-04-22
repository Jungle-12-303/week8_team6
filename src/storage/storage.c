#include "storage.h" // 방금 작성한 스토리지 스키마 뼈대 헤더 파일을 가장 위로 가져옵니다.

#include "util.h" // 문자열 제어나 대소문자 무시 비교, 메모리 관리를 도와줄 도구함 유틸리티 헤더를 가져옵니다.

#include <stdio.h>  // fopen, fputs, fget 등 C언어에서 실제 하드 디스크 파일을 다루고 직접 입출력하기 위한 핵심 헤더
#include <stdlib.h> // calloc, realloc 등 가변 길이를 갖는 배열 동적 메모리를 다루기 위해 포함하는 헤더
#include <string.h> // 문자열 분리나 복사, 메모리 공간 블록 전체를 한 번에 덮어쓰기 위한 모듈 헤더

// 콤마(,) 등 특정 구분자로 잘라낸 단어들(텍스트 파편들)을 순서대로 일렬로 나열해 보관할 간단한 문자열 포켓 구조체
typedef struct {
    char **items; // 문자열을 저장한 포인터의 집합, 즉 문자열 배열.
    size_t count; // 배결 칸수에 몇개의 단어들이 무사히 들어앉아 있는지 세는 카운터
} StringList;

// 위 포켓 구조체 내부의 텍스트와 뼈대들을 싸그리 폐기시키고 메모리를 비워주는 전용 청소부 보조 함수
static void free_string_list(StringList *list) {
    if (list == NULL) { // 지우라고 던져준 리스트가 텅 빈 공기라면
        return;         // 즉시 복귀
    }

    sql_free_string_array(list->items, list->count); // 공용 유틸함수에 배열과 개수를 넘겨 내부 아이템의 알맹이를 전부 지옥으로 날려줍니다.
    list->items = NULL; // 뼈대를 가리키던 포인터를 분질러 널로 고정
    list->count = 0;    // 들어있는 정보 개수도 하나도 남김없이 0으로 깨끗이 리셋
}

// "data" 폴더와 "users" 테이블명을 합쳐 -> "data/users.tbl" 이라는 물리적인 파일 진짜 경로값을 만들어 문자열로 조합해내는 믹서 함수
static int make_table_path(const char *data_dir, const char *table_name, char *buffer, size_t buffer_size) {
    // snprintf 를 써서 %s/%s.tbl 이라는 규격 폼에 디렉토리명과 테이블명을 각각 끼워넣어 버퍼에 찍어냅니다.
    int written = snprintf(buffer, buffer_size, "%s/%s.tbl", data_dir, table_name); 
    // 글자를 쓴 길이가 0보다 크고, 버퍼 한도 사이즈보다 작아 넘치지 않게 안전히 성립되었다면 성공(1반환) 표기
    return written > 0 && (size_t)written < buffer_size;
}

// 파일에서 문장을 읽었을 때 눈에 보이지 않게 뒤에 딸려오는 귀찮은 찌꺼기인 엔터표시(개행 기호: \n, \r) 문자들을 싹둑 잘라 없애주는 다림질 함수
static void trim_newline(char *line) {
    size_t length = strlen(line); // 받아온 글 전체 길이를 먼저 잰 뒤
    // 글의 맨 끝부분이 엔터(\n) 나 캐리지리턴(\r) 형식으로 이루어진 공백인 동안은 무한 스킵 회전하며
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[length - 1] = '\0'; // 해당 쓸모없는 개행 위치에 강제로 글자 종결 기호('\0')를 덮어씌워 텍스트를 한칸 줄여버립니다.
        length--; // 확인 길이를 1 줄이고 다시 전 칸을 검사하러 갑니다.
    }
}

// 파일에서 1줄단위의 긴 텍스트를 읽어들인 뒤 그 안에 숨겨진 콤마(,)를 기준으로 각각의 단어들을 똑똑 잘라서 리스트 구조체로 담아주는 CSV 전용 칼잡이 함수
static int split_csv_line(const char *line, StringList *out_list) {
    const char *start = line;  // 현재 자르기 시작할 단어의 첫 간판 위치를 나타내는 커서
    const char *cursor = line; // 한 글자씩 우측으로 밀면서 콤마나 끝부분인지 탐색할 움직이는 칼날 커서
    char **new_items;          // 리스트 배결을 증축할때 쓸 포인터
    char *field;               // 칼로 잘라낸 1단어(필드)의 임시보관용

    memset(out_list, 0, sizeof(*out_list)); // 남은 쓰레기가 없도록 반환할 출력 리스트 상자를 락스청소

    while (1) { // 무한 검사루프 가동
        // 만일 탐색 커서가 가리킨 자리가 단어를 자르는 '콤마(,)' 이거나 아니면 이 줄 전체의 진짜 마지막 끝부분('\0')에 도달했다면!
        if (*cursor == ',' || *cursor == '\0') {
            new_items = (char **)realloc(out_list->items, (out_list->count + 1) * sizeof(char *)); // 리스트에 새 단어를 담기 위해 짐칸을 1칸 더 재할당(realloc) 증축
            if (new_items == NULL) { // 실패 시
                free_string_list(out_list); // 만들었던 리스트 산산조각 내 해제
                return 0; // 그리고 폭파 실패 종료
            }
            out_list->items = new_items; // 성공 증축시 주소 갱신

            field = sql_strndup(start, (size_t)(cursor - start)); // 시작 간판부터 쭉 이동했던 탐색 커서까지 거리(길이)만큼의 알맹이 단어를 도려내어 새 위치에 깊은 복사해 포장합니다.
            if (field == NULL) { // 메모리 문제라면
                free_string_list(out_list); // 산산조각
                return 0; // 폭파 실패 종료
            }

            out_list->items[out_list->count] = field; // 빈 끄트머리 짐칸에 방금 무사히 도려내어 포장한 알맹이 단어(컬럼/값 데이터)를 탁 싣습니다!
            out_list->count++; // 하나 실렸으니 개수 카운터 증가!

            if (*cursor == '\0') { // 만일 방금 자른 곳이 콤마가 아니라 아예 글줄 전체의 맨 마지막 종단 끝점('\0') 이었다면
                break; // 이 줄에는 더 이상 뒤질 게 없으니 무한 루프 탈출명령 수행
            }

            cursor++; // 아직 콤마여서 뒤에 더 내용이 있다면 일단 먼저 탐색 커서를 콤마 뒤 한 칸 이동시킵니다.
            start = cursor; // 이제 여기부터 시작 커서야! 하고 새로운 단어의 시작 간판을 세워 이어 검사합니다.
            continue; // 아래코드 스킵하고 루프 다시 반복
        }
        cursor++; // 아무 특이점이 없는 일반 글자였다면 그냥 쓱싹 탐색 칼날만 1칸 이동시킵니다.
    }

    return 1; // 무사히 1줄을 전부 쪼개 리스트화 완수(1)
}

// 전체 테이블 정보 중, 유저가 SELECT id, name 명령 등으로 '필요하다고 특정하여 요구한(required)' 컬럼들의 숫자 인덱스와 이름 목록만 간추려 솎아주는 특별 필터링 함수
static int copy_selected_columns(const StringList *header,
                                 char *const *required_columns,
                                 size_t required_count,
                                 size_t **out_indices,
                                 char ***out_columns,
                                 char *error_buf,
                                 size_t error_buf_size) {
    size_t *indices;  // 원본 테이블의 방대한 헤더 위치 중, 내가 필요한 필드가 몇번 인덱스인지 모아둘 숫자 목록
    char **columns;   // 내가 필요한 헤더 필드 이름들만 모아둘 문자열 목록
    size_t i;         // 반복문 인덱스 1
    size_t j;         // 반복문 인덱스 2
    int found;        // 일치 발견 스위치

    // 만일 유저가 특정 컬럼을 편식하지 않고 "전부 다 갖다 줘! (*)" 즉, required 요구사항이 빈 값이라면
    if (required_columns == NULL || required_count == 0) {
        indices = (size_t *)calloc(header->count, sizeof(size_t)); // 전체 헤더 갯수와 정확히 똑같은 사이즈의 숫자 방 할당
        columns = (char **)calloc(header->count, sizeof(char *));  // 전체 헤더 갯수와 정확히 똑같은 문자 방 할당
        if (indices == NULL || columns == NULL) { // 메모리 부족시
            free(indices); // 방 다 빼고 해제
            free(columns);
            snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
            return 0; // 0 반환
        }

        // 전체 컬럼 수만큼 뺑뺑이 돌면서
        for (i = 0; i < header->count; ++i) {
            indices[i] = i; // 필요한 추출 인덱스는 0, 1, 2, 3 전체 그대로 사용 (순번 필터링 없음)
            columns[i] = sql_strdup(header->items[i]); // 헤더 이름도 깡그리 싹다 그대로 복사해서 담습니다.
            if (columns[i] == NULL) { // 딥 카피 도중 용량 부족이면
                sql_free_string_array(columns, i); // 그때까지 쌓인거 다날리기
                free(indices);
                snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                return 0;
            }
        }

        *out_indices = indices; // 만들어진 수치 목록을 외부로 보고 송출
        *out_columns = columns; // 만들어진 컬럼명 목록을 외부로 보고 송출
        return 1; // "전체 컬럼 담기 모드" 성공 반환
    }

    // 아래부턴 위와 달리 SELECT 등에서 요구한 특정 컬럼이 있을 경우의 처리 구간입니다. (예외적 필터 추출 모드)
    indices = (size_t *)calloc(required_count, sizeof(size_t)); // 요구받은 숫자만큼만 작은 방 할당
    columns = (char **)calloc(required_count, sizeof(char *));  // 요구받은 숫자만큼 문자 방 할당
    if (indices == NULL || columns == NULL) {
        free(indices);
        free(columns);
        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
        return 0;
    }

    // 요구받은 컬럼의 개수(i)만큼 하나씩 돌면서 확인합니다.
    for (i = 0; i < required_count; ++i) {
        found = 0; // 이번 컬럼 찾았음? 아직(0)!
        for (j = 0; j < header->count; ++j) { // 진짜 실제 테이블이 지닌 원본 헤더들(j) 전체를 하나하나 뒤집어까봅니다
            if (sql_case_equal(required_columns[i], header->items[j])) { // 오! 내가 원하는 컬럼이 원본 테이블 헤더 속 이름이랑 정확히 일치한다면!
                indices[i] = j; // 아하, 네가 원본의 J번째 방에 있었구나. 원본 인덱스 번호를 기억, 색인 숫자로 저장
                columns[i] = sql_strdup(required_columns[i]); // 그리고 그 이름도 안전하게 딥카피 복제로 리스트에 담음!
                if (columns[i] == NULL) { // 메모리 문제로 퍼기 실패시 싹날림
                    sql_free_string_array(columns, i);
                    free(indices);
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    return 0;
                }
                found = 1; // 기분좋게 "찾았다(found) 스위치" ON!
                break; // 어차피 찾았으니 이 원본 순회 뺑뺑이는 더이상 뒤지지 않고 멈추고 탈출!
            }
        }

        // 만일 원본 뺑기를 다 돌렸는데도, 유저가 요구한 이름이 원본에 없어 결국 found 스위치가 꺼져있는 채라면 (유저 오타나 없는 컬럼 요구)
        if (!found) {
            sql_free_string_array(columns, i); // 가차없이 여태 쌓은 컬럼 데이터 파쇄
            free(indices); // 인덱스 파쇄
            snprintf(error_buf, error_buf_size, "테이블에 존재하지 않는 컬럼입니다: %s", required_columns[i]); // 없는 컬럼이라고 에러 대문짝하게 기록
            return 0; // 작업 전체 실패 선언 0 반환!
        }
    }

    *out_indices = indices; // 필터링 추출을 끝냈으니 좁아진 이 수치 목록을 외부로 이식 송출
    *out_columns = columns; // 필터링된 컬럼명 목록을 외부로 송출
    return 1; // 부분 컬럼 반환 대성공! (1)
}

// 2차원 데이터를 이룩하기 위해, 가로로 나열된 단일 행(row) 한 줄을 테이블 전체의 세로 목록 구조체 밑단에 탁 붙여 추가(Append)해 주는 기능 함수
static int append_row(TableData *table, char **values) {
    char ***new_rows; // 2차원 배열 공간의 바닥을 더 넓히기 위한 3중 거인 포인터

    new_rows = (char ***)realloc(table->rows, (table->row_count + 1) * sizeof(char **)); // 줄을 담는 바스켓의 용량을 +1 확장 재할당 공사
    if (new_rows == NULL) { // 예산 부족(메모리 부족) 시 
        return 0; // 공사 실패 처리
    }

    table->rows = new_rows; // 확장 공사에 성공한 넓은 바스켓 주소로 이정표 이전
    table->rows[table->row_count] = values; // 확장된 맨 끝단 빈 바닥 인덱스에, 방금 받아온 새로운 가로줄(values) 한 세트를 고대로 통째 배치!
    table->row_count++; // 줄이 하나 늘었으니 개수 카운터 증가!
    return 1; // 성공 보고
}

// 전체 과정 중 제일 묵직한 함수: 디스크(tbl 파일)에 잠들어있는 데이터를 전부 뚜껑 열어서 테이블 구조체 메모리로 이사(Load) 시켜 복원하는 로더 총괄 
int storage_load_table(const char *data_dir,
                       const char *table_name,
                       char *const *required_columns,
                       size_t required_count,
                       TableData *out_table,
                       char *error_buf,
                       size_t error_buf_size) {
    char path[1024]; // 디스크 파일 위치 조합 문자열 변수
    FILE *file; // C언어 파일 제어 구조체
    char line[4096]; // 파일 속의 세로 값 딱 1줄을 임시로 크게 떠 퍼담을 국자 버퍼
    StringList header = {0}; // 첫 번째 줄일 헤더만 쏙 담아둘 임시 통
    StringList row = {0}; // 나머지 내용물 단어 하나하나를 담을 임시 통
    size_t *selected_indices = NULL; // 추출용 필터 인덱스
    size_t selected_count; // 추출할 개수
    size_t row_index; // 반복용 줄 번호
    char **selected_row = NULL; // 실제 가라앉힐 정제된 1줄 행

    memset(out_table, 0, sizeof(*out_table)); // 외부로 전달해줄 결과물 테이블 공간 먼지 청소 백지장화!

    // 우선 어느 파일을 깔지 경로를 조합합니다 (예: "data/users.tbl")
    if (!make_table_path(data_dir, table_name, path, sizeof(path))) {
        snprintf(error_buf, error_buf_size, "테이블 파일 경로 조합, 생성에 실패했습니다."); // 글자수 넘치면 버그 에러
        return 0;
    }

    // 실제로 경로를 C내장함수(fopen)로 던져 하드디스크의 .tbl 파일을 읽기(r) 전용 형태로 잡고 포인터를 엽니다
    file = fopen(path, "r");
    if (file == NULL) { // 파일 권한 없음이거나 경로에 아예 파일이 안 존재 한다면!
        snprintf(error_buf, error_buf_size, "물리적 테이블 파일을 디스크에서 발견하여 열 수 없습니다: %s", path);
        return 0; // 파일 로드 실패 종료!
    }

    // 파일이 무사히 열렸으면 국자(fgets)를 푹 담궈 먼저 엔터를 치기 전 대망의 파일 첫 1줄 (즉, 컬럼 헤더 부분) 을 퍼냅니다
    if (fgets(line, sizeof(line), file) == NULL) { 
        fclose(file); // 근데 첫 줄부터 건져올릴게 없이 텅 비었다면 그건 빈껍데기 쓰레기 테이블입니다
        snprintf(error_buf, error_buf_size, "테이블 파일 안이 완전히 텅 비어 있습니다: %s", path);
        return 0; // 파일 수거 폐기 및 오류 종료
    }

    trim_newline(line); // 퍼온 헤더 문장의 꼬리에 붙어 자꾸 걸리적거리는 귀찮은 엔터 줄바꿈 특수기호를 날카롭게 다림질해 없앱니다
    if (!split_csv_line(line, &header)) { // 콤마로 잘라내는 csv 파워 칼잡이를 동원해 헤어의 단어 토막을 분리시킵니다
        fclose(file); // 쪼개다 컴 오류시 파일 다시 닫고
        snprintf(error_buf, error_buf_size, "헤더 정보를 파싱해 쪼개는 과정에 실패했습니다.");
        return 0; // 실패.
    }

    // 만약 사용자가 (id, name 등) 특정 컬럼만 가져오길 원할 수 있으니 아까 만든 필터 솎아내기 함수를 가동해 사용할 컬럼과 위치 번호만 솎아내 옵니다
    if (!copy_selected_columns(&header, required_columns, required_count, &selected_indices, &out_table->columns, error_buf, error_buf_size)) {
        fclose(file);  // 요구 컬럼이 개판이면 파일 닫아버리고
        free_string_list(&header); // 잡았던 헤더도 박살
        return 0; // 실패.
    }
    // 컬럼 솎기가 무사히 끝났다면 유저가 요구한 숫자를, 혹은 전원이라면 헤더 개수 자체를 결과 테이블의 총 기둥 갯수로 엄숙히 확정 지어줍니다
    out_table->column_count = (required_columns == NULL || required_count == 0) ? header.count : required_count;
    selected_count = out_table->column_count; // 빠른 로직 조회를 위해 별도 변수에도 저장

    // 이제 진짜 핵심: 위에 한 줄 제외하고 그 아래에 남아있는 진짜 데이터 행들을 끝까지 1줄씩 거대한 국자로 뜰 수 있는 만큼 계속 푹푹 퍼담으며(while 루프) 처리합니다!
    while (fgets(line, sizeof(line), file) != NULL) {
        trim_newline(line); // 뽑은 1줄 데이터 끝의 골치아픈 엔터표시도 다림질 삭삭
        if (line[0] == '\0') { // 근데 빈 줄 빈 여백이라면?
            continue; // 에러치지 말고 아무 데이터도 없으니 쿨하게 패스, 다음 줄을 퍼담자!
        }

        // CSV 칼잡이 전담 마크 출동시켜 방금 다림질 완료된 문장을 콤마 기준으로 하나하나 쪼개버립니다!
        if (!split_csv_line(line, &row)) {
            fclose(file); // 여기서 실패했다면
            free(selected_indices); // 메모리 다 거둬가고
            free_string_list(&header); // 헤더 메모리도 폭파
            storage_free_table(out_table); // 여태 넣은 테이블 구조체도 폭파 파쇄
            snprintf(error_buf, error_buf_size, "데이터 행을 CSV 룰로 파싱해 분해하는 데 실패했습니다.");
            return 0; // 폭파후 도주
        }

        // 이 세상에 기둥보다 데이터가 긴 집은 없습니다. 데이터행 값이 파싱된 토막 개수가 스키마(헤더) 토막 개수와 다르면 구조적 결함 에러입니다!
        if (row.count != header.count) {
            fclose(file); 
            free(selected_indices); 
            free_string_list(&header);
            free_string_list(&row);
            storage_free_table(out_table); // 모든거 싹다 철수 파쇄
            snprintf(error_buf, error_buf_size, "특정 행의 콤마 구분 개수가 테이블 컬럼수 스키마와 다릅니다. (데이터 손상 의심)");
            return 0; // 테이블 폭파 후 도망
        }

        // 내가 이 줄에서 진짜 가져갈 알짜배기들만 간추려 담을 1차원 데이터 새 보관소 공간을 동적 할당 제작! 
        selected_row = (char **)calloc(selected_count, sizeof(char *));
        if (selected_row == NULL) {
            fclose(file);
            free(selected_indices);
            free_string_list(&header);
            free_string_list(&row);
            storage_free_table(out_table);
            snprintf(error_buf, error_buf_size, "알짜배기 데이터 보관용 메모리 할당(calloc)에 실패했습니다.");
            return 0;
        }

        // 자, 필터링용으로 허락받은 알짜 개수만큼 빙글빙글 루프를 돌면서 복제이식을 시작합니다.
        for (row_index = 0; row_index < selected_count; ++row_index) {
            // 원본 덩어리(row)에서, 우리가 아까 뽑아둔 "알짜 인덱스 색인 번호" 로 콕 집어서 접근해, 그 특정 칼럼의 데이터 값만 쪽 빨아먹고 신규 방(selected)으로 딥카피 복사!
            selected_row[row_index] = sql_strdup(row.items[selected_indices[row_index]]);
            if (selected_row[row_index] == NULL) { // 메모리 문제라면
                fclose(file); // 하던거 다 멈추고 파일 떼기
                free(selected_indices);
                free_string_list(&header);
                free_string_list(&row); // 껍데기들 분해 폐기
                sql_free_string_array(selected_row, row_index); // 만들던 것도 폐기
                storage_free_table(out_table);
                snprintf(error_buf, error_buf_size, "특정 셀 내부 값 할당 복사 중에 메모리 할당에 실패했습니다.");
                return 0; // 터짐
            }
        }

        // 특정 값만 이쁘게 쏙쏙 뽑아서 새롭게 정제한 이 빛나는 가로 한 줄 완성본(selected_row)을, 드디어 메인 표적지(out_table) 바닥 맨 끝단에 조인트(append) 탁 붙여버립니다!
        if (!append_row(out_table, selected_row)) {
            fclose(file);
            free(selected_indices);
            free_string_list(&header);
            free_string_list(&row);
            sql_free_string_array(selected_row, selected_count);
            storage_free_table(out_table); // 여기까지 와서 붙이기에 실패하다니. 싹 다 무너뜨리고 
            snprintf(error_buf, error_buf_size, "메인 공간에 행 데이터 세트를 붙여 저장하는 데 실패했습니다.");
            return 0; // 장렬히 실패 0반환.
        }

        selected_row = NULL; // 성공적으로 표적지에 줘버렸으니, 미련 없이 내 포인터 고리는 삭제
        free_string_list(&row); // 더미 원본 한 줄 값도 쓴물 단물 다 빨았으니 메모리 해제하여 다음 while 줄을 읽기 위한 구역으로 청소
    } // ~ 디스크의 줄이 끝날 때까지 위 맹렬한 퍼담기 과정 무한 반복

    // 퍼담기가 최종 끝났습니다! 파일이 다 털렸으니 운영체제에 얌전히 반납(close)합니다
    fclose(file); 
    free(selected_indices); // 할 일 끝난 인덱스 필터 메모리도 반환삭제
    free_string_list(&header); // 할 일 끝난 첫줄용 헤더 포켓 메모리도 반납삭제
    return 1; // 모든 게 순조롭게 메모리에 표착 성공(1 반환) 하여 당당히 본대로 복귀합니다!
}

// 메모리에 로드되지 않은 채 단순히 디스크에만 있는 .tbl 파일에서 헤더 이름들만 슬쩍 한 줄만 퍼와서 간보며 확인해 주는 탐정 같은 닌자 보조 함수
static int read_header(const char *path, StringList *header, char *error_buf, size_t error_buf_size) {
    FILE *file = fopen(path, "r"); // 읽기 전용으로 조용히 파일 객체를 가리켜 오픈합니다.
    char line[4096]; // 헤더의 정체를 파서 떠올릴 빈 거대 국자 버퍼 하나

    if (file == NULL) { // 이 경로에 그딴 파일은 애초에 없을때
        snprintf(error_buf, error_buf_size, "테이블 파일을 열 수 없습니다: %s", path); // 안타까움
        return 0; 
    }

    if (fgets(line, sizeof(line), file) == NULL) { // 맨 첫 한줄 국자로 떠봄, 근데 첫줄부터 값이 없었으면
        fclose(file); // 불량 쓰레기 파일이므로 던짐
        snprintf(error_buf, error_buf_size, "테이블 파일 안이 비어 있습니다: %s", path);
        return 0;
    }

    // 이미 목적 달성(첫 한줄 떴음) 했으니 미련 없이 즉각 폴더 파일 권한 닫아치워버리기(close)!
    fclose(file); 
    trim_newline(line); // 퍼온 글 줄 꼬리쪽 꼴보기 싫은 엔터표시 등 다림질
    if (!split_csv_line(line, header)) { // 콤마 썰기 칼잡이를 이용해 단어 분리!
        snprintf(error_buf, error_buf_size, "CSV 헤더 파싱에 실패했습니다."); // 파싱 터지면
        return 0; // 조용히 은둔실패
    }

    return 1; // 원본 구조 분석 확인 무사 성공 (1반환)
}

// 헤더나 컬럼 목록 같은 문자열 리스트 안에서 특정 이름이 몇 번째 칸에 있는지 찾아 인덱스로 반환하는 작은 탐색 도우미
static int find_string_list_index(const StringList *list, const char *value) {
    size_t index;

    for (index = 0; index < list->count; ++index) {
        if (sql_case_equal(list->items[index], value)) {
            return (int)index;
        }
    }

    return -1;
}

// 값을 저장하기 앞서 그 텍스트 내용물 자체가 콤마나 엔터를 포함하는 빌런이 아닌지, 저장 형식에 부합하는 클린 데이터인지 사전에 무단 검진해주고 쫓아내는 세관 검사 함수
static int validate_literal_for_storage(const Literal *literal, char *error_buf, size_t error_buf_size) {
    // 저장값 글자중에 csv를 붕괴시킬 콤마(,)가 있거나 개행(\n, \r)이 들어있으면 저장 규약에 치명적이므로
    if (strchr(literal->text, ',') != NULL || strchr(literal->text, '\n') != NULL || strchr(literal->text, '\r') != NULL) {
        snprintf(error_buf, // 에러 메시지를 강력 세팅
                 error_buf_size,
                 "현재 간단한 MVP 임시 저장 포맷에서는, 값 내부에 '콤마'나 '줄바꿈기호'가 포함된 악성 값은 시스템 마비를 초래하여 지원하지 않습니다: %s",
                 literal->text);
        return 0; // 입국 거부 (0 반환)
    }
    return 1; // 아무 문제 없는 클린 데이터임을 확인 (1 반환)
}

// 메모리에 준비된 데이터들을 디스크 하드의 진짜 .tbl 파일 맨 밑단에 '이어서 쓰는(Append)' 기능! INSERT 쿼리의 핵심 하드 저장 메커니즘을 총괄하는 함수! 
int storage_append_row(const char *data_dir,
                       const char *table_name,
                       char *const *input_columns,
                       size_t input_column_count,
                       const Literal *values,
                       size_t value_count,
                       char *error_buf,
                       size_t error_buf_size) {
    char path[1024]; // 경로 뚝딱
    FILE *file; // 파일 조절 막대
    StringList header = {0}; // 먼저 헤더 검사용
    const Literal **ordered_values = NULL; // 입력 컬럼 순서가 따로 들어온 경우, 스키마 순서로 다시 꽂아둘 포인터 배열
    size_t schema_column_count; // 헤더 해제 이후에도 에러 메시지에 안전하게 쓸 스키마 컬럼 수 백업본
    size_t index; // 루프 인원수

    if (!make_table_path(data_dir, table_name, path, sizeof(path))) { // 경로 무사 생성 확인부터
        snprintf(error_buf, error_buf_size, "테이블 물리경로 생성에 실패했습니다.");
        return 0;
    }

    // 자 대상 파일을 은밀히 스캔 떠서 헤더 구조가 어떤지 파악(read_header 보조함수 닌자 투입)!
    if (!read_header(path, &header, error_buf, error_buf_size)) {
        return 0; // 이 테이블이 비었거나 스캔에 실패하면 에러를 띄움
    }

    schema_column_count = header.count;

    // 입력 컬럼 목록이 없다면 기존처럼 값 순서가 곧 스키마 순서라고 가정합니다.
    if (input_columns == NULL || input_column_count == 0) {
        if (header.count != value_count) {
            free_string_list(&header); // 검사용 스캔목록 닫고
            snprintf(error_buf, // 개수부족 논리적 오류를 출력해 크게 꾸짖음
                     error_buf_size,
                     "현재 넣으려고 시도하는 INSERT 값 세트의 개수(%zu)와 오리지널 테이블 스키마의 컬럼 수(%zu)가 전혀 어울리지 않고 다릅니다.",
                     value_count,
                     schema_column_count);
            return 0; // 삽입 실패 기권 강제 반환!
        }
    }
    else {
        if (input_column_count != value_count) {
            free_string_list(&header);
            snprintf(error_buf,
                     error_buf_size,
                     "INSERT 컬럼 목록 개수(%zu)와 VALUES 안의 값 개수(%zu)가 서로 다릅니다.",
                     input_column_count,
                     value_count);
            return 0;
        }

        if (input_column_count != header.count) {
            free_string_list(&header);
            snprintf(error_buf,
                     error_buf_size,
                     "현재 단순 INSERT 컬럼 매핑 모드에서는 테이블의 모든 컬럼을 한 번씩 지정해야 합니다. 입력 컬럼 수=%zu, 스키마 컬럼 수=%zu",
                     input_column_count,
                     schema_column_count);
            return 0;
        }

        ordered_values = (const Literal **)calloc(header.count, sizeof(Literal *));
        if (ordered_values == NULL) {
            free_string_list(&header);
            snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
            return 0;
        }

        for (index = 0; index < input_column_count; ++index) {
            int header_index = find_string_list_index(&header, input_columns[index]);
            if (header_index < 0) {
                free(ordered_values);
                free_string_list(&header);
                snprintf(error_buf,
                         error_buf_size,
                         "INSERT 에서 지정한 컬럼이 실제 테이블 스키마에 없습니다: %s",
                         input_columns[index]);
                return 0;
            }

            if (ordered_values[header_index] != NULL) {
                free(ordered_values);
                free_string_list(&header);
                snprintf(error_buf,
                         error_buf_size,
                         "INSERT 컬럼 목록에 같은 컬럼이 중복되어 있습니다: %s",
                         input_columns[index]);
                return 0;
            }

            ordered_values[header_index] = &values[index];
        }
    }

    // 갯수도 딱 맞고 무사 통과됐다면, 이제 본격 삽입을 위해 대상 파일을 '추가작성 모드("a": append, 뒤에 이어 붙이기 모드)' 로 화끈하게 힘주어 엽니다!
    file = fopen(path, "a");
    if (file == NULL) { // 만일 수정 권한이 막혀있거나 파일이 잠겼다면
        free(ordered_values);
        free_string_list(&header); // 스캔목록 지우고
        snprintf(error_buf, error_buf_size, "테이블 파일을 병합수정 목적인 append('a') 모드로 열 수가 없습니다: %s", path);
        return 0; // 문 걸어 잠김 실패 반환!
    }

    // 드디어 파일 문이 열렸고. 내가 쥔 데이터 개수만큼 빙빙 돌며 루프 이식(Write)을 시작합니다.
    for (index = 0; index < header.count; ++index) {
        const Literal *literal = ordered_values == NULL ? &values[index] : ordered_values[index];

        if (literal == NULL) {
            fclose(file);
            free(ordered_values);
            free_string_list(&header);
            snprintf(error_buf,
                     error_buf_size,
                     "INSERT 컬럼 목록이 테이블 스키마 전체를 채우지 못했습니다. 누락된 컬럼이 있습니다.");
            return 0;
        }

        // 넣고자 하는 값이 혹시 시스템 버그를 낼 악성(콤마 등) 값은 아닌지 1회용 세관 검사 함수 돌림
        if (!validate_literal_for_storage(literal, error_buf, error_buf_size)) {
            fclose(file); // 빌런 발견 시 파일 즉시 강제 닫고 격리!
            free(ordered_values);
            free_string_list(&header); 
            return 0; // 쓰기 도중 폭파 실패 반환
        }

        // 빌런이 아니라면? 일단 첫번째 값은 그냥 넘어가되, 앞쪽에 들어간 게 있는 1,2번째 녀석이 넣기 시작할 때 그 사이사이에 칸박이용으로 콤마(,) 구조 기호를 하나 하드에 써버립니다!
        if (index > 0) {
            fputc(',', file); // 파일에 , 한 글자 프린팅
        }
        // 칸막이 쳤으니 이제 진짜 알맹이 글자들을 하드상 파일에 쭉 프린팅(출력) 해서 박아버립니다! 쾅쾅!
        fputs(literal->text, file); 
    }
    // 루프가 끝나 값들이 모두 써졌으면, 줄의 완성을 장식할 엔터(줄 넘김: '\n') 문자를 하나 마지막으로 맨 뒤에 찍고 장식 마무리를 날려버립니다!
    fputc('\n', file);

    // 하드디스크 변경사항 지시가 물리적으로 온전하게 다 끝났으므로 해당 권한 파일 객체를 폐쇄시킵니다(반납). 닫아야 진짜 저장됨!
    fclose(file);
    free(ordered_values);
    free_string_list(&header); // 보조 스캔목록 지우고
    return 1; // 아름답고 안전한 물리적 영구 저장 성공을 축하합니다!!! (1)
}

// 나중에 최적화기나 어디서 조회를 할 때, "야, 'id' 컬럼은 과연 배열 내 몇번 인덱스 방 위치에 있니?" 를 0,1,2 등 방번호 주소값 수치로 빠르게 찾아내 알려주는 검색기 함수
int storage_find_column_index(const TableData *table, const char *column_name) {
    size_t index; // 루프를 돌 변수

    // 대상 테이블의 가로방향 기둥(컬럼) 갯수만큼 옆으로 이동하며 순회 탐색전 가동
    for (index = 0; index < table->column_count; ++index) {
        // 앗 뭔가 이름이 비슷한걸 찾았다면, 대소문자 무시한채 둘의 철자가 진짜 똑같냐? 1:1 진실의 방 대면 비교 조사!
        if (sql_case_equal(table->columns[index], column_name)) {
            return (int)index; // 철자가 똑같다면 빙고! 바로 그놈이 있던 호수(인덱스 변환숫자)를 돌려주며 성공 귀환!
        }
    }
    return -1; // 배열 끝까지 다 털어 뒤져봐도 그런 이름은 우리동네에 없습니다! 존재 안한다는 뜻으로 음수 -1 마이너스 반환!
}

// 덩치가 존나게 큰 이 공룡같은 2차원 배열 구조체 (TableData) 메모리를 OS 시스템에 티끌없이 깔끔하게 분해 반환하는 청소 함수 원형
void storage_free_table(TableData *table) {
    size_t row_index; // 세로줄을 탐색할 변수
    size_t column_count; // 가로줄 기둥갯수 복사용

    if (table == NULL) { // 애초에 치울 테이블 자체가 허상이라면
        return;          // 암것도 맙시 바로 집에가셈
    }

    column_count = table->column_count; // 나중에 편하게 쓰기 위해 기둥(컬럼)개수 따로 변수에 스킵 복사 백업
    sql_free_string_array(table->columns, table->column_count); // 유틸 폭탄마를 동원해 일단 표 윗부분에 적힌 컬럼명 배열들부터 싸그리 소거해 날립니다!
    table->columns = NULL; // 가스라이팅 끊기 위해 포인터 날리고 
    table->column_count = 0; // 남은 개수도 0으로 뇌세탁

    // 컬럼명은 날아갔으니 이제 아래에 적힌 본진. 무수한 세로 알맹이 데이터 행들의 배열 집단들을 도륙낼 차례!
    for (row_index = 0; row_index < table->row_count; ++row_index) {
        // 유틸 폭탄마에게 가로줄 배열 하나에 기둥수 폭탄 장전해서 세로줄 한 줄씩 폭파하여 안쪽 데이터 메모리부터 날려버리기 무한 반복!!
        sql_free_string_array(table->rows[row_index], column_count); 
    }
    // 내부의 가로 자잘구리 데이터줄들이 다 날아가고 빈 허상만 남은 줄세우기 포인터 배열 그 자체 껍데기도 마침내 파괴자(free)가 처리 폭발!
    free(table->rows); 
    table->rows = NULL; // 꼬리 자르기
    table->row_count = 0; // 행의 숫자도 리셋 0
} // 무거운 테이블이 모두 공기중의 메모리로 사라졌습니다. Clear.
