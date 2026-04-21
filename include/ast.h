#ifndef AST_H // AST(추상 구문 트리) 관련 헤더 중복 포함(include) 방지용 가드 시작
#define AST_H // AST_H 선언

#include <stddef.h> // size_t 등을 쓰기 위해 표준 데이터 정의 헤더를 포함합니다.

// SQL 안의 '값'이 문자인지 숫자인지 종류를 구분하기 위한 열거형(Enum)
typedef enum {
    LITERAL_STRING, // "Alice" 와 같은 따옴표 쳐진 문자열 타입
    LITERAL_NUMBER  // 123 과 같은 단순 정수 등의 숫자 타입
} LiteralType;

// SQL 내부의 구조적인 값이 아닌, 고정된 '상수값(Literal)' 1개를 표현하는 구조체
typedef struct {
    LiteralType type; // 지금 이 값이 문자인지 숫자인지를 나타내는 위에 정의된 종류 플래그
    char *text;       // 실제 해당 값을 문자 형태로 보관하는 가변 포인터 배열 (예: "123" 또는 "Alice")
} Literal;

// "SELECT id, name" 같은 명령에서 추출된 id, name 컬럼명 목록들을 담아놓는 구조체
typedef struct {
    char **items; // 컬럼명들을 나열해놓은 2차원 문자열 배열 포인터
    size_t count; // 방금 위 배열에 컬럼명이 몇 개나 쌓여서 들어있는지를 세는 카운터 변수
    int is_star;  // SELECT * 기능처럼 특별하게 "모든 항목"을 선택했는지(참이면 1)를 나타내는 플래그 변수
} ColumnList;

// WHERE 절에서 어떤 비교 연산을 수행할지 나타내는 열거형
typedef enum {
    COMPARE_EQUALS,
    COMPARE_NOT_EQUALS,
    COMPARE_LESS_THAN,
    COMPARE_LESS_THAN_OR_EQUAL,
    COMPARE_GREATER_THAN,
    COMPARE_GREATER_THAN_OR_EQUAL
} CompareOperator;

// "WHERE id = 1" 같은 조회 및 제약 조건의 역할을 담는 구조체
typedef struct {
    char *column_name; // 어떤 컬럼을 기준으로 비교 조건을 잡았는가? (예: "id")
    CompareOperator op; // 기준 컬럼과 리터럴을 어떤 방식으로 비교할지 나타내는 연산자
    Literal value;     // 그 특정 컬럼과 비교할 구체적 기준 값은 무엇인가? (예: Literal 형태로 1)
} WhereClause;

// 구문 분석이 끝난 최종 "SELECT" 명령어 구조체의 형태
typedef struct {
    char *table_name;   // 어느 테이블 파일에서 조회할지를 나타내는 테이블 이름 변수 (예: "users")
    ColumnList columns; // 조회 대상 파일에서 어떤 컬럼들만 쏙쏙 뽑아서 조회할지의 목록 리스트
    int has_where;      // 단순히 전체를 가져오지 않고 WHERE 조건절이 포함되어 있는가? (참이면 1, 거짓은 0)
    WhereClause where;  // 조건이 있다는 위 플래그가 1이라면, 이곳에 조건의 상세 내역과 비교값이 채워짐
} SelectStatement;

// 구문 분석이 끝난 최종 "INSERT" 명령어 구조체의 형태
typedef struct {
    char *table_name;    // 내가 가진 데이터를 어느 목적지 테이블에 집어넣을지를 나타내는 테이블 이름
    ColumnList columns;  // 사용자가 INSERT 시 직접 적어준 입력 컬럼 순서 목록. 비어 있으면 테이블 스키마 순서를 그대로 따른다고 봅니다.
    Literal *values;     // 테이블에 밀어 넣을 데이터(값) 여러 개를 담은 묶음(배열의 시작점 포인터)
    size_t value_count;  // 넣을 값이 총 몇 개나 되는지 개수를 파악하기 위한 카운터 
} InsertStatement;

// 컴퓨터가 파싱(해석)을 마친 최종 명령문이 도대체 SELECT 인지 INSERT 인지 가장 상위 종류를 구분짓는 열거형(Enum)
typedef enum {
    AST_SELECT_STATEMENT, // 이 명령은 데이터를 찾는(조회) 명령이다
    AST_INSERT_STATEMENT  // 이 명령은 데이터를 넣는(삽입) 명령이다
} StatementType;

// 최상위 단위인 "프로그램의 명령문(Statement)" 한 묶음 자체를 표현하는 래퍼(Wrapper) 구조체
typedef struct {
    StatementType type; // 내가 실행해야 할 이번 명령의 실제 종류를 나타냄 (SELECT인가? INSERT인가?)
    union { // 공용체: 메모리 낭비를 줄이기 위해 SELECT용 정보 묶음과 INSERT용 정보 묶음 중 한가지 목적만 덮어서 공간을 낭비없이 공유함
        SelectStatement select_stmt; // 이 명령이 만약 SELECT 명령이라면 활성화하여 사용할 공간
        InsertStatement insert_stmt; // 이 명령이 만약 INSERT 명령이라면 활성화하여 사용할 공간
    } as; // 'as' 라는 이름의 구조체 멤버에 접근하여, as.select_stmt 등의 형태로 위 공용체 내부 실 데이터에 접근하게 함
} Statement;

void free_literal(Literal *literal); // 사용이 다 끝난 뒤 Literal 구조체의 내부 메모리를 안전하게 비우고 돌려주는 함수 원형 선언
Literal clone_literal(const Literal *literal); // 기존 Literal 구조체와 완벽히 분리된 복제본을 똑같이 깊은 복사하여 찍어내는 함수 원형 선언
int clone_statement(const Statement *source, Statement *destination); // 디버그/트레이스 등에 쓰기 좋게 Statement 전체를 깊은 복사하는 함수 원형 선언
void free_statement(Statement *statement); // 명령 세트인 Statement 전체 구조의 메모리를, 하위 트리들을 재귀적으로 돌며 모두 깨끗하게 지워주는 함수 원형 선언

#endif // AST_H 가드를 닫음
