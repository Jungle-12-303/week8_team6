#include "optimizer.h" // 내 뼈대인 옵티마이저 헤더를 가져옵니다

#include "util.h" // 문자 배열 중복 검사 등 편리한 툴킷이 있는 유틸 헤더를 씁니다

#include <stdio.h> // 에러 메시지 작성(snprintf)용 표준 입출력 헤더
#include <stdlib.h> // free 선언을 가져와 중복 컬럼 정리 시 메모리를 안전하게 해제하기 위한 헤더
#include <string.h> // 메모리 초기화(memset) 등 문자열, 메모리 조작 헤더

// 핵심 기능 1: SELECT 문장에서 유저가 실수로 'id, name, id' 같이 같은 컬럼을 또
// 똑같이 불러달라며 시간, 메모리 낭비를 일으킨 중복된 요청을 솎아내고 쳐내는
// 최적화 다이어트 함수
static void optimize_select(SelectStatement *select_stmt) {
  size_t write_index = 0; // 중복이 아닌 진짜배기 핵심 알맹이 단어들만 앞으로
                          // 당겨서 채워넣어줄 커서 위치
  size_t i; // 전체 배열을 한바퀴 뺑글뺑글 순회할 인덱스 커서

  // 설마 (*) 로 전체 검색인 거면 솎아낼 필요가 애초에 1도 없으므로 바로 탈출
  if (select_stmt->columns.is_star) {
    return;
  }

  // 솎아낼 필요가 있다면, 컬럼을 요구한 숫자만큼 반복해서 뒤집어 까봅니다!
  for (i = 0; i < select_stmt->columns.count; ++i) {
    // 유틸 폭탄마를 동원, 내가 이번에 살펴보는 이 [i] 위치의 단어가, 앞서
    // 쓰여진(0부터 write_index 구간까지) 곳에 이미 똑같이 적혀있는 단어냐?
    // 진실의 방 중복검사 실시
    if (!sql_string_array_contains(select_stmt->columns.items, write_index,
                                   select_stmt->columns.items[i])) {
      // 다행히 한 번도 안 쓰인 신규 컬럼 이름이라면?
      // "합격!" 판정을 때리고, 합격자 전용 커서(write_index) 위치로 이름표를 탁
      // 당겨서 넣어줍니다.
      select_stmt->columns.items[write_index] = select_stmt->columns.items[i];

      // 그리고 다음 합격자를 받기 위해 합격자석 커서를 1칸 뒤로 이동!
      write_index++;
    } else {
      // 안타깝게도 이미 앞에서 "나온" 컬럼 이름을 또 적어놓은 심각한 중복 낭비
      // 단어라면? "불합격!" 시키고, 당겨쓸 필요 없이 메모리에서 영구히
      // 삭제(free)시켜 파쇄해버립니다!
      free(select_stmt->columns.items[i]);
    }
  }

  // 뺑뺑이가 다 끝났습니다! 배열의 총 크기(count)를 중복이 제거되어 깔끔해진
  // 합격자들의 수(write_index)로 확 줄여 다이어트 결과를 확정 갱신시킵니다!
  select_stmt->columns.count = write_index;
}

// **옵티마이저 총괄 본체**: 파서가 만든 최초의 거칠고 거대한 트리(스트럭처)를
// 넘겨받아 군살을 빼고 다듬어 최고의 속도를 낼 준비를 마치는 메인 컨트롤러
int optimize_statement(Statement *statement, char *error_buf,
                       size_t error_buf_size) {
  // 엥? 넘겨받은 구조물이 백지 텅 빈 서류라면?
  if (statement == NULL) {
    snprintf(error_buf, error_buf_size,
             "최적화할 문장이 없습니다."); // 황당하다는 로그를 남기고
    return 0;                              // 작업 취소 선언 (0)
  }

  // 서류 종류 마크(type)가 SELECT 로 조회하는 작업 모델이라면
  if (statement->type == AST_SELECT_STATEMENT) {
    optimize_select(
        &statement->as.select_stmt); // 아까 만들어둔 SELECT 전용 중복 파쇄
                                     // 다이어트 부대를 투입시킵니다
  }
  // (참고: 현 버전에서 INSERT 등은 딱히 최적화로 군살 뺄 구조가 아직 없어 그냥
  // 통과패스시킵니다)

  // 모든 최적화 대수술이 무사히 다 끝났으면 성공(1) 반환!
  return 1;
}
