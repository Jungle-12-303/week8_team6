# mini_sql with B+ Tree Index

기존 `mini_sql` SQL 처리기를 유지한 채, `id` 컬럼에 대한 디스크 persist B+ 트리 인덱스(`.idx`)를 붙인 과제 제출용 버전입니다.

## 구현 범위

- SQL 파이프라인 유지
  - `Tokenizer -> Parser -> Optimizer -> Executor`
- Week8 API 서버 추가
  - C 기반 HTTP 서버
  - Thread Pool 기반 요청 처리
  - 기존 SQL 처리기와 B+ 트리 인덱스 재사용
  - VS Code + Thunder Client 테스트 가능
- 지원 SQL
  - `INSERT INTO table VALUES (...)`
  - `INSERT INTO table (col1, col2, ...) VALUES (...)`
  - `SELECT * FROM table`
  - `SELECT col1, col2 FROM table`
  - `SELECT ... FROM table WHERE column <op> literal`
- 지원 비교 연산자
  - `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`
- 인덱스 적용 대상
  - `WHERE id <op> ?` 전체
  - `id = ?`, `id != ?`, `id < ?`, `id <= ?`, `id > ?`, `id >= ?`
- 선형 탐색 대상
  - `WHERE email = ?` 같은 non-id 조건

## 핵심 동작

### 1. B+ 트리 인덱스

- `id` 를 key, `.tbl` 파일의 row 시작 offset 을 value 로 저장합니다.
- B+ 트리 노드와 헤더는 `users.idx` 같은 바이너리 인덱스 파일에 페이지 단위로 저장됩니다.
- equality 는 단건 탐색, range 조건은 leaf linked list 를 따라 순차 방문합니다.
- `!=` 는 전체 leaf 를 순회하면서 해당 key 만 제외합니다.

### 2. 자동 ID 부여

- `id` 컬럼을 빼고 INSERT 하면 현재 최대 id + 1 을 자동 부여합니다.
- 이미 `id` 를 직접 넣는 INSERT 도 허용하지만, 중복 key 는 거절합니다.

### 3. 컬럼 순서 재매핑

- INSERT 시 컬럼 목록을 주면 스키마 헤더 기준으로 값을 재정렬합니다.
- 예:

```sql
INSERT INTO users (email, name) VALUES ('carol@example.com', 'Carol');
```

저장 결과:

```text
3,Carol,carol@example.com
```

### 4. API 서버와 Thread Pool

Week8 과제용 API 서버는 기존 SQL 엔진을 다시 만들지 않고 감싸는 방식입니다.

```text
Thunder Client
    -> HTTP request
    -> api_server.c
    -> ThreadPool worker
    -> DbApi
    -> 기존 SqlEngine
    -> Tokenizer / Parser / Optimizer / Executor
    -> storage + B+ tree index
```

핵심 설계:

- `api_server.c`
  - HTTP 요청을 읽고 `/health`, `/query`, `/metrics` 라우팅을 처리합니다.
- `thread_pool.c`
  - 요청마다 새 스레드를 만들지 않고, 미리 만든 worker thread가 큐에서 작업을 가져갑니다.
- `db_api.c`
  - 기존 `SqlEngine`을 API 서버에서 쓰기 좋은 형태로 감쌉니다.
  - 기존 엔진은 thread-safe하게 작성된 코드가 아니므로 SQL 실행 구간은 mutex로 보호합니다.
  - 즉, 서버는 여러 요청을 동시에 받을 수 있고, 실제 DB 파일/인덱스 접근은 안전하게 한 번에 하나씩 처리합니다.

발표 때 한 줄로 말하면:

> "외부에서는 HTTP API로 SQL을 보내고, 서버 내부에서는 스레드풀이 요청을 받아 기존 SQL 엔진에 연결합니다. 단, 기존 DB 엔진의 파일과 B+ 트리 동시 접근은 mutex로 보호했습니다."

## 디스크/메모리 사용 방식

`DBMS 구현인데 전부 RAM에 올려서 동작하면 안 되는 것 아니냐`는 지적에 대한 답은 명확합니다.

- 이 구현은 전체 row를 메모리에 적재하지 않습니다.
- B+ 트리 인덱스는 `users.idx` 바이너리 파일에 저장되며, 조회/삽입 때 필요한 노드를 디스크에서 읽고 씁니다.
- 실제 row 데이터는 `.tbl` 파일에 남아 있고, 조회 시 필요한 row만 `fseek + fgets`로 읽습니다.
- non-id 조건은 파일을 한 줄씩 선형 탐색합니다.
- 인덱스 파일이 이미 있으면 재시작 후 그대로 사용하고, 비어 있거나 없을 때만 `.tbl`을 한 번 읽어 복구합니다.

즉 현재 구조는:

- `in-memory table engine` 이 아니라
- `binary on-disk B+ tree index + file-backed table` 구조입니다.

즉 "row도 인덱스도 전부 메모리에 올려놓는 구현"은 아닙니다. 다만 row 파일은 아직 CSV 텍스트이므로, 비교 대상 레포처럼 데이터 파일까지 고정 길이 바이너리 레코드로 저장하는 단계까지는 가지 않았습니다.

## 실행 시간 로그

모든 SQL 실행은 `logs/query_timing.log` 에 기록됩니다.

로그 항목:

- `mode`
  - `FILE`: SQL 파일 실행
  - `API`: 엔진 직접 호출
- `path`
  - `insert`
  - `indexed`
  - `full_scan`
  - `unknown`
- `status`
  - `ok`
  - `error`
- `time_ms`
- `scanned_rows`
- `matched_rows`
- `sql`

예시:

```text
2026-04-15 21:10:00 | mode=FILE | path=indexed | status=ok | time_ms=0.123 | scanned_rows=1 | matched_rows=1 | sql=SELECT name FROM users WHERE id = 2;
```

## 파일 구조

```text
mini_sql/
├── build.ps1
├── Makefile
├── data/
│   └── users.tbl
├── include/
│   ├── ast.h
│   ├── bptree.h
│   ├── database.h
│   ├── api_server.h
│   ├── db_api.h
│   ├── engine.h
│   ├── executor.h
│   ├── optimizer.h
│   ├── parser.h
│   ├── storage.h
│   ├── tokenizer.h
│   ├── thread_pool.h
│   └── util.h
├── sql/
│   ├── insert_user.sql
│   ├── select_all.sql
│   ├── select_duplicate_columns.sql
│   ├── select_names.sql
│   └── select_where.sql
├── tests/
│   └── api_tests.c
└── src/
    ├── api_server.c
    ├── ast.c
    ├── benchmark_main.c
    ├── bptree.c
    ├── database.c
    ├── db_api.c
    ├── engine.c
    ├── executor.c
    ├── main.c
    ├── optimizer.c
    ├── parser.c
    ├── storage.c
    ├── server_main.c
    ├── test_main.c
    ├── thread_pool.c
    ├── tokenizer.c
    └── util.c
```

## 빌드

### Windows / PowerShell

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

생성 파일:

- `mini_sql.exe`
- `mini_sql_tests.exe`
- `mini_sql_server.exe`
- `mini_sql_api_tests.exe`
- `mini_sql_benchmark.exe`
- `mini_sql_seed.exe`

### Linux/macOS

```bash
make
```

### Docker

```bash
docker build -t mini-sql-bptree .
```

기본 실행은 REPL 이며, Docker named volume 으로 데이터가 유지됩니다.

```bash
docker run --rm -it mini-sql-bptree
```

원하는 바이너리를 직접 실행할 수도 있습니다.

```bash
docker run --rm mini-sql-bptree ./mini_sql sql/select_where.sql
docker run --rm mini-sql-bptree ./mini_sql_tests
docker run --rm -p 8080:8080 mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080
docker run --rm mini-sql-bptree ./mini_sql_benchmark --rows 1000000 --repetitions 1
```

기본 `make docker-repl` / `make docker-run` 은 `mini-sql-bptree-data` named volume 을 사용하므로 컨테이너를 껐다 켜도 데이터가 남습니다.

```bash
docker volume ls
```

Makefile 타깃도 제공합니다.

```bash
make docker-build
make docker-test
make docker-api-test
make docker-server
make docker-run
make docker-repl
make docker-benchmark
make docker-seed-million
```

### MacBook 시연 권장 방식

맥북에서도 같은 결과를 보여줘야 한다면 Docker 실행을 권장합니다.

이유:

- C 컴파일러/라이브러리 차이를 줄일 수 있습니다.
- Windows, macOS, Linux에서 같은 Debian 컨테이너 환경으로 실행됩니다.
- Thunder Client는 호스트의 `localhost:8080`으로 그대로 호출하면 됩니다.

MacBook 터미널:

```bash
docker build -t mini-sql-bptree .
docker run --rm -it -p 8080:8080 mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080 --threads 4
```

로컬 `runtime_data`의 100만 건 데이터를 Docker 컨테이너에 연결해서 시연하려면:

```bash
docker run --rm -it -p 8080:8080 -v "$(pwd)/runtime_data:/app/runtime_data" mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080 --threads 4
```

또는 Makefile:

```bash
make docker-server
```

Thunder Client에서는 Windows와 동일하게 호출합니다.

```text
GET  http://127.0.0.1:8080/health
POST http://127.0.0.1:8080/query
GET  http://127.0.0.1:8080/metrics
```

## 실행

```powershell
.\mini_sql.exe
.\mini_sql.exe --repl
.\mini_sql.exe sql\select_all.sql
.\mini_sql.exe sql\select_where.sql
.\mini_sql.exe sql\insert_user.sql
```

### API 서버 실행

VS Code 터미널에서:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
.\mini_sql_server.exe --data-dir data --host 127.0.0.1 --port 8080 --threads 4
```

100만 건 데이터로 시연하려면:

```powershell
.\mini_sql_server.exe --data-dir runtime_data --host 127.0.0.1 --port 8080 --threads 4
```

서버가 켜지면 아래처럼 표시됩니다.

```text
Mini SQL API server listening on http://127.0.0.1:8080
Try: GET /health, POST /query, GET /metrics
```

Thunder Client 테스트:

| 목적 | Method | URL | Body |
|---|---|---|---|
| 서버 상태 확인 | `GET` | `http://127.0.0.1:8080/health` | 없음 |
| SQL 실행 | `POST` | `http://127.0.0.1:8080/query` | raw text SQL |
| 서버 지표 확인 | `GET` | `http://127.0.0.1:8080/metrics` | 없음 |

`POST /query` Body 예시 1, raw SQL:

```sql
SELECT name, email, age FROM users WHERE id >= 2;
```

`POST /query` Body 예시 2, JSON:

```json
{"sql":"SELECT name FROM users WHERE id = 2;"}
```

응답 예시:

```json
{
  "ok": true,
  "result": "name\nBob\n",
  "columns": ["name"],
  "rows": [
    {
      "name": "Bob"
    }
  ],
  "rowCount": 1,
  "stats": {
    "usedIndex": true,
    "scannedRows": 1,
    "matchedRows": 1,
    "elapsedMs": 0.123
  }
}
```

REPL 예시:

```text
sql> SELECT name, email, age FROM users WHERE id >= 2;
name,email,age
Bob,bob@example.com,25
[indexed] time_ms=0.123 scanned_rows=1 matched_rows=1
sql> INSERT INTO users (email, name, age) VALUES ('carol@example.com', 'Carol', 30);
Inserted 1 row into users
[insert] time_ms=0.456
sql> EXIT;
```

예시:

```sql
SELECT name, email, age FROM users WHERE id >= 2;
```

출력:

```text
name,email,age
Bob,bob@example.com,25
```

## 테스트

```powershell
.\mini_sql_tests.exe
.\mini_sql_api_tests.exe
```

검증 항목:

- B+ 트리 insert/search/range visit
- 자동 ID 부여
- `INSERT (email, name)` 컬럼 순서 재매핑
- `id` 조건의 `=`, `<`, `>=`, `!=` 인덱스 경로
- non-id 조건 선형 탐색 유지
- API용 `DbApi` 결과 캡처
- Thread Pool 큐 작업 실행

## 벤치마크

```powershell
.\mini_sql_benchmark.exe --rows 1000000 --repetitions 1
```

벤치마크는 다음 SQL 경로를 그대로 사용합니다.

- 1,000,000회:
  - 랜덤 `name`, 랜덤 `email`, 랜덤 `age` 생성 후 `INSERT INTO users (name, email, age) VALUES (...)`
- 인덱스 비교:
  - 랜덤하게 뽑은 `id` 에 대한 `SELECT ... WHERE id = ?`
  - 랜덤 시작점에 대한 `SELECT ... WHERE id >= ?`
- 선형 탐색 비교:
  - 삽입 중 샘플링한 랜덤 `email` 에 대한 `SELECT ... WHERE email = ?`

주의:

- 일반 실행기와 REPL의 INSERT 결과는 `.tbl` 파일에 저장되므로 프로그램을 껐다 켜도 유지됩니다.
- 시작할 때 `.idx` 인덱스 파일이 있으면 그대로 사용하고, 없거나 비어 있을 때만 `.tbl`을 읽어 재구성합니다.
- 단, `mini_sql_benchmark` 는 매 실행마다 `benchmark_data/users.tbl` 을 새로 만들어 다시 측정합니다.
- 도커 REPL/실행은 기본적으로 named volume 을 쓰도록 Makefile 에 맞춰뒀습니다.

## 100만 건 시드 데이터

테스트용으로 1,000,000건의 랜덤 사용자 데이터를 SQL 엔진으로 실제 INSERT 하려면:

```bash
make docker-seed-million
```

특징:

- 컬럼: `id,name,email,age`
- `id` 는 자동 증가
- `name`, `email`, `age` 는 랜덤 생성
- 단 하나의 고정 테스트 케이스를 반드시 포함
  - `name = 'CHOIHYUNJIN'`
  - `email = 'guswls1478@gmail.com'`
  - `age = 24`

생성 후 같은 데이터는 아래처럼 다시 조회할 수 있습니다.

```bash
make docker-repl
```

```sql
SELECT * FROM users WHERE email = 'guswls1478@gmail.com';
SELECT * FROM users WHERE id = 777777;
```

### 실제 실행 결과

이 환경에서 직접 실행한 결과:

```text
Rows inserted: 1000000
Insert elapsed: 374867.00 ms
id equality avg: 0.0000 ms
  used_index=1 scanned_rows=1 matched_rows=1
id range avg: 0.0000 ms
  used_index=1 scanned_rows=10 matched_rows=10
email equality avg: 674.0000 ms
  used_index=0 scanned_rows=1000000 matched_rows=1
```

핵심 차이:

- `id` 기반 조회는 B+ 트리가 바로 offset 을 찾아서 필요한 row만 읽습니다.
- `email` 기반 조회는 파일 전체를 끝까지 선형 스캔합니다.

## 팀4 저장소와 비교

비교 대상:

- `Jungle-12-303/week7-team4-sql-processor-Bp-tree-index`

### 기능/구조 비교

| 항목 | 팀4 저장소 | 현재 mini_sql |
|---|---|---|
| 저장 포맷 | `meta + binary .dat` | 바이너리 `.idx` + 텍스트 `.tbl` |
| SQL 범위 | `schema.table`, `BETWEEN`, REPL 포함 | 최소 SQL + 컬럼 매핑 INSERT |
| 인덱스 조건 | `id =, >, >=, <, <=, BETWEEN` | `id =, !=, <, <=, >, >=` |
| `id !=` 인덱스 | 미사용 | 사용 |
| INSERT 컬럼 목록 | 없음 | 있음 |
| 자동 ID | 있음 | 있음 |
| 실행 시간 로그 | 있음 | 있음 |
| 대량 벤치마크 러너 | 수동/스크립트 중심 | 실행 파일로 포함 |
| 테스트 | B+ 트리 + BETWEEN 파싱 | B+ 트리 + INSERT 매핑 + 인덱스 경로 |

### 탑다운 최소구현 과제 관점 비교

| 관점 | 팀4 저장소 | 현재 mini_sql |
|---|---|---|
| 과제 필수조건만 빠르게 맞추기 | 범위가 조금 큼 | 더 적합 |
| 기존 SQL 처리기에 최소 침투 | 보통 | 강함 |
| 발표 시 설명 난이도 | 다소 높음 | 낮음 |
| 구현량 대비 결과 데모 | 큼 | 효율적 |
| "WHERE id 조건은 인덱스" 메시지 전달 | 명확 | 더 직접적 |

### 공부 관점 비교

| 관점 | 팀4 저장소 | 현재 mini_sql |
|---|---|---|
| 실제 DBMS 느낌 | 더 강함 | 충분함 |
| 저장소/메타 분리 학습 | 좋음 | 보통 |
| 바이너리 row 해석 학습 | 좋음 | 없음 |
| 바이너리 index/page 해석 학습 | 좋음 | 충분함 |
| B+ 트리 범위 스캔 학습 | 좋음 | 충분함 |
| SQL 파이프라인 이해 | 충분함 | 충분함 |
| MVP 설계 감각 | 보통 | 좋음 |

정리하면:

- 팀4 저장소는 "더 넓게 구현한 미니 DBMS" 쪽입니다.
- 현재 mini_sql은 "기존 코드에 최소 수정으로 과제 필수조건을 빠르게 만족시키는 탑다운 MVP" 쪽입니다.
- 발표에서 방어하기 쉬운 쪽은 현재 mini_sql이고, 공부용으로 파고들기 좋은 쪽은 팀4 저장소입니다.

## 발표 포인트

- 기존 SQL 처리기는 그대로 두고 executor 뒤의 storage 접근만 인덱스 친화적으로 바꿨습니다.
- B+ 트리 value 를 row pointer 대신 file offset 으로 둬서 전체 row를 RAM에 올리지 않습니다.
- `WHERE id <op> ?` 전체를 인덱스로 처리해 equality-only 최적화보다 범위가 넓습니다.
- `INSERT (email, name)` 같은 컬럼 순서 재매핑을 지원합니다.
- 실행 시간 로그를 남겨 인덱스 사용 여부를 발표에서 바로 보여줄 수 있습니다.
