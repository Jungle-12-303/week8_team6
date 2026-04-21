#!/bin/sh
set -eu

DATA_DIR="${MINI_SQL_DATA_DIR:-/app/runtime_data}"

mkdir -p "$DATA_DIR" /app/logs

if [ ! -f "$DATA_DIR/users.tbl" ] && [ -f /app/data/users.tbl ]; then
    cp /app/data/users.tbl "$DATA_DIR/users.tbl"
fi

if [ "${1:-}" = "./mini_sql" ] || [ "${1:-}" = "./mini_sql_server" ]; then
    cmd="$1"
    shift

    has_data_dir=0
    for arg in "$@"; do
        if [ "$arg" = "--data-dir" ]; then
            has_data_dir=1
            break
        fi
    done

    if [ "$has_data_dir" -eq 0 ]; then
        set -- "$cmd" --data-dir "$DATA_DIR" "$@"
    else
        set -- "$cmd" "$@"
    fi
fi

exec "$@"
