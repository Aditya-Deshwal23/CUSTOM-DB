# Relational Storage Engine: Build & Run Instructions

This repo deploys as one C++ Crow backend that serves the vanilla static dashboard from `dashboard/`. There is no Node, Vite, or React build step.

## Prerequisites

- CMake 3.14+
- A C++17 compiler such as GCC, Clang, or Visual Studio 2022
- Git, required by CMake FetchContent for Crow and Asio
- Docker, optional for containerized local testing

## Environment Variables

- `PORT`: HTTP port for the Crow server. Defaults to `8080`.
- `DB_PATH`: database file path. Defaults to `./test.db`.

In production, `DB_PATH` must point to persistent storage. For containers, use a mounted volume such as `/data/custom.db`.

## Local Build And Run

From the project root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
PORT=8080 DB_PATH=./test.db ./build/relational_db
```

Open [http://localhost:8080](http://localhost:8080). The health check is available at [http://localhost:8080/health](http://localhost:8080/health).

On Windows with MSYS2, configure with your MinGW generator/compiler as needed:

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j
```

## Docker

Build the production image:

```bash
docker build -t custom-db .
```

Run it with persistent database storage:

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

Fly.io:
- Deploy with the Dockerfile from the repo root.
- Keep the service internal port aligned with `PORT=8080`.
- Create and mount a Fly volume at `/data`.
- Set `DB_PATH=/data/custom.db`.

The dashboard uses same-origin API calls when served by the backend, so production requests go to `/api/metrics` and `/api/query` on the same host.

## Local Dashboard Demo Mode

If you only want to demo the frontend without the C++ engine running:

```bash
cd dashboard
python -m http.server 5500
```

Visit [http://localhost:5500](http://localhost:5500). The dashboard keeps a localhost backend fallback for ports `5500` and `5501`; if the backend is unavailable, it reverts to simulation mode.

## Console Commands

Try entering these commands in the dashboard SQL Console:

- `SELECT * FROM Users`
- `INSERT 42 100`
- `SELECT 10`


