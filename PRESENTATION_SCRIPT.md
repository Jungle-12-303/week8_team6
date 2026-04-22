# 4분 발표 대본

## 0:00 - 0:30 과제 목표

```text
안녕하세요 발표를 맡게된 6팀의 최현진입니다.
이번 주 과제는 미니 DBMS를 외부 클라이언트가 사용할 수 있게 API 서버로 올리는 것이었습니다.
저희는 지난주에 만든 SQL 처리기와 B+ Tree 인덱스를 그대로 내부 엔진으로 사용했고,
그 앞에 C 기반 HTTP API 서버와 Thread Pool을 붙였습니다.

요구사항 기준으로 보면 POST /query로 SQL을 실행할 수 있고,
요청은 Thread Pool worker가 처리하며,
기존 엔진의 공유 파일 접근은 mutex로 보호했습니다.
```

## 0:30 - 1:10 전체 아키텍처

```text
전체 흐름은 vscode의 Thunder Client가 HTTP로 SQL을 보내면 api_server가 요청을 받고,
Thread Pool worker가 실제 요청 처리를 담당합니다.

worker는 db_api.c를 통해 기존 SQL 엔진을 호출합니다.
엔진 결과를 DbApiResult로 정리하고, 최종 JSON 응답은 api_server가 만듭니다.
```

## 1:10 - 1:50 요청 1개 처리 흐름

```text
POST /query 하나를 보면, 서버는 먼저 accept로 연결을 받고 ClientTask 즉 일할거리를 Thread Pool queue에 넣습니다.
worker가 그 task를 꺼내 HTTP body에서 SQL 문자열을 읽습니다.
그 다음 db_api_execute_sql()을 호출하고, 내부에서는 기존 SQL 엔진이 실행됩니다.

엔진 출력은 기존처럼 CSV 문자열로 나오지만, API 응답에서는 columns를 추가해서 Thunder Client에서 보기 좋게 만들었습니다.
```

## 1:50 - 2:30 Thread Pool과 동시성

```text
요청마다 스레드를 새로 만들면 요청이 몰릴 때 스레드가 계속 늘어날 수 있습니다.
그래서 서버 시작 시 worker thread 4개를 미리 만들고 재사용합니다.
제 PC는 논리 프로세서가 20개지만, 이 서버는 기존 DB 엔진을 mutex로 보호하기 때문에 query 실행 자체는 병렬화되지 않습니다.
그래서 worker를 20개로 늘려도 SQL 처리량이 20배 늘지 않습니다.

추가로
여기서 OS accept backlog와 Thread Pool queue는 다른 대기 장소입니다.
OS backlog는 accept되기 전 커널 대기열이고,
Thread Pool queue는 accept된 뒤 worker를 기다리는 프로그램 내부 대기열입니다.

queue가 가득 차면 submit이 backlog로 되돌리는 것이 아니라,
accept loop가 thread_pool_submit 안에서 멈춥니다.
그래서 이후 새 연결들이 아직 accept되지 못해 OS backlog에 남습니다.
```

## 2:30 - 3:00 부하 로그와 mutex

```text
이 그래프는 각 요청을 100번 하였을떄의 서버의 평균 소요시간을 나타낸 그래프입니다
서버와 연결상태를 확인하는 /health요청은 DB를 타지 않아서 100번 요청해도 거의 0초 근처에 붙어 있습니다.

반면 /query 요청은 worker가 병렬로 받아도 기존 SQL 엔진 구간은 mutex로 한 번에 하나씩만 들어갑니다.
그래서 뒤쪽 요청으로 갈수록 mutex 앞 대기 시간이 누적되어 total 시간이 증가합니다.
즉 이번 구현은 API 요청 처리는 병렬화했지만, 기존 DB 엔진의 파일과 인덱스가 꼬이지 않도록 mutex로 보호했습니다.
```


## 3:00 - 4:00 시연

```text
이제 Thunder Client로 실제 동작을 보여드리겠습니다.
먼저 /health로 서버가 살아있는지 확인합니다.
우측에 보시면 상태가 200으로 리턴 받는걸 볼수있습니다

다음으로 POST /query에 id 조건 SELECT를 보내면 rows에 실제 데이터가 나오는것을 확인할수있습니다.

그 다음 INSERT를 실행하고, 방금 넣은 id로 다시 SELECT해서 API 서버를 통해 데이터 추가와 조회가 모두 되는 것을 확인합니다.
마지막으로 /metrics를 호출해서 서버가 처리한 요청 통계를 확인하겠습니다.
```
