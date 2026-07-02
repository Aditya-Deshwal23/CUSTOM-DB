# Custom DB Engine

A custom relational database and storage engine implemented in C++ with a Crow HTTP backend and a vanilla static dashboard. The project is packaged as one Dockerized web service: the backend exposes database APIs, serves the dashboard from `dashboard/`, and provides a health check for deployment platforms.

## Live Demo

Live Render deployment: [https://custom-db-engine.onrender.com](https://custom-db-engine.onrender.com)

Render Free tier is currently used, so the first request after inactivity may take some time while the service wakes up.

Available routes:

- `/`: dashboard
- `/health`: health check
- `/api/metrics`: runtime metrics
- `/api/query`: query execution API

## Project Overview

- C++ storage engine with page, buffer pool, disk manager, B+ tree, WAL, and executor components.
- Crow-based HTTP API for runtime metrics and query execution.
- Vanilla HTML/CSS/JavaScript dashboard served by the same backend origin.
- Docker deployment with configurable port and database path.
- Persistent storage support through a mounted data directory.

## Technology

- C++17
- Crow
- CMake
- Docker
- Render
- Vanilla HTML, CSS, and JavaScript

## Local Docker Run

```bash
docker build -t custom-db .
docker run --rm -p 8080:8080 -e PORT=8080 -e DB_PATH=/data/custom.db -v custom-db-data:/data custom-db
```

Then open [http://localhost:8080](http://localhost:8080).

## Deployment

The deployed service is a Dockerized C++ Crow backend. The same backend serves the static dashboard from `dashboard/`, so the dashboard can call `/api/metrics` and `/api/query` on the same origin without a separate frontend deployment.

Render deployment uses the root `Dockerfile`.

Required environment variables:

- `PORT`: HTTP port used by Crow. Defaults to `8080` locally.
- `DB_PATH`: database file path used by the storage engine. Defaults to `./test.db` locally.

On Render Free tier, filesystem storage is temporary. For production persistence, mount a persistent disk at `/data` and set:

```bash
DB_PATH=/data/custom.db
```

## Health Check

```bash
curl https://custom-db-engine.onrender.com/health
```

Expected response:

```json
{"status":"ok"}
```

## Documentation

Detailed local build, Docker, Docker Compose, and deployment instructions are in [RUN_INSTRUCTIONS.md](RUN_INSTRUCTIONS.md).
