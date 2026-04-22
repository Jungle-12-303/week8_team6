# 코드 리뷰용 한눈에 보기

이 문서는 VS Code 에서 코드 리뷰할 때 “어떤 함수가 왜 있고, 요청이 어떤 순서로 흘러가는지”를 빠르게 설명하기 위한 자료입니다.

줄 번호는 현재 주석 작업 후 기준입니다. 코드가 더 수정되면 줄 번호는 조금 달라질 수 있습니다.

## 전체 시퀀스

```text
[Thunder Client]
    |
    |  POST http://127.0.0.1:8080/query
    |  Body: SELECT * FROM users WHERE id = 777777;
    v
src/apps/server_main.c:91 main()
    |
    |  실행 옵션을 ApiServerConfig 로 정리
    v
src/api/api_server.c:1320 api_server_run()
    |
    +--> db_api_init()
    |       기존 SQL 엔진과 DB mutex 준비
    |
    +--> thread_pool_create()
    |       worker thread N개를 미리 생성
    |
    +--> open_listener()
    |       socket -> bind -> listen
    |
    +--> accept() loop
            |
            |  클라이언트 연결 1개를 ClientTask 로 포장
            v
        thread_pool_submit(handle_client, task)
            |
            v
src/concurrency/thread_pool.c:175 worker_main()
            |
            |  queue 에서 작업 pop
            v
src/api/api_server.c:1164 handle_client()
            |
            +--> read_http_request()
            |       HTTP method/path/body 읽기
            |
            +--> [라우트 분기]
                    |
                    +-- GET /health
                    |       send_json()
                    |
                    +-- GET /metrics
                    |       format_metrics_body() -> send_json()
                    |
                    +-- POST /query
                    |       handle_query()
                    |
                    +-- 그 외
                            404 JSON 응답
```

## POST /query 내부 시퀀스

```text
handle_client()
    |
    v
handle_query()
    |
    +--> extract_sql_text()
    |       |
    |       +-- body 가 JSON 이면 extract_json_sql_string()
    |       +-- body 가 raw text 이면 duplicate_trimmed_body()
    |
    +--> db_api_execute_sql()
    |       |
    |       +--> tmpfile()
    |       |       기존 엔진 출력 캡처용 FILE*
    |       |
    |       +--> mutex_lock()
    |       |       users.tbl/users.idx/SqlEngine 공유 상태 보호
    |       |
    |       +--> sql_engine_execute_sql()
    |       |       SQL 파싱 후 SELECT/INSERT 실행
    |       |
    |       +--> read_stream_to_string()
    |       |       FILE* 결과를 char* 로 변환
    |       |
    |       +--> mutex_unlock()
    |
    +--> format_query_body()
            |
            +-- 성공
            |   {"ok":true, "result":"CSV...", "columns":[], "rows":[], "stats":...}
            |
            +-- 실패
                {"ok":false, "error":"..."}
```

## SELECT 분기 시나리오

```text
database_execute_select()
    |
    +--> load_table()
    |       .tbl/.idx 준비, 필요하면 인덱스 재생성
    |
    +--> build_output_indices()
    |       SELECT * 또는 SELECT name,email 컬럼 목록 결정
    |
    +--> [WHERE 조건 분기]
            |
            +-- WHERE id <op> value
            |       execute_indexed_select()
            |       B+ tree 로 id -> 파일 offset 탐색
            |       fseek(offset) 후 필요한 row 만 읽음
            |
            +-- WHERE 없음 또는 non-id 조건
                    execute_linear_select()
                    users.tbl 을 처음부터 끝까지 한 줄씩 검사
```

## INSERT 분기 시나리오

```text
database_execute_insert()
    |
    +--> load_table()
    |
    +--> build_insert_row()
    |       |
    |       +-- id 제공됨
    |       |       중복 id 인지 B+ tree 로 확인
    |       |
    |       +-- id 생략됨
    |               table->next_id 로 자동 부여
    |
    +--> 값 검증
    |       현재 CSV 저장 포맷상 콤마/줄바꿈은 거절
    |
    +--> users.tbl append
    |
    +--> bptree_insert(id, row_offset)
            새 row 의 파일 위치를 인덱스에 추가
```

## 함수 한눈에 보기

### src/apps/server_main.c

| 줄 | 함수 | 하는 일 | 왜 필요한가 |
|---:|---|---|---|
| 46 | `print_usage()` | 실행 옵션과 API 목록 출력 | `--help` 로 시연자가 바로 사용법을 확인 |
| 60 | `parse_int_arg()` | port 같은 int 옵션 검증 | 잘못된 포트 입력을 서버 시작 전에 차단 |
| 75 | `parse_size_arg()` | thread/queue/max-body 옵션 검증 | 0 또는 문자 입력 같은 설정 오류 차단 |
| 91 | `main()` | CLI 옵션을 읽고 `api_server_run()` 호출 | 프로그램 진입점, 설정 조립 담당 |

### src/api/api_server.c

| 줄 | 함수 | 하는 일 | 왜 필요한가 |
|---:|---|---|---|
| 182 | `duplicate_range()` | 버퍼 일부를 heap 문자열로 복사 | HTTP body, CSV field 같은 부분 문자열 보관 |
| 198 | `starts_with_ci()` | 대소문자 무시 prefix 비교 | HTTP 헤더 이름 비교 |
| 216 | `request_free()` | 요청 body 메모리 해제 | 요청 1개 처리 후 누수 방지 |
| 226 | `find_header_end()` | HTTP header/body 경계 탐색 | body 를 어디서부터 읽을지 판단 |
| 251 | `parse_content_length()` | `Content-Length` 값 파싱 | body byte 수 계산 |
| 315 | `read_http_request()` | 소켓에서 HTTP 요청 1개 읽기 | TCP stream 에서 header/body 를 직접 조립 |
| 417 | `duplicate_trimmed_body()` | raw SQL body 앞뒤 공백 제거 | Thunder Client Text body 지원 |
| 436 | `extract_json_sql_string()` | JSON body 의 `sql` 필드 추출 | `{"sql":"..."}` 요청 지원 |
| 513 | `extract_sql_text()` | raw SQL/JSON body 분기 | API 입력 형식을 SQL 문자열로 통일 |
| 531 | `json_escape()` | JSON 문자열 escape | 줄바꿈/따옴표 때문에 응답 JSON 이 깨지는 것 방지 |
| 584 | `builder_init()` | 동적 문자열 버퍼 초기화 | rows JSON 을 안전하게 조립 |
| 607 | `builder_reserve()` | 동적 문자열 버퍼 확장 | 큰 SELECT 결과 대응 |
| 760 | `append_table_projection_json()` | CSV 결과를 `columns/rows/rowCount` 로 변환 | Thunder Client 에서 row 를 보기 좋게 표시 |
| 884 | `format_table_projection_json()` | CSV 변환 wrapper | 실패 처리와 heap 문자열 반환 통일 |
| 910 | `format_query_body()` | DbApiResult 를 최종 JSON body 로 변환 | 성공/실패 응답 형식 통일 |
| 970 | `metrics_record()` | 요청 통계 증가 | `/metrics` 표시와 동시성 안전성 |
| 1017 | `send_all()` | socket 에 모든 byte 전송 | `send()` 부분 전송 문제 방지 |
| 1043 | `send_json()` | HTTP header + JSON body 전송 | 모든 API 응답 공통 처리 |
| 1072 | `send_error()` | 공통 에러 JSON 전송 | 파싱/라우팅 실패 응답 통일 |
| 1108 | `handle_query()` | POST `/query` 처리 | SQL 실행의 API 진입점 |
| 1164 | `handle_client()` | 요청 읽기와 라우팅 분기 | worker thread 의 실제 작업 |
| 1211 | `network_startup()` | Windows socket 초기화 | Windows/macOS/Linux 호환 |
| 1242 | `open_listener()` | listen socket 생성 | 외부 클라이언트 접속 대기 |
| 1320 | `api_server_run()` | 서버 전체 생명주기 실행 | DB, thread pool, socket, accept loop 관리 |

### src/concurrency/thread_pool.c

| 줄 | 함수 | 하는 일 | 왜 필요한가 |
|---:|---|---|---|
| 175 | `worker_main()` | queue 에서 작업을 꺼내 실행 | 요청 병렬 처리의 실제 worker 루프 |
| 215 | `cleanup_pool()` | pool heap 메모리 해제 | 생성 실패/종료 정리 |
| 237 | `thread_pool_create()` | worker thread 와 queue 생성 | 서버 시작 시 병렬 처리 준비 |
| 300 | `thread_pool_submit()` | accept 된 client 작업을 queue 에 push | 요청마다 새 thread 생성 방지 |
| 337 | `thread_pool_destroy()` | worker 종료 대기 후 자원 해제 | 안전한 서버 종료 |

### src/api/db_api.c

| 줄 | 함수 | 하는 일 | 왜 필요한가 |
|---:|---|---|---|
| 99 | `read_stream_to_string()` | FILE* 출력 전체를 문자열로 변환 | 기존 엔진 출력 결과를 JSON 으로 감싸기 위함 |
| 139 | `db_api_init()` | SqlEngine 과 mutex 초기화 | API 서버와 DB 엔진 연결 준비 |
| 174 | `db_api_free()` | SqlEngine 과 mutex 해제 | 서버 종료 시 자원 정리 |
| 200 | `db_api_execute_sql()` | SQL 1개 실행, 결과 캡처 | HTTP 요청을 기존 SQL 엔진으로 연결 |
| 259 | `db_api_result_free()` | 결과 문자열 해제 | 요청별 heap 메모리 정리 |

### src/storage/database.c

| 줄 | 함수 | 하는 일 | 왜 필요한가 |
|---:|---|---|---|
| 291 | `build_index()` | `.tbl` 을 읽어 id -> row offset 인덱스 생성 | `WHERE id` 빠른 조회 |
| 345 | `count_table_rows()` | 실제 테이블 row 수 계산 | 인덱스 최신성 검증 |
| 372 | `table_file_newer_than_index()` | `.tbl` 수정 시간이 `.idx` 보다 최신인지 확인 | stale index 감지 |
| 388 | `rebuild_index()` | 오래된 인덱스를 삭제하고 재생성 | 잘못된 offset 조회 방지 |
| 420 | `load_table()` | 테이블 schema, 파일 경로, 인덱스 준비 | SELECT/INSERT 전 공통 준비 |
| 702 | `execute_indexed_select()` | B+ tree 로 SELECT 수행 | id 조건 조회 성능 확보 |
| 761 | `execute_linear_select()` | 파일 전체 scan 으로 SELECT 수행 | non-id 조건과 전체 조회 지원 |
| 842 | `database_execute_select()` | SELECT 최상위 분기 | 인덱스/선형 탐색 선택 |
| 940 | `build_insert_row()` | INSERT 값을 테이블 컬럼 순서로 재배열 | 컬럼 목록 INSERT 와 자동 id 지원 |
| 1055 | `database_execute_insert()` | row append 와 B+ tree 갱신 | INSERT 기능 구현 |

## 발표용 짧은 설명

```text
이 서버는 HTTP 요청을 thread pool worker 에게 넘기고,
worker 는 /query body 에서 SQL 을 꺼내 DbApi 로 보냅니다.
DbApi 는 기존 week7 SQL 엔진을 그대로 호출하되,
기존 엔진이 thread-safe 하지 않기 때문에 SQL 실행 구간만 mutex 로 보호합니다.
SELECT 는 WHERE id 조건이면 B+ tree 인덱스로 row offset 을 찾아 빠르게 읽고,
그 외 조건은 CSV 테이블 파일을 순차 스캔합니다.
INSERT 는 tbl 파일 끝에 row 를 붙인 뒤 B+ tree 에 id 와 파일 offset 을 추가합니다.
```
