# Relational Storage Engine: Build & Run Instructions

This repo deploys as one C++ Crow backend that serves the vanilla static dashboard from `dashboard/`. There is no Node, Vite, or React build step.

## Prerequisites

- CMake 3.20+
- A C++17 compiler such as GCC, Clang, or MinGW/UCRT64
- Git, required by CMake FetchContent for Crow and Asio
- Docker, optional for containerized local testing

## Environment Variables

- `PORT`: HTTP port for the Crow server. Defaults to `8080`.
- `DB_PATH`: database file path. Defaults to `./test.db`.

For containers, `DB_PATH` can point to a mounted volume such as `/data/custom.db`. The volume retains page-file bytes, but API records are not restored after restart because the tree root is not persisted.

## Local Build And Run

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
PORT=8080 DB_PATH=./test.db ./build/relational_db
```

Open [http://localhost:8080](http://localhost:8080). The health check is available at [http://localhost:8080/health](http://localhost:8080/health).

On Windows with MSYS2/UCRT64, configure with your MinGW generator/compiler as needed:

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## Docker

Build the production image:

```bash
docker build -t custom-db .
```

Run it with a named volume:

```bash
docker run --rm -p 8080:8080 \
  -e PORT=8080 \
  -e DB_PATH=/data/custom.db \
  -v custom-db-data:/data \
  custom-db
```

Then open [http://localhost:8080](http://localhost:8080).

## Docker Compose

For local container testing:

```bash
docker compose up --build
```

This maps `8080:8080`, sets `DB_PATH=/data/custom.db`, and mounts the named volume `custom-db-data` to `/data`.

## Deployment Notes

Render:
- Deploy the Dockerfile from the repo root.
- Set `PORT` to the value Render provides or `8080` if using a Docker web service with that exposed port.
- Set `DB_PATH` to a path on a mounted persistent disk, for example `/data/custom.db`.
- Mount persistent storage at `/data`.

The dashboard uses same-origin requests to `/api/v1/query`, `/api/v1/stats`, `/api/v1/stats/buffer_pool`, `/api/v1/stats/storage`, `/api/v1/tables`, and `/api/v1/version`. The deprecated `/api/metrics` and `/api/query` routes remain available to existing clients.

## Local Dashboard

When serving the dashboard separately for development, requests target the local engine at `127.0.0.1:8080`. Without a reachable engine the page explicitly shows an offline/waking state and retries with backoff; it does not switch to fabricated values.

```bash
cd dashboard
python -m http.server 5500
```

Visit [http://localhost:5500](http://localhost:5500). The engine must also be running at port `8080`. To record a real session for `?replay=1`, run `python3 scripts/record_session.py`; the output defaults to `dashboard/replay/session.json`.

## Supported Console Commands

The console accepts the SQL subset documented in [docs/SQL_EXECUTION.md](docs/SQL_EXECUTION.md). Results and errors are returned by `POST /api/v1/query`. Legacy integer commands remain available on the deprecated `/api/query` endpoint.

## Tests and checks

```bash
ctest --test-dir build --output-on-failure
scripts/run_known_bugs.sh
scripts/check.sh
```

Known-bug tests are disabled in normal CTest runs. The dedicated script enables them and prints a results table.
