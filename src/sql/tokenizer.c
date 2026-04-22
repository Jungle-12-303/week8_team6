#include "tokenizer.h" // 앞서 작성한 논리를 품은 토크나이저 헤더 파일을 가져옵니다.

#include "util.h" // 공통된 문자열 제어나 메모리 등을 돕는 유틸리티 헤더를 가져옵니다.

#include <ctype.h>  // 알파벳인지 문자인지 판단하는 isalpha, isdigit 등 내장 문자 분류 판별 함수들을 쓰기 위한 헤더
#include <stdio.h>  // snprintf 등 메모리에 글자를 쓰기 위해 형식을 다루는 표출 내장 함수용 헤더
#include <stdlib.h> // 변수 크기를 넘어선 동적 메모리를 다루기 위한 조작 함수 (malloc, realloc, free)
#include <string.h> // 문자열 메모리 덮어쓰기(memset) 등을 위한 문자열 제어 헤더

// 핵심 내부 보조 함수: 추출물을 보관하는 토큰 배열(tokens)에, 새로운 조각(Token) 하나를 맨 끝에 안전하게 이어 붙여줍니다.
static int append_token(TokenArray *tokens, TokenType type, char *lexeme, size_t position) {
    Token *new_items; // 배열이 꽉 찼을 경우 더 넓은 땅을 빌리기 위해 포인터 공간 준비

    if (tokens->count == tokens->capacity) { // 배열에 이미 기록된 토큰의 갯수(count)가 창고 한계 용량(capacity)에 꽉 도달했다면
        size_t new_capacity = tokens->capacity == 0 ? 16 : tokens->capacity * 2; // 용량이 아예 처음부터 없었으면 기초값 최소 16칸을, 아니라면 현재 용량의 딱 2배를 새 한도액으로 설정합니다.
        new_items = (Token *)realloc(tokens->items, new_capacity * sizeof(Token)); // 기존의 아이템 배열을 새 한도 사이즈만큼 메모리를 강제 재할당(realloc)하여 늘려버립니다.
        if (new_items == NULL) { // 메모리 확장 재할당에 실패했다면 (예: 시스템 메모리 초과)
            free(lexeme);        // 토큰으로 들어오려다 공중에 떠버린 버려진 텍스트(lexeme)를 먼저 회수하여 지워주고
            return 0;            // 추가를 실패했음을 의미하는 오류표지 역할의 0을 반환합니다.
        }
        tokens->items = new_items;       // 배열 확장이 성공 시 확보된 넓어진 새 주소로 배열의 머리를 갈지시켜 기억시킵니다.
        tokens->capacity = new_capacity; // 두배로 늘어난 이 최대 용량값도 한계치로 재기억시킵니다.
    }

    // 배열 내의 "현재 갯수(count)" 번째 위치라는 것은 곧 맨 끝 빈 공간의 위치를 의미하므로, 이 빈 공간에 전달받은 값들을 착착 채워 넣습니다.
    tokens->items[tokens->count].type = type;         // 어떤 부류의 단어 명찰인가? (키워드인가 문자열인가 등) 저장
    tokens->items[tokens->count].lexeme = lexeme;     // 실제 "글자" 그 자체의 형태가 들어있는 메모리 주소 저장
    tokens->items[tokens->count].position = position; // 나중에 에러 디버깅을 위해 이 단어가 원본 글의 전체 길이 중 몇 번째 위치에 있었는가 정보 저장
    tokens->count++; // 방금 한 개를 맨 귀퉁이 끝에 찔러 넣었으므로 전체 적재량 개수를 1개 증가시킵니다.
    return 1; // 성공적으로 덧붙였음을 보고(1 반환) 합니다.
}

// 추출해낸 단어(텍스트 블록)를 이리저리 비교해 보고 어떠한 타입(종류 키워드)인지 명찰을 확정해 주는 판독 함수
static TokenType identifier_type(const char *text) {
    if (sql_case_equal(text, "INSERT")) { // 만일 단어가 "INSERT" (대소문자 무시) 모양이라면
        return TOKEN_INSERT;              // 일반 명사가 아니라 특별히 삽입(INSERT) 예약 기능을 가진 특이 토큰 명찰을 반환합니다.
    }
    if (sql_case_equal(text, "INTO")) {   // 단어가 "INTO" 라면
        return TOKEN_INTO;                // INTO 예약어 명찰을 반환
    }
    if (sql_case_equal(text, "VALUES")) { // 단어가 "VALUES" 라면
        return TOKEN_VALUES;              // VALUES 예약어 명찰을 반환
    }
    if (sql_case_equal(text, "SELECT")) { // 단어가 "SELECT" 라면
        return TOKEN_SELECT;              // SELECT 예약어 명찰을 반환
    }
    if (sql_case_equal(text, "FROM")) {   // 단어가 "FROM" 이라면
        return TOKEN_FROM;                // FROM 예약어 명찰을 반환
    }
    if (sql_case_equal(text, "WHERE")) {  // 단어가 "WHERE" 라면
        return TOKEN_WHERE;               // WHERE 예약어 명찰을 반환
    }
    return TOKEN_IDENTIFIER;              // 만약 조각이 위 특별한 SQL 문법 속 키워드 중 어디에도 속하지 않는다면, 단순하고 일반적인 사용자의 이름 명명(테이블명 등)으로 규정합니다.
}

// 영어 알파벳이나 밑줄(_)로 시작되는 글자 뭉치를 쭉 밀며 연속해서 하나로 묶어 '단어(Identifier) 조각'으로 치부하고 추출해 내는 파쇄 함수
static int tokenize_identifier(const char *input, size_t *index, TokenArray *tokens, char *error_buf, size_t error_buf_size) {
    size_t start = *index; // 이 단어가 원본 긴 글의 몇 번째 인덱스부터 시작했는지 파편의 시작 위치를 기록해 둡니다.
    char *lexeme;          // 잘라낸 문자열을 담을 포인터 장소
    TokenType type;        // 완성된 단어의 판독 결과값 종류

    // 현재 읽은 글자가 알파벳 혹인 숫자(isalnum), 혹은 프로그래밍에서 허용되는 밑줄('_') 기호인 동안은 한 덩어리 유효 문자이므로 계속 반복하여 커서를 옆으로 이동합니다.
    while (isalnum((unsigned char)input[*index]) || input[*index] == '_') {
        (*index)++; // 옆으로 한 글자 탐색 이동
    }

    // 단어라고 생각되는 범위가 잘려 끝났으므로, 시작점(start)부터 이동한 현재 끝점위치(*index)까지의 길이만큼 입력 문장에서 글자를 떠서 똑 잘라(복사)옵니다.
    lexeme = sql_strndup(input + start, *index - start);
    if (lexeme == NULL) { // 메모리 문제로 글자를 복사해 퍼오는 데 실패했다면
        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다."); // 에러 버퍼에 이유 적어주고
        return 0; // 실패(0)를 즉각 반환하며 종료합니다.
    }

    // 잘라온 단어가 특별한 예약 키워드인지 단순 식별자 이름인지 검사하여 타입을 위에서 만든 함수로 분류합니다.
    type = identifier_type(lexeme);
    // 분류도 다 끝났으니 완성된 조각(토큰)과 타입들을 본 배열 리스트에 꼬리 순차적으로 탑승 합류시킵니다!
    return append_token(tokens, type, lexeme, start);
}

// 숫자(0~9)로 인식된 부분을 쭉 밀며 읽어서 하나의 이어지는 숫자 덩어리형 토큰으로 추출해 내는 파쇄 함수
static int tokenize_number(const char *input, size_t *index, TokenArray *tokens, char *error_buf, size_t error_buf_size) {
    size_t start = *index; // 숫자의 맨 앞 시작 위치를 똑같이 기록해 둡니다.
    char *lexeme;          // 뽑아낼 숫자 글자 덩어리 보관용 포인터

    if (input[*index] == '-') { // 만약 처음 나타난 글자가 빼기('-') 기호 즉 마이너스 수치 단위를 뜻한다면
        (*index)++;             // 이 역시 숫자의 유효한 일부 기호로 인정하고 옆칸으로 넘깁니다.
    }
    while (isdigit((unsigned char)input[*index])) { // 그 후 만나는 다음 글자들이 오직 연달은 아라비아 숫자(0~9)인 경우라면 덩이의 끝을 찾을 때까지 계속
        (*index)++; // 옆으로 한 칸이동
    }

    // 마이너스 부호 및 숫자들을 모두 밀어서 찾아 확인했으니 그 길이만큼 글자를 똑 잘라서 메모리로 복사해옵니다. (단, 아무리 숫자라도 컴파일러 상에선 일단 문자열 메모리 덩어리 상태로 뜹니다)
    lexeme = sql_strndup(input + start, *index - start);
    if (lexeme == NULL) { // 복사 중 메모리 부족 등으로 실패 시
        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다."); // 안타까움을 에러 버퍼 텍스트에 남기고
        return 0; // 실패(0)를 반환
    }

    // 숫자 뭉치를 '순수 숫자 타입(TOKEN_NUMBER)' 전용 꼬리표를 달아 배열 리스트 끝에 이어 붙입니다.
    return append_token(tokens, TOKEN_NUMBER, lexeme, start);
}

// 따옴표(')로 묶인 하나의 문장부호 덩어리를 "문자열 텍스트" 값으로 인식하고 토큰으로 정제 추출해 내는 파쇄 함수
static int tokenize_string(const char *input, size_t *index, TokenArray *tokens, char *error_buf, size_t error_buf_size) {
    size_t start = *index; // 오리지널 시작 따옴표의 위치를 저장해둡니다. (나중에 에러 표기 시 정확한 위치가 필요함)
    size_t content_start;  // 진짜 따옴표를 제외한 알맹이(내용물 안쪽) 글자의 시작 위치
    char *lexeme;          // 단어 내용물 덩어리 복사용 메모리 포인터

    (*index)++; // 시작을 여는 따옴표 부호 그 자체 문법 기호이므로 값에 포함시키지 않을 것이기 때문에 칸 이동하여 버립니다.
    content_start = *index; // 이동된 요 안쪽 지점부터가 진또배기 글자의 시작 지점입니다.

    // 긴 글이 갑자기 툭 잘려 끝나지 않으면서('\0'), 동시에 문자를 다시 닫아주는 부호(닫는 홑따옴표 '\'') 가 나올 때까지 전부 사용자 데이터 본문 값으로 취급하며 옆으로 계속 밉니다.
    while (input[*index] != '\0' && input[*index] != '\'') {
        (*index)++; // 옆으로 한 칸이동하며 탐색
    }

    if (input[*index] != '\'') { // 글이 툭 잘려 끝났는데, 닫을 때 필수인 닫는 따옴표를 못 찾았다면 (문법 에러 상태)
        snprintf(error_buf, error_buf_size, "문자열 리터럴이 닫히지 않았습니다. 위치=%zu", start); // 시작 위치를 엮어 사용자용 에러 표출!
        return 0; // 실패(0) 강제 반환
    }

    // 시작점부터 끝 따옴표 앞까지 포함된 길이의, 겉포장을 제외한 순수 "알맹이 내용물 텍스트"만 문자열로 쏙 빼와 복사합니다.
    lexeme = sql_strndup(input + content_start, *index - content_start);
    if (lexeme == NULL) { // 메모리 문제라면 당연히 복사가 안 먹힐테니
        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다."); // 에러 기록 및
        return 0; // 실패(0) 반환
    }

    (*index)++; // 이제 닫는 따옴표 부호까지 확인 완료했으니 포인터 커서를 한 칸 더 옮겨 드디어 지긋지긋한 따옴표 바깥세상으로 탈출시킵니다.
    // 뽑힌 진짜 문자 값들은 TOKEN_STRING (따옴표에 싸인 스트링 구문) 이라는 가장 높은 등급의 명찰을 달아주어 배열에 붙입니다.
    return append_token(tokens, TOKEN_STRING, lexeme, start);
}

// **토크나이저 본체**: 하나의 1줄짜리 긴 SQL 문장 전체를 입력받아서 잘게 쪼갠 조각(Token) 배열들로 변환 후 반환하는 메인 심장 함수
int tokenize_sql(const char *input, TokenArray *out_tokens, char *error_buf, size_t error_buf_size) {
    size_t index = 0; // 거대한 전체 텍스트 입력 문자열을 맨 첫 글자(인덱스 0)부터 순회할 커서(탐색기) 생성

    memset(out_tokens, 0, sizeof(*out_tokens)); // 토큰 파편 결과들을 산처럼 담을 반환 출구 배열(out_tokens) 전체 구조체를 깨끗하게 세탁 초기화시킵니다.

    while (input[index] != '\0') { // 꼬리를 물고 전체 입력 문자열 포인터가 글의 맨 끝부분(널 문자, EOF)을 만날 때까지 반복하며 한 글자씩 파헤칩니다!
        char ch = input[index]; // 지금 가리키는 커서의 한 글자(단 1바이트 크기)문자를 변수 ch 에 꺼내봅니다.

        // 1번 검사루프: 이 글자가 스페이스, 탭구분, 줄글 넘김 등 아무 의미없는 공백 문자(empty space)인가?
        if (isspace((unsigned char)ch)) {
            index++; // 아무 의미 없는 허공이므로 무시하고 옆으로 한 칸 그냥 뛰어 넘깁니다.
            continue; // 위 탐색구간으로 바로 다시 돌아가 다음 글자를 검토해 봅니다.
        }

        // 2번 검사루프: 이 글자가 로마 알파벳 글자이거나 밑줄(_)로 시작하는 구문인가? -> 그렇다면 이 덩어리는 변수 혹은 명령형의 단어 조각류일 것이다!
        if (isalpha((unsigned char)ch) || ch == '_') {
            if (!tokenize_identifier(input, &index, out_tokens, error_buf, error_buf_size)) { // 단어 한 무더기 조각을 잘라 추출을 시도했는데 내부에서 실패(0값)가 발생하면서 튕겼다면
                free_tokens(out_tokens); // 지금까지 엉성하게 붙여 담았던 이전의 모든 조각(결과물)들을 통째 폐기시킨 뒤 펑! 터뜨립니다.
                return 0; // 파싱 실패 및 종료 알림!
            }
            continue; // 이 글자들 무더기를 무사히 단어로 자르고 왔다면 해당 커서가 많이 이동되었을테니 위로 다시 돌립니다.
        }

        // 3번 검사루프: 글자가 순수 숫자(0~9)이거나 빼기 기호('-') 바로 뒤에 숫자가 붙어 나오는 음수 숫자 패턴구문인가? -> 이것은 분명 숫자 데이터 덩어리다!
        if (isdigit((unsigned char)ch) || (ch == '-' && isdigit((unsigned char)input[index + 1]))) {
            if (!tokenize_number(input, &index, out_tokens, error_buf, error_buf_size)) { // 한 무더기 숫자를 쭉 밀어서 추출하는 행동에 실패했다면
                free_tokens(out_tokens); // 지금까지 모은 배열 다 터뜨림
                return 0; // 실패 종료 보고.
            }
            continue; // 잘 자르고 왔으면 다시 다음 남은 글자로 이동.
        }

        // 4번 검사루프: 여는 홑따옴표가 시작되었다면? -> 따옴표(')로 감싸여져 있는 그 자체 "텍스트 문자열 데이터 덩어리" 형태일 것이다!
        if (ch == '\'') {
            if (!tokenize_string(input, &index, out_tokens, error_buf, error_buf_size)) { // 닫는 따옴표까지 쭈욱 읽어가며 완전체 덩어리 추출 기능 사용을 시도했는데 터졌다면
                free_tokens(out_tokens); // 전부 폐기 시키고
                return 0; // 실패.
            }
            continue; // 무사히 추출완료 시 위로 계속.
        }

        // 5번 마지막 검사루프: 위 복잡한 단어, 텍스트, 숫자 덩어리들이 하나도 아니고 아주 짧은 단순 특수 기호들(1개씩) 에 해당하는지 개별 검사합시다.
        switch (ch) { // 현재 글자 기호 1개를 두고 스위치 분기 처리
            case '*': // 모든 항목 뜻하는 (별표)
                if (!append_token(out_tokens, TOKEN_STAR, sql_strdup("*"), index)) { // 별점(STAR) 토큰을 생성하여 리스트에 등록을 시도!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다."); // 붙이기 실패시 에러 뱉기
                    free_tokens(out_tokens); // 모은 거 날림
                    return 0; // 실패
                }
                index++; // 성공 시 다음 글자로 전진
                break; // 스위치 구문 탈출
            case ',': // 항목들의 열을 바늘처럼 나눌 때 쓰는 기호 쉼표(콤마)
                if (!append_token(out_tokens, TOKEN_COMMA, sql_strdup(","), index)) { // 물리적인 쉼표(,) 토큰 생성 추가!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case '(': // 삽입문 등에서 묶음 범위를 한단계 그루핑하는 왼쪽 소괄호
                if (!append_token(out_tokens, TOKEN_LPAREN, sql_strdup("("), index)) { // 시작 소괄호( 토큰 생성!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case ')': // 그루핑이 문법적으로 끝났음을 알리는 닫는 형태의 오른쪽 소괄호
                if (!append_token(out_tokens, TOKEN_RPAREN, sql_strdup(")"), index)) { // 우측 소괄호) 막음 토큰 생성!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case ';': // 한 가지 SQL 문장이나 명령이 완전한 종착을 맞이했음을 뜻하는 강력한 세미콜론 종결자
                if (!append_token(out_tokens, TOKEN_SEMICOLON, sql_strdup(";"), index)) { // 세미콜론(;) 닫기 토큰 생성!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case '=': // 두 문자가 일치하는지 논리적인 조건 값 비교 등에 쓰이는 부호 등호(=)
                if (!append_token(out_tokens, TOKEN_EQUALS, sql_strdup("="), index)) { // 등호(=) 전용 토큰 생성!
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case '!':
                if (input[index + 1] != '=') {
                    snprintf(error_buf, error_buf_size, "지원하지 않는 문자입니다: '%c' (위치=%zu)", ch, index);
                    free_tokens(out_tokens);
                    return 0;
                }
                if (!append_token(out_tokens, TOKEN_NOT_EQUALS, sql_strdup("!="), index)) {
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index += 2;
                break;
            case '<':
                if (input[index + 1] == '=') {
                    if (!append_token(out_tokens, TOKEN_LESS_EQUAL, sql_strdup("<="), index)) {
                        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                        free_tokens(out_tokens);
                        return 0;
                    }
                    index += 2;
                    break;
                }
                if (input[index + 1] == '>') {
                    if (!append_token(out_tokens, TOKEN_NOT_EQUALS, sql_strdup("<>"), index)) {
                        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                        free_tokens(out_tokens);
                        return 0;
                    }
                    index += 2;
                    break;
                }
                if (!append_token(out_tokens, TOKEN_LESS_THAN, sql_strdup("<"), index)) {
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            case '>':
                if (input[index + 1] == '=') {
                    if (!append_token(out_tokens, TOKEN_GREATER_EQUAL, sql_strdup(">="), index)) {
                        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                        free_tokens(out_tokens);
                        return 0;
                    }
                    index += 2;
                    break;
                }
                if (!append_token(out_tokens, TOKEN_GREATER_THAN, sql_strdup(">"), index)) {
                    snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다.");
                    free_tokens(out_tokens);
                    return 0;
                }
                index++;
                break;
            default: // 그 외에 우리가 전혀 처리해주지 못하는 이상한 외계어, 백틱등의 특수문자, 한글 낱자 따위가 섞여 들어왔을 치명적 경우!
                snprintf(error_buf, error_buf_size, "지원하지 않는 문자입니다: '%c' (위치=%zu)", ch, index); // 즉각적인 거부 에러 메시지
                free_tokens(out_tokens); // 지금까지 구슬땀흘리며 차곡차곡 쌓은 토큰들 리스트 판 엎기 삭제!
                return 0; // 토큰 파편 제작을 중단, 완전 실패 선언!
        }
    }

    // 통과 및 끝판왕 처리: 무사히 커서를 잘 굴려 문자열 가장 끝에 도착해서 에러 없이 while 루프를 무사통과 빠져나왔다면,
    // 다 끝났다는 뜻으로 맨 마지막 꼬리에 <eof>(완전한 파일데이터 끝지점) 이라는 종료 상징 특수 토큰을 하나 더 추가적으로 도장 찍어 배열을 마감해 줍니다.
    if (!append_token(out_tokens, TOKEN_EOF, sql_strdup("<eof>"), index)) {
        snprintf(error_buf, error_buf_size, "메모리 할당에 실패했습니다."); // 어이쿠, 맨오른쪽 끝 도장 메모리 하나 박는거 실패해서
        free_tokens(out_tokens); // 그동안 고생해서 모은게 다 날아갑니다..
        return 0;
    }

    return 1; // 단 하나도 모난 곳이나 에러가 없이 정상적으로 1개의 온전한 배열 무더기로 완료되어 반환 성공! (1반환)
}

// 생성해서 사용한 1차 분쇄 토큰 배열을 쓰임이 완료된 후 메모리에서 모두 깨끗하게 비워 해제하고 반환하는 가비지 함수 구현부
void free_tokens(TokenArray *tokens) {
    size_t index; // 배열을 하나씩 살펴보고 순회용으로 이동할 숫자 인덱스 지정

    if (tokens == NULL) { // 띠용, 배결 포인터가 처음부터 아예 비어(NULL)있어 해제할 게 없었다면
        return;           // 무시하고 시간낭비 없이 바로 함수 탈출하여 반환!
    }

    for (index = 0; index < tokens->count; ++index) { // 토큰이 가진 총 전체 갯수(요소)만큼 순서대로 빙글빙글 돌면서 탐색하여
        free(tokens->items[index].lexeme); // từng từng 박혀있던 글자(text, lexeme) 덩어리들의 메모리면적들을 개별적으로 다 파내어 해제합니다.
    }
    free(tokens->items);  // 내부 알맹이들을 다 지웠으면 이제 이를 감싸고 있던 껍데기 박스인 items 통짜 배열 구조의 메모리 자체마저 바사삭 해제시킵니다.
    tokens->items = NULL; // 쓰레기 참조나 버그가 되지 않도록 포인터 고리를 단단히 끊어버리고 명시적인 NULL 값 기록
    tokens->count = 0;    // 들어있는 정보 개수도 완전히 제로(0)로 초기화 세탁
    tokens->capacity = 0; // 이전에 가지고 있던 확장 최대 수용량(capacity) 기록 정보 마저 0으로 세탁합니다.
}

// 에러가 났거나 디버깅해야 할 때, TokenType 이라는 컴퓨터용 고유숫자를 넣으면 컴퓨터 말고 사람(사용자)이 읽기 쉬운 문자열 단어와 영어로 예쁘고 멋지게 번역해주는 가독용 이름 반환기
const char *token_type_name(TokenType type) {
    switch (type) {
        case TOKEN_EOF:        return "EOF";        // 파일의 끝을 나타내는 문자열 표기 기호 번역 반환
        case TOKEN_IDENTIFIER: return "IDENTIFIER"; // 일반 명칭, 식별자 문자열 표기 번역 반환
        case TOKEN_STRING:     return "STRING";     // 텍스트상수 문자열 부류 번역 반환
        case TOKEN_NUMBER:     return "NUMBER";     // 숫자상수 문자열 부류 번역 반환
        case TOKEN_INSERT:     return "INSERT";     // 예약어 명령 반환
        case TOKEN_INTO:       return "INTO";       // 예약어 명령 반환
        case TOKEN_VALUES:     return "VALUES";     // 예약어 명령 반환
        case TOKEN_SELECT:     return "SELECT";     // 예약어 명령 반환
        case TOKEN_FROM:       return "FROM";       // 예약어 명령 반환
        case TOKEN_WHERE:      return "WHERE";      // 예약어 명령 반환
        case TOKEN_STAR:       return "STAR";       // 모든 컬럼을 표시하는 별표 기호 번역 반환
        case TOKEN_COMMA:      return "COMMA";      // 쉼표 기호 번역 반환
        case TOKEN_LPAREN:     return "LPAREN";     // 좌괄호 여는 기호 번역 반환
        case TOKEN_RPAREN:     return "RPAREN";     // 우괄호 닫는 기호 번역 반환
        case TOKEN_SEMICOLON:  return "SEMICOLON";  // 문장의 마침표 종결자 기호 반환
        case TOKEN_EQUALS:     return "EQUALS";     // 비교 논리 등호 번역 반환
        case TOKEN_NOT_EQUALS: return "NOT_EQUALS";
        case TOKEN_LESS_THAN:  return "LESS_THAN";
        case TOKEN_LESS_EQUAL: return "LESS_EQUAL";
        case TOKEN_GREATER_THAN: return "GREATER_THAN";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
    }
    return "UNKNOWN"; // 만일 우리 리스트에 없는 듣도보도못한 타입 코드가 날아왔다면 "나도 그게 뭔지 모름(UNKNOWN) 에러" 이라는 텍스트 문자열 반환
}
