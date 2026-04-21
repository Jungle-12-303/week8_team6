FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential make \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY Makefile ./
COPY include ./include
COPY src ./src
COPY tests ./tests
COPY data ./data
COPY sql ./sql

RUN make

FROM debian:bookworm-slim

WORKDIR /app

COPY --from=builder /app/mini_sql /app/mini_sql
COPY --from=builder /app/mini_sql_tests /app/mini_sql_tests
COPY --from=builder /app/mini_sql_server /app/mini_sql_server
COPY --from=builder /app/mini_sql_api_tests /app/mini_sql_api_tests
COPY --from=builder /app/mini_sql_benchmark /app/mini_sql_benchmark
COPY --from=builder /app/mini_sql_seed /app/mini_sql_seed
COPY --from=builder /app/data /app/data
COPY --from=builder /app/sql /app/sql
COPY docker-entrypoint.sh /app/docker-entrypoint.sh

RUN mkdir -p /app/logs
RUN sed -i 's/\r$//' /app/docker-entrypoint.sh && chmod +x /app/docker-entrypoint.sh

EXPOSE 8080

ENTRYPOINT ["/app/docker-entrypoint.sh"]
CMD ["./mini_sql", "--repl"]
