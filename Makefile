CC = cc
CFLAGS = -std=c11 -Wall -Wextra -pedantic -Iinclude
THREAD_LIBS = -pthread

COMMON_SRCS = \
	src/engine.c \
	src/util.c \
	src/ast.c \
	src/tokenizer.c \
	src/parser.c \
	src/optimizer.c \
	src/storage.c \
	src/executor.c \
	src/database.c \
	src/bptree.c

TARGET = mini_sql
TEST_TARGET = mini_sql_tests
API_SERVER_TARGET = mini_sql_server
API_TEST_TARGET = mini_sql_api_tests
BENCH_TARGET = mini_sql_benchmark
SEED_TARGET = mini_sql_seed
DOCKER_DATA_VOLUME = mini-sql-bptree-data
DOCKER_LOG_VOLUME = mini-sql-bptree-logs

TARGET_SRCS = src/main.c $(COMMON_SRCS)
TEST_SRCS = src/test_main.c $(COMMON_SRCS)
API_SERVER_SRCS = src/server_main.c src/api_server.c src/thread_pool.c src/db_api.c $(COMMON_SRCS)
API_TEST_SRCS = tests/api_tests.c src/thread_pool.c src/db_api.c $(COMMON_SRCS)
BENCH_SRCS = src/benchmark_main.c $(COMMON_SRCS)
SEED_SRCS = src/seed_main.c $(COMMON_SRCS)

TARGET_OBJS = $(TARGET_SRCS:.c=.o)
TEST_OBJS = $(TEST_SRCS:.c=.o)
API_SERVER_OBJS = $(API_SERVER_SRCS:.c=.o)
API_TEST_OBJS = $(API_TEST_SRCS:.c=.o)
BENCH_OBJS = $(BENCH_SRCS:.c=.o)
SEED_OBJS = $(SEED_SRCS:.c=.o)

.PHONY: all clean docker-build docker-test docker-api-test docker-server docker-run docker-repl docker-benchmark docker-seed-million

all: $(TARGET) $(TEST_TARGET) $(API_SERVER_TARGET) $(API_TEST_TARGET) $(BENCH_TARGET) $(SEED_TARGET)

$(TARGET): $(TARGET_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TARGET_OBJS)

$(TEST_TARGET): $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS)

$(API_SERVER_TARGET): $(API_SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(API_SERVER_OBJS) $(THREAD_LIBS)

$(API_TEST_TARGET): $(API_TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $(API_TEST_OBJS) $(THREAD_LIBS)

$(BENCH_TARGET): $(BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $(BENCH_OBJS)

$(SEED_TARGET): $(SEED_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SEED_OBJS)

clean:
	rm -f $(TARGET) $(TEST_TARGET) $(API_SERVER_TARGET) $(API_TEST_TARGET) $(BENCH_TARGET) $(SEED_TARGET) src/*.o tests/*.o

docker-build:
	docker build -t mini-sql-bptree .

docker-test: docker-build
	docker run --rm -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql_tests

docker-api-test: docker-build
	docker run --rm -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql_api_tests

docker-server: docker-build
	docker run --rm -it -p 8080:8080 -v $(DOCKER_DATA_VOLUME):/app/runtime_data -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql_server --data-dir /app/runtime_data --host 0.0.0.0 --port 8080 --threads 4

docker-run: docker-build
	docker run --rm -v $(DOCKER_DATA_VOLUME):/app/runtime_data -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql --data-dir /app/runtime_data sql/select_where.sql

docker-repl: docker-build
	docker run --rm -it -v $(DOCKER_DATA_VOLUME):/app/runtime_data -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree

docker-benchmark: docker-build
	docker run --rm -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql_benchmark --rows 1000000 --repetitions 1

docker-seed-million: docker-build
	docker run --rm -v $(DOCKER_DATA_VOLUME):/app/runtime_data -v $(DOCKER_LOG_VOLUME):/app/logs mini-sql-bptree ./mini_sql_seed --data-dir /app/runtime_data --rows 1000000
