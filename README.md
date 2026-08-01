# StrataDB

![C++](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![Build](https://github.com/coolspartak9819-rgb/stratadb-cpp/actions/workflows/ci.yml/badge.svg)
![Docker](https://img.shields.io/badge/Docker-ready-2496ED?logo=docker&logoColor=white)

> A compact C++20 storage engine with an HTTP API and durable write-ahead log.

![StrataDB architecture](docs/architecture.svg)

StrataDB is a C++20 key-value storage engine built to explore the systems
problems behind durable backend infrastructure.

## Current Slice

The first vertical slice includes:

• thread-safe in-memory key-value storage;
• append-only binary WAL;
• replay and recovery after process restart;
• HTTP API for `GET`, `PUT` and `DELETE`;
• health endpoint and Prometheus-style metrics;
• a minimal browser status page;
• CMake build, unit tests and Docker Compose.

## What Is Already Working

| Capability | Evidence |
| --- | --- |
| Durable writes | Every `PUT` and `DELETE` is appended to the binary WAL before the in-memory state changes |
| Crash recovery | The WAL is replayed during startup and restores the latest state |
| Compaction | `POST /compact` writes a sorted SSTable and safely resets the WAL |
| Concurrent access | Store operations are protected by a mutex and HTTP requests are handled in separate threads |
| HTTP interface | `GET`, `PUT`, `DELETE`, health, metrics and a browser status page |
| Reproducible delivery | Docker image, Compose setup, CMake tests and GitHub Actions CI |

## API At A Glance

```text
PUT /kv/user-1  ──►  WAL append  ──►  memory update  ──►  201 Created
GET /kv/user-1  ──►  memory lookup  ──►  200 OK + value
DELETE /kv/user-1 ─►  WAL append  ──►  memory erase  ──►  204 No Content
GET /metrics    ──►  request counter + key count
POST /compact   ──►  sorted SSTable snapshot + WAL reset
```

## Run

```bash
docker compose up --build
```

Open [http://localhost:8080](http://localhost:8080).

Store a value:

```bash
curl -X PUT http://localhost:8080/kv/user-1 \
  -H 'Content-Type: text/plain' \
  --data 'Igor'
```

Read it back:

```bash
curl http://localhost:8080/kv/user-1
```

Metrics:

```bash
curl http://localhost:8080/metrics
```

Compact the current state into an SSTable:

```bash
curl -X POST http://localhost:8080/compact
```

The WAL is mounted at `data/stratadb.wal`. Restarting the container restores
the key-value state from the log.

## Recovery Proof

The following scenario is covered by the local smoke test:

```text
$ curl -X PUT http://localhost:8080/kv/demo --data 'persistent value'
{"status":"stored"}

$ docker compose restart stratadb

$ curl http://localhost:8080/kv/demo
persistent value
```

The value is read from the replayed WAL after the process restart, not from a
preloaded fixture.

## Verification

The GitHub Actions pipeline runs a clean CMake build, the unit test suite and
a Docker image build on every push and pull request.

The storage format is intentionally small and inspectable: each mutation is
written as an operation byte followed by key and value lengths and their raw
bytes. The store replays this append-only log before accepting requests.

## Local Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Roadmap

The current storage layer has a WAL, an in-memory table and sorted SSTable
compaction. The next milestones are background compaction, snapshots, TTL,
benchmarks, Prometheus histograms and a three-node replication layer. Each
milestone will be backed by tests and a reproducible demo.
