# StrataDB

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

The WAL is mounted at `data/stratadb.wal`. Restarting the container restores
the key-value state from the log.

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

The next milestones are MemTable and SSTable storage, background compaction,
snapshots, TTL, benchmarks, Prometheus histograms and a three-node replication
layer. Each milestone will be backed by tests and a reproducible demo.
