# Week8 Mini DBMS - API Server

외부 클라이언트가 HTTP API로 SQL을 보내면, C 기반 API 서버가 Thread Pool로 요청을 받아 기존 SQL 처리기와 B+ Tree 인덱스를 사용해 결과를 JSON으로 반환하는 미니 DBMS 서버입니다.

발표 한 줄 요약:

> Thunder Client에서 SQL을 HTTP로 보내면, 서버는 worker thread에 요청을 배정하고, 내부에서는 기존 SQL 엔진과 B+ Tree 인덱스를 재사용해 SELECT/INSERT 결과를 JSON으로 반환합니다.

## 발표 핵심

- 구현 언어는 C입니다.
- API 서버는 `GET /health`, `POST /query`, `GET /metrics`를 제공합니다.
- 요청 처리는 Thread Pool 기반입니다.
- 기존 SQL 처리기와 B+ Tree 인덱스를 새 API 서버 안에 연결했습니다.
- 기존 DB 엔진은 공유 파일과 인덱스를 다루므로, DB 실행 구간은 mutex로 보호했습니다.
- Thunder Client에서 결과를 보기 쉽게 CSV 원문과 JSON `columns`, `rows`, `stats`를 함께 반환합니다.

## 요구사항 대응표

| 과제 요구사항 | 구현 내용 | 확인 위치 |
|---|---|---|
| 미니 DBMS - API 서버 구현 | C HTTP 서버 구현, `/query`로 SQL 실행 | `src/api/api_server.c` |
| 외부 클라이언트에서 DBMS 기능 사용 | Thunder Client에서 HTTP 요청으로 SELECT/INSERT 실행 | `POST /query` |
| Thread Pool 구성 | 고정 worker thread + bounded queue | `src/concurrency/thread_pool.c` |
| 요청마다 스레드 할당 | accept된 client 작업을 queue에 넣고 worker가 처리 | `thread_pool_submit()` |
| SQL 요청 처리 | body에서 SQL 추출 후 기존 엔진 호출 | `handle_query()` |
| 이전 SQL 처리기 재사용 | Tokenizer, Parser, Optimizer, Executor 유지 | `src/sql/` |
| B+ Tree 인덱스 재사용 | `id -> row offset` 인덱스 기반 조회 | `src/storage/bptree.c`, `src/storage/database.c` |
| 멀티 스레드 동시성 이슈 | worker는 병렬, 공유 DB 엔진 실행 구간은 mutex 보호 | `src/api/db_api.c` |
| API 서버 아키텍처 | API 계층, Thread Pool, DB Adapter, SQL Engine, Storage 분리 | `src/` 폴더 구조 |

## 전체 아키텍처

![전체 아키텍처](docs/assets/architecture.svg)

## 요청 처리 시퀀스

![요청 처리 시퀀스](docs/assets/request-sequence.svg)

## Thread Pool과 동시성 설계

![Thread Pool과 동시성 설계](docs/assets/thread-pool.svg)

동시성 판단:

- 여러 client 연결은 worker thread들이 병렬로 받습니다.
- HTTP parsing, routing, response formatting은 worker별로 독립 실행됩니다.
- 기존 SQL 엔진은 파일과 B+ Tree 인덱스를 공유하므로 SQL 실행 구간은 mutex로 한 번에 하나씩 보호합니다.
- 이 선택은 성능보다 데이터 정합성을 우선한 설계입니다.
- 개선 방향은 `SELECT`는 read lock, `INSERT`는 write lock으로 분리하는 것입니다.

## SQL 실행 분기

![SQL 실행 분기](docs/assets/sql-flow.svg)

인덱스 사용 기준:

- `WHERE id = 777777`
- `WHERE id >= 999990`
- `WHERE id < 10`
- `WHERE id != 3`

선형 탐색 기준:

- `WHERE name = 'CHOIHYUNJIN'`
- `WHERE email = 'guswls1478@gmail.com'`
- `WHERE age >= 20`
- `WHERE`가 없는 전체 조회

## 데이터 저장 구조

![데이터 저장 구조](docs/assets/storage-index.svg)

핵심:

- row 데이터는 `.tbl` 파일에 저장됩니다.
- B+ Tree는 `.idx` 파일에 저장됩니다.
- 인덱스 value는 row 자체가 아니라 `.tbl` 안의 파일 offset입니다.
- 따라서 `WHERE id = ?` 조회는 전체 파일을 읽지 않고 offset 위치로 바로 이동합니다.

## API 명세

| Method | URL | 역할 |
|---|---|---|
| `GET` | `/health` | 서버 준비 상태 확인 |
| `POST` | `/query` | SQL 실행 |
| `GET` | `/metrics` | 요청 처리 통계 확인 |

`POST /query`는 두 가지 body를 지원합니다.

Raw SQL:

```sql
SELECT * FROM users WHERE id = 777777;
```

JSON:

```json
{"sql":"SELECT * FROM users WHERE id = 777777;"}
```

응답 예시:

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

## 시연 순서

### 1. VS Code에서 서버 실행

프로젝트 폴더:

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

### 2. Thunder Client 요청

상태 확인:

```text
GET http://127.0.0.1:8080/health
```

인덱스 조회:

```text
POST http://127.0.0.1:8080/query
```

Body:

```sql
SELECT * FROM users WHERE id = 777777;
```

INSERT:

```sql
INSERT INTO users (name, email, age) VALUES ('TEAM6_API_USER', 'team6-api-user@example.com', 26);
```

non-id 조건 조회:

```sql
SELECT * FROM users WHERE email = 'team6-api-user@example.com';
```

통계 확인:

```text
GET http://127.0.0.1:8080/metrics
```

## MacBook / Docker 실행

MacBook에서는 Docker 실행을 권장합니다. Windows와 macOS의 C 컴파일러 차이를 줄이고 같은 Linux 환경에서 시연할 수 있습니다.

```bash
docker build -t mini-sql-bptree .
docker run --rm -it -p 8080:8080 -v "$(pwd)/runtime_data:/app/runtime_data" mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080 --threads 4
```

Thunder Client에서는 동일하게 호출합니다.

```text
http://127.0.0.1:8080/query
```

## 파일 구조

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

## 테스트

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

## 차별점

- Thunder Client에서 바로 보기 좋은 `columns`, `rows`, `rowCount` JSON 응답을 추가했습니다.
- `/metrics`로 총 요청 수, query 요청 수, 성공/실패 응답 수를 확인할 수 있습니다.
- `.tbl`과 `.idx`가 어긋났을 때 row 수와 수정 시간을 비교해 인덱스를 재생성합니다.
- Windows는 `build.ps1`, macOS/Linux는 Docker로 같은 시연 흐름을 제공합니다.

## 발표 마무리 문장

```text
이번 구현의 핵심은 새 DBMS를 다시 만든 것이 아니라,
지난주 SQL 처리기와 B+ Tree 인덱스를 내부 엔진으로 재사용하고,
그 앞에 C 기반 HTTP API 서버와 Thread Pool을 붙여 외부 클라이언트가 사용할 수 있게 만든 것입니다.
동시성은 Thread Pool로 요청을 병렬 처리하되,
기존 엔진의 공유 파일과 인덱스 접근은 mutex로 보호해 정합성을 우선했습니다.
```
