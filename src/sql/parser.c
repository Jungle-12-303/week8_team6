#include "parser.h" // 파서 구조가 정의되어 있는 자기 자신의 뼈대 헤더 파일을 가져옵니다.

#include "util.h" // 문자열 제어와 메모리 재할당 등을 손쉽게 돕는 공용 도구함(유틸리티) 헤더를 포함합니다.

#include <stdio.h> // 에러 기록과 형식 맞춰 문자열 포매팅(snprintf) 작성을 돕기 위한 표준 입출력 헤더
#include <stdlib.h> // 부족한 동적 메모리 할당 용량을 재조절 및 관리(realloc, free) 해주기 위한 표준 라이브러리
#include <string.h> // 사용 전 구조체 완전 초기화(memset) 등 메모리 덮기 제어를 위한 스트링 헤더

// 중요한 관리책: 구문 분석(Parsing)이 주르륵 진행되는 동안, 현재 우리가 "순서상 몇 번째 토큰"을 읽고 있는지 위치나 상태를 쉽게 추적하고 보관해 두는 로컬 추적용 구조체
typedef struct {
    const TokenArray *tokens; // 이전 공장인 토크나이저가 애써 이쁘게 잘라 넘겨준 입력 배열 뭉치원본(토큰들)이 있는 곳의 주소
    size_t current;           // 현재 내가 순찰하며 순회하고 있는 토큰의 위치 커서(인덱스 번호 0번부터 시작)
    char *error_buf;          // 구문 에러가 터졌을 때 사용자를 탓하며 친절한 안내 메시지를 남길 빈 종이(버퍼)의 주소
    size_t error_buf_size;    // 그 에러 버퍼가 넘치지 않도록 제한하는 최대 크기 용도값
} Parser;

// 지금 현재 커서(current)가 위치한, 즉 아직 소비되지 않고 가장 앞단에 대기 중인 첫 번째 토큰을 몰래 살짝 먼저 들여다보고(peek) 확인만 해보는 눈팅용 보조 함수
static const Token *peek(Parser *parser) {
    return &parser->tokens->items[parser->current]; // 배열에서 현재 순회 커서가 위치하고 있는 해당 아이템의 주소번지를 그대로 쏙 꺼내서 결과로 돌려줍니다.
}

// 방금 직전에 꿀꺽 꺼내서 읽고 지나와서 이제는(과거형이 된) 바로 이전 칸의 토큰을 다시 꺼내주는 백미러 보조 함수
static const Token *previous(Parser *parser) {
    return &parser->tokens->items[parser->current - 1]; // 현재 이동한 커서의 딱 한 칸 뒤(-1) 백미러 상에 있던 이전 아이템 주소를 반환합니다.
}

// 우리가 배열 속의 모든 토큰 기호들을 다 냠냠 읽어치워서 더 이상 남은 문장이 없는지(파일 배열의 진짜 끝인지) 확인하는 레이더망 함수
static int is_at_end(Parser *parser) {
    return peek(parser)->type == TOKEN_EOF; // 현재 눈 앞에 대기중인 토큰의 명찰이 아예 글의 끝을 알리는 EOF 형이라면 참(1)을, 글이 남아있다면 거짓(0) 반환
}

// 현재 바로 대기 중인 토큰을 입으로 집어삼켜(소비하며 처리 처리했다고 치고), 포인터 커서를 한 칸 앞으로 한걸음 전진시키는 함수
static const Token *advance_token(Parser *parser) {
    if (!is_at_end(parser)) { // 아직 파일이나 토큰 배열이 다 끝나지 않고 글이 쌩쌩하게 남아있다면
        parser->current++;    // 커서를 기분 좋게 다음 칸으로 1칸 증가시킵니다.
    }
    return previous(parser); // 방금 당장 집어먹었던 그 토큰(현재 증가된 커서 기준으론 한 칸 도망가버린 과거 위치)을 꺼내어 반환해 줍니다.
}

// 현재 눈 앞 대기 중인 토큰이 우리가 문법상 "꼭 와줘야만 한다고 기대하는" 단어 타입(type)과 일치하는지 맞춰보는 검문소 함수 
static int match_token(Parser *parser, TokenType type) {
    if (peek(parser)->type != type) { // 눈팅으로 살펴봤는데, 대기 중인 토큰이 내가 원하는 종류의 명찰(타입)이 절대 아니라면
        return 0; // 틀렸고 맘에 안드니 전진(소비)하지 않고 그대로 실패(0)를 반환. 내버려둡니다.
    }
    advance_token(parser); // 원하던 기호가 딱 맞다면 기분 좋게 꿀꺽 삼키며 커서를 미래로 한칸 앞 전진시킵니다!
    return 1; // 성공적으로 매칭하여 집어 삼켰음(1) 을 외부로 알림
}

// 특정 타입(type)의 기호토큰이 이 위치에 "반드시 필수적으로" 나와야만 하는 강제 상황에서 쓰이는 독재자 함수. 안 나오면 몹시 화를 내며(에러 문구 세팅) 종료됨.
static int consume_token(Parser *parser, TokenType type, const char *message) {
    if (match_token(parser, type)) { // 먼저 매치 토큰 함수를 살짝 시켜서, 이게 원하는 타입이라면 조용히 먹고 치우라고 시도합니다.
        return 1; // 무사히 매칭되어 성공적으로 먹었다면 아무 에러 없이 기분 좋게 끝(1 반환).
    }

    // 만약 먹지 못하고 매치 실패했다면(이 위치에 전혀 얼토당토않은 문법 기호가 들어와 있었다면), 사용자가 입력한 구문이 어법에 심각하게 어긋났다는 뜻이므로
    snprintf(parser->error_buf,             // 에러 버퍼에 빨간 글씨를 남길 준비
             parser->error_buf_size,        // 버퍼 사이즈 넘치지 않게 조절
             "%s 현재 토큰=%s('%s')",             // 출력될 최종 문장 골격 틀 지정
             message,                               // 원래 이 자리에 기입을 요구했던 친절한 안내 메시지 (예: FROM이 와야 합니다!)
             token_type_name(peek(parser)->type),   // 지금 실제로 잘못 들어와서 자리차지한 엉뚱한 타입 명칭 문자열 (예: IDENTIFIER 따위)
             peek(parser)->lexeme);                 // 실제 잘못 쳤던 유저의 텍스트가 뭔지 오리지널 원문 기입 
    return 0; // 이 구문은 틀려먹었음, 파싱 불가능 심각한 에러 발생으로 취급 후 0 반환
}

// 순수 문자열('Alice')이나 순수 아라비아 숫자(123) 형태의 찐 "실제 값 데이터" 뭉치가 눈앞에 오면, 그걸 스윽 뽑아내서 Literal 이란 상자 구조체로 포장 가공하는 함수
static Literal parse_literal(Parser *parser, int *ok) {
    Literal literal; // 뽑아서 이쁘게 포장할 반환용 패키지 상자 구조체 껍데기 선언 준비
    const Token *token; // 꺼내올 토큰 보관용

    literal.type = LITERAL_STRING; // 일단 별 의미 없지만 기본 디폴트값으로 이 값은 문자열타입이다 라고 가짜로 적어둠
    literal.text = NULL;           // 내부 진짜 알맹이 문자 텍스트는 아직 안채웠으니 텅 빔(NULL)

    if (match_token(parser, TOKEN_STRING)) { // 만약 우리 눈앞에서 기다리던 첫값이 무사히 내가 아는 올바른 '문자열 타입' 으로 들어왔다면 냅다 삼킴!
        token = previous(parser);            // 입안으로 꿀떡 삼켰던 그 맛있고 달콤한 문자열 토큰을 배에서 다시 꺼내서
        literal.type = LITERAL_STRING;       // 이 포장 패키지의 실제 내용물 종류를 문자열(STRING) 타입 명찰로 명확히 확정!
        literal.text = sql_strdup(token->lexeme); // 원본에 들어있던 알맹이 글자를 우리가 가진 새로운 메모리방(깊은 복사본)으로 쏙 독립해 빼옵니다.
        *ok = literal.text != NULL;          // 혹시 메모리 부족으로 빼오는 데 실패하진 않았는지 성패 여부를 바깥 매개 포인터(*ok)로 세팅해 수신 보고합니다.
        return literal; // 깔끔하고 안전하게 잘 포장된 리터럴 덩어리를 택배출고(변수반환) 합니다!
    }

    if (match_token(parser, TOKEN_NUMBER)) { // 까보니까 문자열이 아니고 혹시 파란색 아라비아 숫자(NUMBER) 토큰이 들어왔다면 그것도 냅다 삼킴!
        token = previous(parser);            // 삼켰던 그 숫자 토큰을 뱉어 꺼내서
        literal.type = LITERAL_NUMBER;       // 이 패키지 내용물 종류는 숫자(NUMBER) 타입 명찰로 명확히 확정!
        literal.text = sql_strdup(token->lexeme); // 비록 숫자의 의미지만 지금 C언어 컴파일러 보관법칙상 문자열 통째 그대로 안전한 복사본 방에 딥카피 저장!
        *ok = literal.text != NULL;          // 성공여부 세팅 보고!
        return literal; // 성공적으로 포장이 끝난 숫자 리터럴 덩어리를 쓩 외부로 반환!
    }

    // 최악의 경우: 만약 기대하던 문자도 아니고, 숫자도 아닌... 되도않는 특수 기호나 키워드가 우리가 데이터 값을 넣을 자리(VALUES 안쪽)에 잘못 기어들어왔다면!
    snprintf(parser->error_buf, // 에러 버퍼 공간에
             parser->error_buf_size,
             "리터럴이 필요합니다. 현재 토큰=%s('%s')",  // 여기엔 진짜 상수값이 모셔야 합니다 하고 꾸짖는 에러메시지와 함께
             token_type_name(peek(parser)->type), // 잘못 기어들어온 빌런 토큰의 정체 파악 이름
             peek(parser)->lexeme);               // 유저가 잘못 쳤던 본문 텍스트 내역 
    *ok = 0; // 으악 도무지 상수 패키지 조합에 실패했다고 바깥쪽 참조 변수에 깃발을 마구 흔들고
    return literal; // 터진 패키지 껍데기 박스 상태 그대로 강제로 반환. (어차피 밖에서 파싱 실패 에러 처리됩니다)
}

// "SELECT id, name" 같이 여러개의 연속된 뽑을 컬럼 목록 조각들이 발견될 때, 그 여러개의 이름들을 관리자 문자열 배열 맨 끝에 차곡차곡 한칸씩 쌓아(확장 연장해)주는 보조 함수
static int append_column(ColumnList *columns, const char *name) {
    char **new_items; // 아이템을 계속 더 끝도없이 담고자 새롭게 넓혀 재계약할 배열 공간 포인터
    char *copy;       // 이름을 복사해둘 작은 단어장 공간

    // 현재 기둥이 담고 있는 원래 배열 개수(count) + 1칸인 새 공간으로 배열 부지를 강제로 확장(realloc) 합니다.
    new_items = (char **)realloc(columns->items, (columns->count + 1) * sizeof(char *)); 
    if (new_items == NULL) { // 메모리 재할당 증축 공사에 안타깝게도 실패했다면
        return 0; // 확장 불가함을 알리는 심각한 오류 0 반환
    }
    columns->items = new_items; // 성공했으면 배를 불리고 넓힌 쾌적하고 넉넉해진 새 공간 주소로 바꿔치기 연결합니다.

    copy = sql_strdup(name); // 건네받은 새 컬럼 이름을, 잘 분리된 아방궁 같은 새로운 독립 공간에 똑 잘라서 딥카피 복사합니다.
    if (copy == NULL) {      // 이 복사 도중에도 용량이 모자라 실패 시
        return 0; // 돌이킬수없는 오류 0 반환
    }

    columns->items[columns->count] = copy; // 마침내 넓혀놓은 새 큰 배열 맨 끄트머리 방(인덱스)에 방금 복사해온 새 이름을 예쁘게 착 꽂아넣습니다!
    columns->count++; // 방 한칸 찼으니 개수 카운터 1 증가 갱신
    return 1; // 기분좋은 무사 추가 완료 성공 보고(1)
}

// 덩어리형 `SELECT id, name` 명령어 배열 중 앞의 명령을 뺀 `id, name` 뒷부분 이름들 나열만 전담해서 해석하고 구문 컬럼 리스트(ColumnList) 관리구조체로 잘 묶어 반환하는 전문 함수
static int parse_column_list(Parser *parser, ColumnList *columns) {
    const Token *identifier; // 우리가 컬럼으로 빼올 식별자 이름 토큰 하나

    memset(columns, 0, sizeof(*columns)); // 결과로 돌려줄 컬럼배열 관리자용 외부 구조체를 제일 먼저 싹 비우고 초기화 청소시킵니다.

    if (match_token(parser, TOKEN_STAR)) { // 만일 컬럼을 낱낱이 귀찮게 지정한 게 아니라, 별표(*)라는 편의성 토큰 하나로 치고 빠졌다면?
        columns->is_star = 1; // "음. 이 유저는 편하게 전체(Star) 컬럼 조회를 원했군!" 이라는 특수 처리 표식(플래그)을 달아줍니다.
        return 1; // 다른 컬럼명 더 찾을것 없이 조사가 이미 성큼 끝났으니 바로 깔끔하게 성공 1 반환!
    }

    // 만일 별표(STAR)가 뜬 게 아니라면, 이 뒤는 무조건 컬럼명인 '사용자 지정 일반 식별자 이름' 이 나와줘야만 말이 됩니다!
    if (!consume_token(parser, TOKEN_IDENTIFIER, "SELECT 뒤에는 '*' 또는 컬럼 이름이 와야 합니다.")) {
        return 0; // 식별자도 아니고 숫자나 이상한 기호나 홑따옴표가 오면 문법 파괴 에러치고 실패 완전 반납!
    }

    // 위의 무시무시한 Consume 확인을 통과했다는 뜻은 첫 컬럼 이름을 당당하게 무사히 잡았다는 뜻이니
    identifier = previous(parser); // 삼켰던 첫 식별자 토큰을 지갑에서 꺼내 가져와서
    if (!append_column(columns, identifier->lexeme)) { // 우리가 만든 배열 꼬리 추가 함수를 이용해 빈 배열에 영광스런 첫 번째 등록 시도!
        snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다."); // 이마저도 실패하면 안타까운 메모리 에러..
        return 0;
    }

    // 자 이제 첫 번째 이름 뒤에, 혹시 단절의 쉼표, 즉 콤마(,) 가 계속 이어져 나오는지를 while 문을 돌리며 무한 확인 검사합니다!
    while (match_token(parser, TOKEN_COMMA)) { 
        // 콤마가 나왔다면, 반드시 쉼표 논리 구조 상 바로 그 뒤에 또 새롭게 이어지는 "다음 추가 컬럼 이름"이 당당히 등장해야만 합니다. 뒤가 비었거나 다른거면 위반입니다.
        if (!consume_token(parser, TOKEN_IDENTIFIER, "콤마 뒤에는 컬럼 이름이 와야 합니다.")) {
            return 0; // 콤마치고 이상한 구문 오면 가차없이 탈락
        }
        identifier = previous(parser); // 무사통과했으니 콤마 뒤에 나온 다음 이름 토큰도 당당히 캐치 포획!
        if (!append_column(columns, identifier->lexeme)) { // 두 번째 녀석, 세 번째 녀석 찾은 이름들도 계속 차례대로 배열 끝에 줄줄이 비엔나처럼 연장시켜 등록!
            snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
            return 0;
        }
    }

    return 1; // 콤마 릴레이가 언젠가 멈추고 컬럼 이름들의 추출이 모두 무사히 성립되면, 완벽한 리스트 완성 공표! (어깨 두드려주며 1 반환)
}

// "INSERT INTO users (name, id)" 같이 괄호 안에 직접 적어준 입력 컬럼 순서를 읽어들이는 전용 파서 함수
static int parse_insert_column_list(Parser *parser, ColumnList *columns) {
    const Token *identifier;

    memset(columns, 0, sizeof(*columns)); // 혹시 이전 쓰레기가 섞이지 않도록 입력 컬럼 목록 구조체도 깨끗이 초기화합니다.

    if (!consume_token(parser, TOKEN_IDENTIFIER, "INSERT 컬럼 목록의 첫 항목에는 컬럼 이름이 와야 합니다.")) {
        return 0;
    }

    identifier = previous(parser);
    if (!append_column(columns, identifier->lexeme)) {
        snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
        return 0;
    }

    while (match_token(parser, TOKEN_COMMA)) {
        if (!consume_token(parser, TOKEN_IDENTIFIER, "INSERT 컬럼 목록에서 콤마 뒤에는 컬럼 이름이 와야 합니다.")) {
            return 0;
        }

        identifier = previous(parser);
        if (sql_string_array_contains(columns->items, columns->count, identifier->lexeme)) {
            snprintf(parser->error_buf,
                     parser->error_buf_size,
                     "INSERT 컬럼 목록에 같은 컬럼이 중복되어 있습니다: %s",
                     identifier->lexeme);
            return 0;
        }

        if (!append_column(columns, identifier->lexeme)) {
            snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
            return 0;
        }
    }

    return 1;
}

// WHERE 절의 비교 연산자를 읽어 CompareOperator 로 변환하는 함수
static int parse_compare_operator(Parser *parser, CompareOperator *out_operator) {
    if (match_token(parser, TOKEN_EQUALS)) {
        *out_operator = COMPARE_EQUALS;
        return 1;
    }
    if (match_token(parser, TOKEN_NOT_EQUALS)) {
        *out_operator = COMPARE_NOT_EQUALS;
        return 1;
    }
    if (match_token(parser, TOKEN_LESS_EQUAL)) {
        *out_operator = COMPARE_LESS_THAN_OR_EQUAL;
        return 1;
    }
    if (match_token(parser, TOKEN_LESS_THAN)) {
        *out_operator = COMPARE_LESS_THAN;
        return 1;
    }
    if (match_token(parser, TOKEN_GREATER_EQUAL)) {
        *out_operator = COMPARE_GREATER_THAN_OR_EQUAL;
        return 1;
    }
    if (match_token(parser, TOKEN_GREATER_THAN)) {
        *out_operator = COMPARE_GREATER_THAN;
        return 1;
    }

    snprintf(parser->error_buf,
             parser->error_buf_size,
             "WHERE 에서는 =, !=, <>, <, <=, >, >= 비교만 지원합니다. 현재 토큰=%s('%s')",
             token_type_name(peek(parser)->type),
             peek(parser)->lexeme);
    return 0;
}

// "WHERE id = 5" 와 같은 조건 제한/제약 파트를 분석하고 WhereClause 구조체에 이 제약 논리 정보들을 쏙쏙 채워 한방에 조립해 주는 특화 함수
static int parse_where_clause(Parser *parser, WhereClause *where) {
    int ok;

    memset(where, 0, sizeof(*where)); // 반환해 주기로 한 빈 조건절 구조체를 먼지 하나 없이 깨끗이 세탁 초기화합니다.

    // WHERE라는 무서운 시작 시작 단어 뒤에는 반드시 논리적으로 '비교 대상이 될 컬럼의 이름' 이 먼저 등장해야 합니다.
    if (!consume_token(parser, TOKEN_IDENTIFIER, "WHERE 뒤에는 컬럼 이름이 와야 합니다.")) {
        return 0; // 이상한게 와서 검거 실패시 즉각 0 반환
    }

    where->column_name = sql_strdup(previous(parser)->lexeme); // 확인된 식별자 토큰에서 "id" 같은 글자만 쏴악 깊은 복사해옵니다.
    if (where->column_name == NULL) { // 메모리 누수나 용량 부족
        snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
        return 0;
    }

    // 비교할 컬럼 이름 다음에는 지원하는 비교 연산자가 와야 합니다.
    if (!parse_compare_operator(parser, &where->op)) {
        return 0; // 부등호나 IN 등 다른거 못씀. 실패
    }

    // 등호(=) 기호 다음에는 무조건 비교할 대상 '상수 값 데이터(Literal)'가 나와야 합니다. 이를 파싱하여 구조체에 통째로 담아 넣습니다.
    where->value = parse_literal(parser, &ok); 
    if (!ok) { // 만일 값으로 예쁘게 파싱 추출하는 데 실패했다면
        return 0; // 분석 기권. 실패 0 반환하고 던져버림.
    }

    return 1; // "컬럼명" "=" "데이터상수값" 이라는 완전체 3단 콤보가 구성, 완성되면 마침내 조건절 조립 성공 선언! (1)
}

// 이 함수가 실행 호출되었다는 것은 맨 앞 문장 시작단어가 "SELECT" 였음이 재판에서 확실시 될 때입니다. 즉 전체적인 SelectStatement 조립 설계도면을 지휘하는 메인 파서 공장장!
static int parse_select(Parser *parser, Statement *statement) {
    SelectStatement *select_stmt = &statement->as.select_stmt; // 최종 완성품으로 나갈 커다란 상자(statement) 내부의 SELECT 전용 취급 공간으로 쓱 비집고 들어갑니다.

    memset(select_stmt, 0, sizeof(*select_stmt)); // 행여 더미 데이터가 발목을 잡을수있으니 전용 방 청소 세탁 철저
    statement->type = AST_SELECT_STATEMENT;       // 아예 밖의 껍데기 래퍼에 "이 박스의 진짜 정체 내용물은 SELECT 구문임!" 하고 라벨 표식 도장을 척 찍어붙입니다.

    // SELECT 단어 직후에 나타나야 할 그 수많은 타겟 컬럼 리스트들(id, name 등 혹은 별표)을 파싱하여, 내부에 마련해둔 공간에 팍 박아넣습니다.
    if (!parse_column_list(parser, &select_stmt->columns)) {
        return 0; // 중간에 이놈이 터졌으면 나도 같이 죽고 0 반환 실패
    }

    // 컬럼명 나열과 분석이 무사히 끝났다면, 이제 데이터들을 도대체 '어디서 가져와 읽어야(조회)' 할지를 묻는 'FROM' 예약 토큰이 와야 합니다.
    if (!consume_token(parser, TOKEN_FROM, "SELECT 목록 뒤에는 FROM이 와야 합니다.")) {
        return 0;
    }

    // FROM 예약어 뒤에는 또다시 어디에 들어있는지 묻는 일반 식별자(테이블 폴더 이름)가 당당히 위치해야 합니다.
    if (!consume_token(parser, TOKEN_IDENTIFIER, "FROM 뒤에는 테이블 이름이 와야 합니다.")) {
        return 0;
    }

    select_stmt->table_name = sql_strdup(previous(parser)->lexeme); // 방금 허락맞고 통과된 타겟 테이블 이름을 이 구조체에 최종 목적지 테이블명으로서 안착, 이식시킵니다.
    if (select_stmt->table_name == NULL) {
        snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
        return 0;
    }

    // 만약 여기까지 별 문제 없이 정말 무사히 조립이 잘 끝나고 뒤쪽 커서를 슬쩍 봤는데 뜻밖에도 부가옵션인 제약조건 'WHERE' 예약 토큰이 있다면?
    if (match_token(parser, TOKEN_WHERE)) {
        select_stmt->has_where = 1; // "앗! 단순 조회가 아니라 필터링 건 조건절 조회도 원했었군!" 하고 옵션 활성화 다이얼(플래그 1)을 딸깍 누르고
        if (!parse_where_clause(parser, &select_stmt->where)) { // WHERE만을 깊게 분석전담하는 파쇄 함수를 긴급 출동시켜서 나머지 디테일 상세 조건들을 다 긁어오게 처리합니다.
            return 0; // 여기서 꼬이면 역시 0 반환
        }
    }

    return 1; // 옵션 포함 모든 SELECT 구문 입체 트리 완성! 대성공! 외부로 결재 완료 처리. (1 반환)
}

// "INSERT INTO users VALUES (1, 'Alice');" 구문에서 진짜 넣을 값인 (1, 'Alice') 안에 포진하고 있는 무수한 값(Literal) 배열들을 하나씩 공간을 늘리면서 리스트로 지어주는 내부 보조 함수
static int append_value(InsertStatement *insert_stmt, Literal literal) {
    Literal *new_items; // 계속 뒤로 주욱 들어올지 모르는 수많은 값들을 담기 위해 넉넉한 배열 집을 넓힐 포인터 준비

    // 여태 구조체가 짊어지고 있던 개수 + 1 크기만큼 리터럴 박스 전용 공간들을 강제 재할당(realloc) 대규모 증축 지시
    new_items = (Literal *)realloc(insert_stmt->values, (insert_stmt->value_count + 1) * sizeof(Literal));
    if (new_items == NULL) { // 메모리 문제로 집 증축 실패 시 
        return 0; // 반환 실패
    }

    insert_stmt->values = new_items; // 새롭게 불려져 넓어진 아늑한 큰 방으로 기존 베이스 캠프 주소를 갈아타고
    insert_stmt->values[insert_stmt->value_count] = literal; // 가장 끝쪽에 확장되어 존재하게 된 새 빈방(여백 인덱스)에 방금 전해 전달받은 데이터 리터럴(literal) 덩이를 턱 올려놓습니다.
    insert_stmt->value_count++; // 무사히 하나 넣었으니 보관 용량 1 증가 갱신
    return 1; // 더 박기 추가 성공 반환
}

// 이 공정이 열렸다는 건 제일 처음 앞단어가 "INSERT(추가 삽입)" 였을 때 확실히 호출된겁니다. 즉, 전체적인 InsertStatement 의 논리 구조 삽입 트리를 조립해나가는 특급 전담파서!
static int parse_insert(Parser *parser, Statement *statement) {
    InsertStatement *insert_stmt = &statement->as.insert_stmt; // 최종 거대 포장 껍데기(아웃 스테이트먼트) 내부의 INSERT 전용 공작실 파쇄 공간으로 들어갑니다.
    Literal literal; // 무수하게 꺼내질 개별적인 값들을 임시로 1단위 포장할 박스
    int ok;

    memset(insert_stmt, 0, sizeof(*insert_stmt)); // 구역 정리, 청소 세탁
    statement->type = AST_INSERT_STATEMENT;       // 아예 가장 바깥 큰 껍데기의 속성을 확고부동한 INSERT 형이라고 라벨 도장 확! 찍음!

    // 명색이 INSERT 조립 공정이라면, 단어 뒤에는 반드시 목적지를 의미하는 'INTO' 예약 토큰이 세트로 와야 합니다.
    if (!consume_token(parser, TOKEN_INTO, "INSERT 뒤에는 INTO가 와야 합니다.")) {
        return 0;
    }

    // INTO 예약어 뒤에는, 도대체 내 값들을 받아 줄 목적 장소인지 그 대상 폴더(테이블) 식별자 이름이 와야 합니다.
    if (!consume_token(parser, TOKEN_IDENTIFIER, "INTO 뒤에는 테이블 이름이 와야 합니다.")) {
        return 0;
    }

    insert_stmt->table_name = sql_strdup(previous(parser)->lexeme); // 무사 확인된 대상을 우리의 테이블 이름값으로 확고히 기록 등록!
    if (insert_stmt->table_name == NULL) {
        snprintf(parser->error_buf, parser->error_buf_size, "메모리 할당에 실패했습니다.");
        return 0;
    }

    // 테이블 이름 바로 뒤에 괄호가 붙었다면, 이는 사용자가 원하는 입력 컬럼 순서를 직접 적어준 경우로 해석합니다.
    if (match_token(parser, TOKEN_LPAREN)) {
        if (!parse_insert_column_list(parser, &insert_stmt->columns)) {
            return 0;
        }

        if (!consume_token(parser, TOKEN_RPAREN, "INSERT 컬럼 목록 뒤에는 ')' 가 와야 합니다.")) {
            return 0;
        }
    }

    // 기존 VALUES 구문은 유지하되, 이제는 선택적으로 INSERT INTO table (col1, col2) VALUES (...) 도 허용합니다.
    if (!consume_token(parser, TOKEN_VALUES, "현재 MVP버전에서는 INSERT INTO table [(col1, col2, ...)] VALUES (...) 형태만 지원합니다.")) {
        return 0;
    }

    // 삽입 값들의 나열을 의미상 한데 이쁘게 모아주는 수학적인 열기 기호인 왼쪽 둥근 괄호 '(' 가 반드시 이 자리에 필요합니다. 
    if (!consume_token(parser, TOKEN_LPAREN, "VALUES 뒤에는 '(' 가 와야 합니다.")) {
        return 0;
    }

    // 이제 대망의 대기중 큐를 열고 첫 번째 데이터를 뽑습니다. 이를 값(Literal) 패키지 단위로 쏙 포장생성합니다.
    literal = parse_literal(parser, &ok); 
    if (!ok || !append_value(insert_stmt, literal)) { // 만일 상수 파싱에 애초에 실패했거나 늘려주는 배열에 이쁘게 끼워넣기에 실패했다면
        snprintf(parser->error_buf, parser->error_buf_size, "INSERT 값 저장에 실패했습니다.");
        if (ok) { // 만일 배열 저장은 실패했는데, 포장 생계유지 자체는 외롭게도 성공해있었다면
            free_literal(&literal); // 낙동강오리알 되어 우주를 떠돌 패키지(공간)를 도로 빡세게 해제해 주어 메모리 누수 방지 조치!
        }
        return 0;
    }

    // 괄호 안에 데이터가 1개만 매정하게 있는 게 아니라, 뒤를 이을 쉼표(,)가 계속 나오는 한 무한 반복루프 돌면서 남은 값들을 계속 쉼없이 주워 담습니다.
    while (match_token(parser, TOKEN_COMMA)) {
        literal = parse_literal(parser, &ok); // 쉼표는 버리고 다음 알맹이 글자도 값(Literal) 패키지로 포장!
        if (!ok || !append_value(insert_stmt, literal)) { // 위와 같은 메모리 실패 등 예외 처리 조작
            snprintf(parser->error_buf, parser->error_buf_size, "INSERT 값 저장에 실패했습니다.");
            if (ok) {
                free_literal(&literal);
            }
            return 0; // 에러치고 조립파업. 실패 반환
        }
    }

    // 쉼표 릴레이가 다 끝나고 났을 땐, 이제 더 넣을 게 없으니 필연적으로 열었던 괄호를 마감 닫혀주는 기호인 오른쪽 둥근 괄호 ')' 가 딱 맞물려 와야 완벽한 문법이 끝납니다!
    if (!consume_token(parser, TOKEN_RPAREN, "VALUES 목록 뒤에는 ')' 가 와야 합니다.")) {
        return 0;
    }

    if (insert_stmt->columns.count > 0 && insert_stmt->columns.count != insert_stmt->value_count) {
        snprintf(parser->error_buf,
                 parser->error_buf_size,
                 "INSERT 컬럼 개수(%zu)와 VALUES 안의 값 개수(%zu)가 서로 다릅니다.",
                 insert_stmt->columns.count,
                 insert_stmt->value_count);
        return 0;
    }

    return 1; // 데이터 삽입용 테이블 완전 트리 거대 구조 성립 및 조립 완전 완료! 대성공 리턴(1).
}

// **파서 대장군 본체**: 어떤 평면적인 순차 명령 배열(토큰)들이든 모두 통째로 받아서 그 문맥적 의도를 컴퓨터가 알기 편한 계층형 나무 구조(Statement(AST)) 로 깊이 번역 및 통괄하는 핵심 파쇄 반환기
int parse_statement(const TokenArray *tokens, Statement *out_statement, char *error_buf, size_t error_buf_size) {
    Parser parser; // 지금 내가 어느 토큰을 들여다보고 있는지 등등 수많은 현재 위치 상태를 기록해둘 보급병(Parser) 구조체 생성

    memset(out_statement, 0, sizeof(*out_statement)); // 최종 반환할 결과물 상자를 깨끗하게 잡티하나 없이 세탁
    memset(&parser, 0, sizeof(parser));               // 파서 위치 보급병 구조체도 싹 세탁 초기화
    parser.tokens = tokens;                           // 읽을 원본 토큰들 리스트 전체를 공장라인에서 넘겨줌
    parser.error_buf = error_buf;                     // 에러 터졌을 때 쓸 바깥 통신용 빨간펜 노트
    parser.error_buf_size = error_buf_size;           // 빨간펜 노트 장수 물리적 한계

    // 자, 대망의 첫 걸음. 맨 첫 번째 글자(토큰) 조각을 꺼내서 이게 그 유명한 "SELECT" 조회 예약어였는지 감별 비교하며 맞다면 꿀꺽 먹어봅니다.
    if (match_token(&parser, TOKEN_SELECT)) {
        if (!parse_select(&parser, out_statement)) { // 오예 조회명령인 SELECT가 확실하다면, SELECT 구문 전용 전문 분석 함수에게 모든 조립을 왕창 떠넘깁니다.
            free_statement(out_statement); // 중간에 조작하다 뻑났거나 에러가 났다면 반환 시 쓰레기를 주면 안되니 조립중이던 미완성 구조물을 싹 압수분해하여 해제하고
            return 0; // 실패 리턴!
        }
    } 
    // 첫 단어가 만약 SELECT가 아니었나? 그럼 차선책으로 혹시 "INSERT" 추가 예약어였는지 비교하고 맞으면 꿀꺽 먹습니다.
    else if (match_token(&parser, TOKEN_INSERT)) {
        if (!parse_insert(&parser, out_statement)) { // 오, INSERT가 맞다면, 추가 명령 INSERT 전담 전문 분석 공장으로 하청보내서 모든 조립을 떠넘깁니다.
            free_statement(out_statement); // 이것도 도중 터졌으면 해체분해조립기 돌리고 폐기!
            return 0; // 실패 리턴!
        }
    } 
    // 그 외에 우리가 안타깝게도 만들지 못했거나 다루지 않는 UPDATE, DELETE같이 이상한 단어가 맨 처음 오면 취급 불가 에러를 즉각 발생.
    else {
        snprintf(error_buf,
                 error_buf_size,
                 "지원하지 않는 구문, 문장입니다. 첫 확인 토큰=%s('%s')",
                 token_type_name(peek(&parser)->type), // "으엥 무려 식별자(IDENTIFIER) 따위가 첫 단어라니요" 식의 조롱 에러이름
                 peek(&parser)->lexeme); // 유저가 친 괴상한 첫 시작단어 원문
        return 0;
    }

    // 만일 위에서 명령 뭉치 거대 조립이 무사히 끝났다면, 그 뒤엔 필연적으로 문맥 마무리가 끝남을 의미하는 "세미콜론(;)"이 와야 하는데, 그걸 살짝 검사하여 버려봅니다. (세미콜론 없어도 그냥 뒤로 넘어가고, 있다면 먹어서 없앰)
    match_token(&parser, TOKEN_SEMICOLON); 

    // 세미콜론이 있든 없든 다 상관없지만, 가장 확실하고 중요한 점은 **이제 뒤에 남은 여분의 찌꺼기 토큰이 아예 1개도 없어야(EOF) 한다는 점** 입니다!
    // 하나의 온전한 명령문장을 완성조립완료했으니 완전한 끝에 도달했는지 확인, 만일 문장이 2개 3개 연달아 쓰여있거나 뒷말이 흐려있다면(EOF가 아니면)
    if (!consume_token(&parser, TOKEN_EOF, "SQL 파일에는 현재 1회당 단 한 개의 문장 처리만 가능, 지원하고 있습니다.")) {
        free_statement(out_statement); // 얄짤없이 빡치면서 만들어둔 트리 압수분해 삭제
        return 0;
    }

    return 1; // 기나긴 장정을 마쳤네요. 여기까지 온 건 모든 명령 조립과 의미 구문 분석 트리 변환이 단 하나도 부서짐 없이 완벽히 입체적으로 퍼즐 완성되었다는 의미!!
}
