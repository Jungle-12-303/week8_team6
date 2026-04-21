#ifndef STORAGE_H // 구조체 중복 선언 등을 막기 위해 방어하는 헤더 가드의 시작점입니다.
#define STORAGE_H // STORAGE_H 매크로를 정의하여 헤더가 열렸음을 기록합니다.

#include "ast.h" // 디스크에 저장하거나 비교할 때 사용할 상수 데이터값(Literal 형식) 구조체를 쓰기 위해 AST 뼈대 헤더를 포함합니다.

#include <stddef.h> // size_t 와 같은 OS 독립적인 기본 크기 측정 자료형을 쓰기 위한 표준 헤더 포함입니다.

// 물리적 하드 디스크(.tbl 파일)에서 긁어온 실제 테이블 내용물을, 사용하기 편하게 메모리에 표(엑셀) 형식으로 임시 전개해놓은 거울 역할의 구조체
typedef struct {
    char **columns;      // 현재 이 테이블이 지니고 있는 속성 기둥(컬럼 이름)들을 차례대로 나열한 문자열들의 1차원 목록 배열입니다.
    size_t column_count; // 위의 컬럼 기둥이 총 가로로 몇 개나 펼쳐져 있는지를 나타내는 개수 카운터입니다.
    char ***rows;        // 실제 유저 데이터(행)들을 줄줄이 담고 있는 2인자원 배열 구조입니다. rows[행번호_인덱스][열번호_인덱스] 로 각각의 텍스트 셀(cell) 값에 접근할 수 있습니다.
    size_t row_count;    // 그렇게 켜켜이 쌓인 유저 데이터 행(row)이 세로로 총 몇 줄이나 들어 있는지를 나타내는 개수 카운터입니다.
} TableData;

// 디스크에 있는 .tbl 파일을 열어, 그 속성 내용들을 바탕으로 전부 혹은 일부를 메모리(TableData 구조체)상으로 복사해 올려붙이는 '불러오기(Load)' 함수의 원형 선언입니다.
int storage_load_table(const char *data_dir,
                       const char *table_name,
                       char *const *required_columns, // 특히 SELECT 에서 이 값으로 전체가 아닌 '일부 특정 컬럼'만 뽑아 가져오라고 제한할 수 있습니다.
                       size_t required_count,
                       TableData *out_table,
                       char *error_buf,
                       size_t error_buf_size);

// INSERT 명령 실행을 위해, 메모리에 준비해 둔 단일 데이터 줄(row) 값들을 디스크의 .tbl 파일 맨 밑바닥 끄트머리에 한줄 그대로 쏟아(Append) 부어 영구 저장 시키는 파일 출력 함수의 원형입니다.
int storage_append_row(const char *data_dir,
                       const char *table_name,
                       char *const *input_columns,
                       size_t input_column_count,
                       const Literal *values,
                       size_t value_count,
                       char *error_buf,
                       size_t error_buf_size);

// 만약 "name" 이라는 컬럼명이 테이블 데이터 배열 내에서 위치상 과연 "몇 번째 인덱스 숫자"인지 0,1,2 등 방번호로 찾아주는 내부 유틸리티 탐색기의 원형입니다.
int storage_find_column_index(const TableData *table, const char *column_name);

// 한 번 조회가 끝난, 용량 덩치가 크디큰 메모리상 2차원 배열 데이터(TableData)의 모든 셀을 재귀적으로 돌아가며 시스템에 깔끔하게 반환 해제하는 테이블 쓰레기통 함수의 원형입니다.
void storage_free_table(TableData *table);

#endif // 중복 방지 가드 코드를 끝맺습니다.
