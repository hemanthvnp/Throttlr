# Throttlr

A production-grade API gateway written in C++20. Handles TLS termination, JWT authentication, token bucket rate limiting, circuit breaking, and load balancing for microservice backends — all in a single epoll-driven process.

---

## Live Demo

Deployed on Render: **https://throttlr-gateway.onrender.com**

> Free tier spins down after 15 min idle — first request may take ~30s to wake.

```bash
# Health check
curl https://throttlr-gateway.onrender.com/health
# {"status":"healthy","uptime_seconds":100,"version":"2.0.0"}

# Readiness probe
curl https://throttlr-gateway.onrender.com/ready
# {"ready":true}

# Backend health + circuit breaker states + connection pool stats
curl https://throttlr-gateway.onrender.com/_admin/backends

# Request metrics + uptime
curl https://throttlr-gateway.onrender.com/_admin/stats

# Prometheus metrics
curl https://throttlr-gateway.onrender.com/metrics

# Rate limit headers (X-RateLimit-Limit, X-RateLimit-Remaining, X-Request-ID)
curl -I https://throttlr-gateway.onrender.com/api/public/test

# Trigger rate limiter — 429 after 100 requests/min per IP
for i in $(seq 1 105); do curl -s -o /dev/null -w "%{http_code}\n" https://throttlr-gateway.onrender.com/api/public/test; done
```

---

## Features

- **HTTP/1.1 reverse proxy** — keep-alive, connection pooling to backends, request tracing via `X-Request-ID`
- **JWT authentication** — HS256 signature verification, `iss` + `exp` claim validation; enforced per-route
- **Token bucket rate limiting** — per-IP or per-path; local in-memory or Redis-backed for multi-instance deployments
- **Circuit breaker** — Closed / Open / Half-Open FSM; count-based and failure-rate-based trip strategies
- **Load balancing** — round-robin, weighted round-robin, least-connections, consistent hashing (150 virtual nodes)
- **TLS** — client-facing termination via OpenSSL; optional TLS to backends
- **CORS** — per-origin allowlist, preflight handling, `Vary: Origin` on non-wildcard responses
- **Observability** — Prometheus metrics at `/metrics`, structured JSON access logs, live admin API
- **Hot reload** — `SIGHUP` reloads routes, backends, and rate limits with zero dropped connections
- **Graceful shutdown** — drains in-flight requests up to 30 s before exiting
- **Kubernetes-ready** — liveness/readiness probes, rolling update manifests, Docker multi-stage build

---

## Performance

Benchmarked on a 4-core / 16 GB machine (`wrk -t4 -c400 -d30s`):

| Metric | Value |
|--------|-------|
| Throughput | 50,000+ req/s |
| P50 latency | ~300 µs |
| P99 latency | ~1.8 ms |
| RSS (idle) | ~20 MB |
| RSS (load) | ~95 MB |

Component micro-benchmarks (Google Benchmark):

```
BM_TokenBucket    45 ns/op
BM_RouterMatch   189 ns/op
BM_ConnPool      124 ns/op
```

---

## Structure

```
src/
├── main.cpp                 # signal handling, arg parsing, startup
├── gateway.cpp              # Gateway class — accept loop, request pipeline, hot reload
├── authenticator.cpp        # JWT HS256 verification
├── redis_rate_limiter.cpp   # Redis Lua atomic rate limiting
└── backend.cpp              # test backend server

include/
├── common.h                 # shared includes, type aliases, constants, util functions
├── config.h                 # Config, BackendConfig, RouteConfig
├── http.h                   # HttpRequest, HttpResponse, HTTP types
├── circuit_breaker.h        # CircuitBreaker FSM
├── rate_limiter.h           # TokenBucket, RateLimiter
├── connection_pool.h        # ConnectionPool (LIFO, idle eviction)
├── load_balancer.h          # LoadBalancer (4 strategies + consistent hash ring)
├── router.h                 # regex-based Router
├── metrics.h                # Prometheus metrics, ThreadPool, AccessLogger
└── gateway.h                # Gateway declaration
```

---

## Quick start

### Build from source

```bash
# Requires: GCC 12+, CMake 3.16+, OpenSSL 3.x, hiredis
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
./bin/backend 9001 &
./bin/gateway -c ../config/local.json
```

### Docker Compose (recommended)

Starts the gateway, 3 backends, Redis, Prometheus, Jaeger, and Grafana:

```bash
docker compose up
```

| Service | URL |
|---------|-----|
| Gateway | http://localhost:8080 |
| Prometheus | http://localhost:9090 |
| Jaeger UI | http://localhost:16686 |
| Grafana | http://localhost:3000 |

### Verify

```bash
curl http://localhost:8080/health
# {"status":"healthy","uptime_seconds":5,"version":"2.0.0"}

curl -I http://localhost:8080/api/public/test
# X-RateLimit-Limit: 100
# X-RateLimit-Remaining: 99
# X-Request-ID: 550e8400-e29b-41d4-a716-446655440000

curl http://localhost:8080/_admin/backends
```

---

## Configuration

Full reference: [`config/gateway.json`](config/gateway.json)

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8080,
    "worker_threads": 0,
    "max_connections": 10000,
    "jwt_secret": "change-in-production"
  },
  "rate_limit": {
    "enabled": true,
    "requests": 100,
    "window_seconds": 60,
    "key_type": "ip",
    "storage": "redis",
    "redis_host": "127.0.0.1",
    "redis_port": 6379
  },
  "load_balancer": {
    "strategy": "least_connections"
  },
  "backends": [
    { "name": "b1", "host": "10.0.0.1", "port": 8000, "weight": 2 },
    { "name": "b2", "host": "10.0.0.2", "port": 8000, "weight": 1 }
  ],
  "routes": [
    { "path": "/api/.*", "backend": "default", "auth_required": true },
    { "path": "/.*",     "backend": "default", "auth_required": false }
  ]
}
```

**Load balancer strategies:** `round_robin` · `weighted` · `least_connections` · `consistent_hash`

**Rate limit key types:** `ip` · `path` · `header` (specify `header_name`)

**Rate limit storage:** `local` (in-memory) or `redis` (distributed, atomic Lua script)

### CLI

```
-c, --config <file>    config file  (default: config/gateway.json)
-p, --port <port>      override listen port
-w, --workers <n>      override worker thread count
-v, --version
-h, --help
```

### Hot reload

```bash
kill -HUP <gateway-pid>
# [info] Reloading configuration from config/gateway.json
# [info] Configuration reloaded — routes=2 backends=3
```

Reloads: rate limits, routes, backends, JWT secret. Requires restart: listen port, TLS certificates.

---

## Architecture

```
Client ──TLS──▶  accept loop  ──SIGHUP──▶  hot reload
                      │
              worker thread pool
              (N threads = CPU cores)
                      │
         ┌── middleware chain ───────┐
         │  1. JWT Bearer auth       │  → 401 on failure
         │  2. Token bucket limiter  │  → 429 on exceed
         │  3. Circuit breaker       │  → 503 if open
         │  4. CORS                  │
         └──────────┬────────────────┘
                    │
             load balancer
      round-robin / weighted / least-conn
        / consistent hash (virtual nodes)
                    │
          connection pool (LIFO reuse,
           idle eviction via timerfd)
                    │
            backend services
```

**I/O model:** `server_fd` is non-blocking; `accept()` returns blocking client sockets dispatched to a worker thread pool. `SO_RCVTIMEO` enforces per-request timeouts on the blocking path.

**Concurrency:** counters use `std::atomic`; circuit breaker state transitions use `compare_exchange_strong`; the backend list uses `std::shared_mutex` (many readers, rare writers); config hot-reload is protected by per-component locks with no global pause.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for deep-dives on each algorithm.

---

## API reference

### System endpoints

| Endpoint | Description |
|----------|-------------|
| `GET /health` | Liveness probe — 200 if process is alive |
| `GET /ready` | Readiness probe — 200 only when ≥1 backend is healthy |
| `GET /metrics` | Prometheus text format |
| `GET /_admin/stats` | Aggregate request stats + uptime |
| `GET /_admin/backends` | Per-backend health, circuit state, pool depth |
| `GET /_admin/routes` | Active route table |

### Rate limit response headers

```http
HTTP/1.1 429 Too Many Requests
X-RateLimit-Limit: 100
X-RateLimit-Remaining: 0
X-RateLimit-Reset: 1735001234
Retry-After: 45
```

### Prometheus metrics

| Metric | Type |
|--------|------|
| `throttlr_requests_total` | counter |
| `throttlr_request_duration_seconds` | histogram (P50/P95/P99 buckets) |
| `throttlr_active_connections` | gauge |
| `throttlr_rate_limited_total` | counter |
| `throttlr_circuit_breaker_open_total` | counter |

---

## Deployment

### Docker

```bash
docker build -t throttlr:latest .
docker run -d -p 8080:8080 \
  -v $(pwd)/config:/opt/throttlr/config \
  -e GATEWAY_CONFIG=/opt/throttlr/config/gateway.json \
  throttlr:latest
```

### Kubernetes

```bash
kubectl apply -f k8s/gateway-deployment.yaml
```

3 replicas, pod anti-affinity, readiness/liveness probes, rolling update strategy.

### Systemd

```ini
[Service]
ExecStart=/usr/local/bin/gateway -c /etc/throttlr/gateway.json
ExecReload=/bin/kill -HUP $MAINPID
Restart=always
LimitNOFILE=65536
```

---

## Tech stack

| Component | Technology |
|-----------|-----------|
| Language | C++20 |
| I/O | Linux `epoll` |
| TLS | OpenSSL 3.x |
| Config | nlohmann/json |
| Logging | spdlog |
| Redis | hiredis |
| JWT | jwt-cpp |
| Containers | Docker, Kubernetes |

---

## License

MIT
