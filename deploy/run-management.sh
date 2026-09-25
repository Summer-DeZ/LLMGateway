#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
if [[ -z "${GW_PG_DSN:-}" ]]; then
    if [[ -z "${GW_PG_PASSWORD:-}" && -f deploy/.env ]]; then
        set -a
        source deploy/.env
        set +a
    fi
    if [[ -z "${PGPASSWORD:-}" ]]; then
        export PGPASSWORD="${GW_PG_PASSWORD:?Set GW_PG_DSN, PGPASSWORD, or GW_PG_PASSWORD in the environment or deploy/.env}"
    fi
    export GW_PG_DSN='host=127.0.0.1 port=5433 dbname=gateway user=gateway'
fi
exec ./build/dev/llm-gateway -c home/management.json "$@"
