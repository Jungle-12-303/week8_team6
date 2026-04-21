#ifndef TOKENIZER_H // 다중 포함(include)을 방지하기 위한 헤더 가드 시작
#define TOKENIZER_H // TOKENIZER_H 매크로 선언

#include <stddef.h> // size_t 등 표준 데이터 타입을 사용하기 위한 표준 라이브러리 포함

// SQL 텍스트를 파싱할 때 쪼개어지는 하나하나의 "단어(Token)"가 어떤 성질을 띠는지 정의한 열거형
typedef enum {
    TOKEN_EOF = 0,      // (End Of File) 파일의 완전한 끝을 의미하는 토큰
    TOKEN_IDENTIFIER,   // 예약어가 아닌 일반 단어들이나 사용자가 지은 이름(테이블명, 컬럼명 등)
    TOKEN_STRING,       // 'Alice' 등 따옴표로 감싸진 문자열 데이터 상수
    TOKEN_NUMBER,       // 123, -45 등 순수 숫자 형태의 데이터 상수
    TOKEN_INSERT,       // 명렁어 키워드: "INSERT"
    TOKEN_INTO,         // 명령어 키워드: "INTO"
    TOKEN_VALUES,       // 명령어 키워드: "VALUES"
    TOKEN_SELECT,       // 명령어 키워드: "SELECT"
    TOKEN_FROM,         // 명령어 키워드: "FROM"
    TOKEN_WHERE,        // 명령어 키워드: "WHERE"
    TOKEN_STAR,         // 기호: 별표(*)
    TOKEN_COMMA,        // 기호: 쉼표(,)
    TOKEN_LPAREN,       // 기호: 왼쪽 괄호 '('
    TOKEN_RPAREN,       // 기호: 오른쪽 괄호 ')'
    TOKEN_SEMICOLON,    // 기호: 세미콜론 ';' (명령 종료)
    TOKEN_EQUALS,       // 기호: 등호 '='
    TOKEN_NOT_EQUALS,   // 기호: 부정 비교 '!=' 또는 '<>'
    TOKEN_LESS_THAN,    // 기호: '<'
    TOKEN_LESS_EQUAL,   // 기호: '<='
    TOKEN_GREATER_THAN, // 기호: '>'
    TOKEN_GREATER_EQUAL // 기호: '>='
} TokenType;

// 잘게 쪼개진 단편적인 조각(토큰) 하나의 정보를 품고 있는 구조체
typedef struct {
    TokenType type;     // 이 토큰이 어떤 성질(예약어냐, 숫자냐, 괄호냐 등)을 가지는지 나타내는 열거형 변수
    char *lexeme;       // 텍스트상에서 실제로 어떻게 생겼는지(예: "SELECT", "123") 담고 있는 문자열 메모리 포인터
    size_t position;    // 나중에 에러 발생 시 위치를 추적하기 위해, 이 단어가 원본 글의 몇 번째 원본 인덱스에 있었는지 보관하는 위치값
} Token;

// 위에서 만든 토큰들이 순서대로 여러 개 모여서 문맥을 이루게 될 "토큰 배열(리스트)" 보관용 구조체
typedef struct {
    Token *items;       // 차곡차곡 쌓은 토큰들이 가지런히 나열되어 있는 동적 배열의 시작 지점(포인터)
    size_t count;       // 이 창고(배열) 안에 토큰이 현재 총 몇 개나 쌓였는지 카운트하는 변수
    size_t capacity;    // 이 창고(배열)가 꽉 차지 않고 최대로 수용 가능한 전체 한도치 용량
} TokenArray;

// 파싱의 1차 관문: 전체 SQL 원시 텍스트(문자형)를 위에서 만든 토큰 배열로 예쁘게 토막내주는 변환 함수의 원형
int tokenize_sql(const char *input, TokenArray *out_tokens, char *error_buf, size_t error_buf_size);
// 토큰들에 쓰였던 모든 메모리(내부 텍스트 문자열, 배열 구조 등)를 시스템에 안전하게 반환하는 청소 함수의 원형
void free_tokens(TokenArray *tokens);
// 토큰의 고유 숫자 타입(TokenType 형식)을 넣으면 컴퓨터 말고 사람이 읽기 좋은 일반 단어 문자열("SELECT" 등)로 변환해주는 함수의 원형
const char *token_type_name(TokenType type);

#endif // 헤더 가드 끝
