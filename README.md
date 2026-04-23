# Week8 Mini DBMS - API Server

외부 클라이언트가 HTTP API로 SQL을 보내면, C 기반 API 서버가 Thread Pool로 요청을 받아 기존 SQL 처리기와 B+ Tree 인덱스를 사용해 결과를 JSON으로 반환하는 미니 DBMS 서버입니다.

발표 한 줄 요약:

> 지난주 SQL 처리기와 B+ Tree 인덱스를 내부 DB 엔진으로 재사용하고, 그 앞에 C 기반 HTTP API 서버와 Thread Pool을 붙여 외부 클라이언트가 SQL을 실행할 수 있게 만들었습니다.

## 4분 발표 흐름

| 시간 | 설명할 내용 | 보여줄 자료 |
|---:|---|---|
| 0:00 - 0:30 | 과제 목표와 우리가 만든 결과물 | 요구사항 대응표 |
| 0:30 - 1:10 | 전체 아키텍처 | 전체 아키텍처 그림 |
| 1:10 - 1:50 | `POST /query` 요청 1개가 처리되는 과정 | 요청 처리 시퀀스 그림 |
| 1:50 - 2:30 | Thread Pool과 동시성 처리 | Thread Pool 그림 |
| 2:30 - 3:00 | 100회 요청 로그와 mutex 대기 확인 | `/health` vs `/query` 비교 그림 |
| 3:00 - 4:00 | Thunder Client 시연 | `/health`, `/query`, `/metrics` |

## 4분 발표 대본

# 4분 발표 대본

## 0:00 - 0:30 과제 목표

```text
안녕하세요 발표를 맡게된 303호 6팀의 최현진입니다  발표를 시작하겠습니다.
이번주차의 핵심은 전주차에 구현한 SQL처리기와  B+ Tree 인덱스를 차용하여
  dbms api 서버를 구현하는것 이었습니다.
저희 팀 목표는 HTTP API 서버와 Thread Pool 그리고 뮤텍스를 설계 및 구현하는데 집중하였습니다



```

## 0:30 - 1:10 전체 아키텍처

```text
전체 흐름은  HTTP로 SQL을 보내면 api_server가 요청을 받고,
Thread Pool worker가 실제 요청 처리를 담당합니다.

worker는 SQL 엔진을 호출하고,
결과를 받으면,
응답은 api_server에서 만들어서 내려줍니다.
```

## 1:10 - 1:50 요청 1개 처리 흐름

```text
POST /query 요청을 예시로 보면,
서버는 먼저 accept로 연결을 받고,
요청을 작업 단위로 만들어 스레드 풀 대기열에 넣습니다.

이후 worker가 작업을 꺼내 HTTP body에서 SQL 문자열을 읽고,
SQL 실행 함수를 호출해 기존 엔진을 실행합니다.

엔진 결과는 원래 CSV 문자열로 나오지만,
API 응답에서는 columns 정보를 추가해서
JSON 형태로 보기 좋게 구성했습니다.
```

## 1:50 - 2:30 Thread Pool과 동시성

```text
요청마다 스레드를 새로 만들면, 접속이 몰릴 때 스레드 수가 과하게 늘어날 수 있습니다.
그래서 서버 시작 시 작업 스레드 4개를 미리 만들어 두고 재사용했습니다.

서버의 물리 프로세서가 20개지만, DB 처리 구간이 mutex로 보호되어 있어서 SQL 실행은 실제로 병렬 처리되지 않습니다.
따라서 작업 스레드를 20개로 늘려도 처리량이 그만큼 비례해서 늘어나지는 않습니다.

이 부분을 설계하면서 추가로 알게 된 점이 있습니다.
운영체제의 backlog와 스레드 풀 대기열은 서로 다른 대기 공간입니다.
backlog는 accept되기 전의 커널 대기열이고, 스레드 풀 대기열은 accept된 뒤 작업 스레드를 기다리는 내부 대기열입니다.

내부 대기열이 가득 차면 accept 루프가 잠시 멈추고,
그 사이 새 연결은 운영체제 backlog에 쌓이게 됩니다.

이를 통해 요청이 처리되는 전체 흐름과 대기 구조까지 이해할 수 있었습니다.

```

## 2:30 - 3:00 부하 로그와 mutex

```text
각 요청 유형에 따른 응답 시간을 시각화한 그래프입니다.
`/health` 요청은 디비를  거치지 않는 구조이기 때문에 요청이 증가해도 응답 시간이 거의 변하지 않습니다.

반면 `/query` 요청은 `mutex`로 보호된 구간을 포함하고 있어 요청이 순차적으로 처리됩니다.
이로 인해 요청누적에 따라  대기 시간이 누적되며,
전체 응답 시간이 점점 증가하는 모습을 확인할 수 있습니다.

```


## 3:00 - 4:00 시연

```text
이제 Thunder Client로 실제 동작을 보여드리겠습니다.
먼저 /health로 서버가 살아있는지 확인합니다.
우측에 보시면 상태가 200으로 리턴 받는걸 볼수있습니다

다음으로 POST /query에 id 조건 SELECT를 보내면 rows에 실제 데이터가 나오는것을 확인할수있습니다.

그 다음 INSERT를 실행하고, 방금 넣은 id로 다시 SELECT해서 API 서버를 통해 데이터 추가와 조회가 모두 되는 것을 확인합니다.


마지막으로 이번 프로젝트를 통해 무조건적인 멀티쓰레드가 동시성 문제떄문에 무조건적인 속도 향상을 가져다 주지 않는 다는것을 배울수있는 시간이었습니다
이상으로 6팀 발표를 마치겠습니다  감사합니다
```

## 1. 과제 목표와 구현 결과

이번 주 과제는 **미니 DBMS - API 서버**입니다.

핵심 요구사항은 다음 네 가지입니다.

- 외부 클라이언트가 API로 DBMS 기능을 사용할 수 있어야 합니다.
- Thread Pool을 구성하고 SQL 요청을 병렬 처리해야 합니다.
- 이전 차수 SQL 처리기와 B+ Tree 인덱스를 그대로 활용해야 합니다.
- 구현 언어는 C입니다.

구현 결과:

| 과제 요구사항 | 구현 내용 | 확인 위치 |
|---|---|---|
| API 서버 구현 | C HTTP 서버, `/query`로 SQL 실행 | `src/api/api_server.c` |
| 외부 클라이언트 사용 | Thunder Client에서 SELECT/INSERT 실행 | `POST /query` |
| Thread Pool | worker thread + bounded queue | `src/concurrency/thread_pool.c` |
| 요청마다 worker 처리 | accept된 client 작업을 queue에 넣고 worker가 처리 | `thread_pool_submit()` |
| 기존 SQL 처리기 재사용 | Tokenizer, Parser, Optimizer, Executor 유지 | `src/sql/` |
| B+ Tree 인덱스 재사용 | `id -> row offset` 인덱스 기반 조회 | `src/storage/bptree.c` |
| 동시성 이슈 처리 | worker는 병렬, DB 엔진 실행 구간은 mutex 보호 | `src/api/db_api.c` |

발표 멘트:

```text
API 서버 자체는 새로 만들었지만, SQL을 해석하고 실행하는 내부 엔진은 지난주 코드를 재사용했습니다.
이번 주 핵심은 기존 엔진 앞에 HTTP API 계층과 Thread Pool 계층을 붙인 것입니다.
```

## 2. 전체 아키텍처

![전체 아키텍처](docs/assets/architecture.svg)

설명 순서:

- Thunder Client가 HTTP로 SQL을 보냅니다.
- `api_server.c`가 HTTP 요청을 읽고 `/query` 라우트로 보냅니다.
- Thread Pool의 worker가 요청을 처리합니다.
- `db_api.c`가 기존 SQL 엔진을 호출합니다.
- SQL 엔진은 `Tokenizer -> Parser -> Optimizer -> Executor` 흐름을 그대로 사용합니다.
- Storage 계층은 `users.tbl`과 `users.idx`를 사용합니다.
- 엔진 출력은 다시 `db_api.c`에서 `DbApiResult`로 정리됩니다.
- 최종 JSON 포맷과 HTTP 응답은 `api_server.c`가 담당합니다.

비판적으로 봐야 할 점:

- `db_api.c`가 JSON을 직접 만드는 것은 아닙니다. `db_api.c`는 `DbApiResult`를 반환하고, JSON 포맷은 `api_server.c`가 담당합니다.
- Thread Pool은 요청을 병렬로 받지만, 기존 DB 엔진 자체는 mutex로 보호됩니다.

## 3. 요청 처리 시퀀스

![요청 처리 시퀀스](docs/assets/request-sequence.svg)

`POST /query` 하나를 기준으로 보면 다음 순서입니다.

1. Thunder Client가 SQL body를 전송합니다.
2. API 서버가 연결을 accept하고 `ClientTask`를 queue에 넣습니다.
3. worker thread가 task를 꺼냅니다.
4. worker가 HTTP method, path, body를 읽습니다.
5. body에서 SQL 문자열을 추출합니다.
6. `db_api_execute_sql()`이 기존 SQL 엔진을 호출합니다.
7. DB 실행 구간은 mutex로 보호됩니다.
8. 엔진 출력은 `FILE*`에서 문자열로 캡처됩니다.
9. API 서버가 JSON body를 만들어 응답합니다.

추가 코멘트:

```text
HTTP 요청 처리와 JSON 응답 생성은 worker별로 병렬 처리됩니다.
다만 기존 엔진이 공유 파일과 B+ Tree 인덱스를 사용하므로, 실제 SQL 실행 구간만 mutex로 감쌌습니다.
```

## 4. Thread Pool과 동시성 설계

![Thread Pool과 동시성 설계](docs/assets/thread-pool.svg)

핵심 판단:

- 요청마다 새 thread를 만들지 않고, 미리 만든 worker thread를 재사용합니다.
- OS accept backlog는 `accept()` 되기 전 연결이 서버 OS 커널에서 기다리는 곳입니다.
- Thread Pool queue는 `accept()` 된 뒤 `ClientTask`가 worker를 기다리는 프로그램 내부 queue입니다.
- Thread Pool queue가 가득 차면 `thread_pool_submit()`이 빈칸이 생길 때까지 기다리고, 이 동안 accept loop는 다음 `accept()`를 호출하지 못합니다.
- 그래서 submit에서 못 넣은 요청이 OS backlog로 되돌아가는 것이 아니라, 그 이후 새 연결들이 아직 `accept()`되지 못해 OS backlog에 남습니다.
- 실제 요청 처리는 worker thread가 수행합니다.
- `/health`, `/metrics`는 DB lock 없이 응답합니다.
- `/query`만 기존 DB 엔진 접근 구간에서 mutex를 탑니다.

왜 mutex를 썼는가:

- 기존 SQL 엔진은 thread-safe하게 작성된 구조가 아닙니다.
- `users.tbl`과 `users.idx`를 여러 thread가 동시에 건드리면 파일 offset, B+ Tree 상태가 꼬일 수 있습니다.
- 그래서 API 요청 접수는 병렬로 처리하되, SQL 실행 구간은 정합성을 위해 직렬화했습니다.

포인트:

```text
완전한 병렬 DB 엔진은 아닙니다.
이번 구현은 API 서버와 요청 처리 구조는 병렬화했고,
기존 DB 엔진의 공유 상태는 mutex로 보호한 구조입니다.
```

## 5. 100회 요청 로그로 본 구조 차이

![100회 요청 로그 비교](docs/assets/load-test-trend.png)

로그 해석:

- 여기서 평균은 보조 지표이고, 핵심은 **완료 순서별 total time 추세**입니다.
- `GET /health` 100회는 모두 200으로 성공했고, DB 엔진을 거치지 않기 때문에 그래프가 거의 0초 근처에 붙어 있습니다.
- `POST /query` 100회도 모두 200으로 성공했지만, 뒤쪽 요청으로 갈수록 end-to-end total 시간이 점점 커집니다.
- 실제 로그 기준으로 `/query`는 첫 완료가 약 0.49초, 마지막 완료가 약 33.28초입니다.
- 이유는 worker가 요청을 병렬로 받아도 기존 SQL 엔진 실행 구간은 mutex로 보호되어 한 번에 하나씩만 들어갈 수 있기 때문입니다.
- 따라서 이 결과는 “Thread Pool은 요청을 병렬로 받지만, 기존 DB 엔진 접근은 정합성을 위해 직렬화되고 대기 시간이 누적된다”는 구조를 보여줍니다.
- 원본 로그는 `docs/load-tests/health-100.txt`, `docs/load-tests/query-100-times.txt`에 보관했습니다.
- 그래프는 `python -m pip install -r requirements-dev.txt` 후 `python tools/plot_load_tests.py`로 원본 로그에서 다시 만들 수 있습니다.

주의해서 점:

```text
이 그래프는 단일 SQL 1개의 순수 실행 시간이 아니라,
100회 요청을 보냈을 때 클라이언트가 관찰한 end-to-end total 시간입니다.
따라서 query 쪽 시간이 길어진 핵심 이유는 SQL 실행 자체뿐 아니라,
mutex로 보호된 기존 엔진 구간 앞에서 대기 시간이 누적되기 때문입니다.
```

## 6. 기존 SQL 엔진과 B+ Tree 재사용

![SQL 실행 분기](docs/assets/sql-flow.svg)

SELECT 분기:

- `WHERE id ...` 조건이면 B+ Tree 인덱스를 사용합니다.
- `WHERE name ...`, `WHERE email ...`, `WHERE age ...` 같은 non-id 조건은 선형 탐색합니다.

INSERT 분기:

- 들어온 값을 테이블 컬럼 순서에 맞게 정렬합니다.
- id가 없으면 자동 부여합니다.
- `users.tbl` 끝에 row를 append합니다.
- 새 id와 row offset을 B+ Tree에 추가합니다.

## 7. 데이터 저장 구조

![데이터 저장 구조](docs/assets/storage-index.svg)

핵심:

- row 데이터는 `users.tbl`에 저장됩니다.
- B+ Tree 인덱스는 `users.idx`에 저장됩니다.
- 인덱스 value는 row 자체가 아니라 `users.tbl` 안의 파일 offset입니다.
- `WHERE id = ?` 조회는 B+ Tree에서 offset을 찾고, 해당 위치로 `fseek`해서 row를 읽습니다.
- 그림의 offset 숫자는 이해를 돕기 위한 예시입니다.

발표 멘트:

```text
B+ Tree가 row 전체를 들고 있는 것이 아니라, row가 파일의 몇 번째 byte 위치에 있는지를 들고 있습니다.
그래서 id 조건 조회는 전체 파일을 스캔하지 않고 해당 offset으로 바로 이동할 수 있습니다.
```

## 8. 시연 순서

### 8.1 서버 실행

VS Code 터미널에서 프로젝트 폴더를 엽니다.

```powershell
cd "C:\Users\cedis\Downloads\idea(backjoon)\week8\mini_dbms_api_server"
```

빌드:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

서버 실행:

```powershell
.\mini_sql_server.exe --data-dir runtime_data --host 127.0.0.1 --port 8080 --threads 4
```

### 8.2 Thunder Client 요청

아래 요청들은 Thunder Client에서 그대로 복사해서 사용합니다.

주의: INSERT 시연을 여러 번 반복했다면 `900001`과 이메일의 숫자를 `900002`, `900003`처럼 바꿔 실행합니다.

1. 서버 상태 확인

```text
GET http://127.0.0.1:8080/health
```

기대 결과:

```json
{"ok":true,"status":"ready"}
```

2. B+ Tree 인덱스 조회

```text
POST http://127.0.0.1:8080/query
```

Body:

```sql
SELECT * FROM users WHERE id = 777777;
```

확인할 것:

- `rows`에 실제 사용자 데이터가 보입니다.
- `stats.usedIndex`가 `true`입니다.
- `stats.scannedRows`가 작게 나옵니다.

3. INSERT

```text
POST http://127.0.0.1:8080/query
```

Body:

```sql
INSERT INTO users (id, name, email, age) VALUES (900001, 'TEAM6_API_USER', 'team6-api-user-900001@example.com', 26);
```

확인할 것:

- `ok`가 `true`입니다.
- `result`에 `Inserted 1 row`가 보입니다.
- 실제 row는 `users.tbl`에 append되고, `users.idx`에는 `id -> row_offset`이 추가됩니다.

4. 방금 넣은 row를 id로 조회

```text
POST http://127.0.0.1:8080/query
```

Body:

```sql
SELECT * FROM users WHERE id = 900001;
```

확인할 것:

- `rows`에 `TEAM6_API_USER`가 보입니다.
- id 조건 조회이므로 `stats.usedIndex`가 `true`입니다.
- 방금 INSERT한 row도 B+ Tree 인덱스로 찾을 수 있습니다.

5. non-id 조건 조회 비교

```text
POST http://127.0.0.1:8080/query
```

Body:

```sql
SELECT * FROM users WHERE email = 'team6-api-user-900001@example.com';
```

확인할 것:

- non-id 조건이므로 `usedIndex`는 `false`입니다.
- 그래도 API 서버를 통해 DBMS 조회 기능은 정상 동작합니다.

6. 요청 통계 확인

```text
GET http://127.0.0.1:8080/metrics
```

## 9. API 응답 예시

```json
{
  "ok": true,
  "result": "id,name,email,age\n777777,CHOIHYUNJIN,guswls1478@gmail.com,24\n",
  "columns": ["id", "name", "email", "age"],
  "rows": [
    {
      "id": "777777",
      "name": "CHOIHYUNJIN",
      "email": "guswls1478@gmail.com",
      "age": "24"
    }
  ],
  "rowCount": 1,
  "stats": {
    "usedIndex": true,
    "scannedRows": 1,
    "matchedRows": 1,
    "elapsedMs": 1.223
  }
}
```

응답에서 볼 포인트:

- `result`: 기존 엔진의 CSV 출력 원문입니다.
- `columns`, `rows`, `rowCount`: Thunder Client에서 보기 좋게 추가한 JSON 구조입니다.
- `stats`: 인덱스 사용 여부와 조회 row 수를 보여줍니다.

## 10. MacBook / Docker 실행

MacBook에서는 Docker 실행을 권장합니다. Windows와 macOS의 C 컴파일러 차이를 줄이고 같은 Linux 환경에서 시연할 수 있습니다.

```bash
docker build -t mini-sql-bptree .
docker run --rm -it -p 8080:8080 -v "$(pwd)/runtime_data:/app/runtime_data" mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080 --threads 4
```

Thunder Client에서는 동일하게 호출합니다.

```text
http://127.0.0.1:8080/query
```

## 11. 파일 구조

```text
src/
├── api/
│   ├── api_server.c      # HTTP 서버, routing, JSON 응답
│   └── db_api.c          # API 서버와 기존 SQL 엔진 연결
├── apps/
│   ├── server_main.c     # API 서버 실행 진입점
│   ├── main.c            # CLI SQL 실행
│   ├── test_main.c       # 단위 테스트 실행
│   ├── benchmark_main.c
│   └── seed_main.c
├── concurrency/
│   └── thread_pool.c     # worker thread, queue, condition variable
├── sql/
│   ├── tokenizer.c
│   ├── parser.c
│   ├── optimizer.c
│   ├── executor.c
│   └── engine.c
├── storage/
│   ├── database.c        # SELECT/INSERT, table file, index 연결
│   ├── bptree.c          # B+ Tree index
│   └── storage.c
└── common/
    ├── util.c
    └── trace.c
```

자세한 함수별 설명과 줄 번호는 `CODE_REVIEW_GUIDE.md`에 정리되어 있습니다.

## 12. 테스트

Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\mini_sql_tests.exe
.\mini_sql_api_tests.exe
```

Docker:

```bash
docker build -t mini-sql-bptree .
docker run --rm mini-sql-bptree ./mini_sql_tests
docker run --rm mini-sql-bptree ./mini_sql_api_tests
```

검증 내용:

- SQL 처리기 기본 동작
- B+ Tree insert/search/range
- API용 DB adapter 결과 캡처
- Thread Pool queue 처리
- stale index 감지 후 재생성
- `POST /query` 응답 JSON 구조

##  마무리 문장

```text
이번 구현은 새 DBMS를 다시 만든 것이 아니라,
지난주 SQL 처리기와 B+ Tree 인덱스를 내부 엔진으로 재사용하고,
그 앞에 C 기반 HTTP API 서버와 Thread Pool을 붙여 외부 클라이언트가 사용할 수 있게 만든 것입니다.
동시성은 Thread Pool로 요청을 병렬 처리하되,
기존 엔진의 공유 파일과 인덱스 접근은 mutex로 보호해 정합성을 우선했습니다.
```
