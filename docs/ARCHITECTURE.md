# Architecture

---

## I/O model

The listen socket is non-blocking. `accept()` returns a new socket that is blocking by default — it is dispatched directly to a worker thread where `SO_RCVTIMEO` enforces per-request timeouts correctly on the blocking path.

```
accept loop  (main thread)
    │
    ├─ EAGAIN → sleep 1ms, retry
    ├─ SIGHUP flag set → call reload_config(), continue
    └─ new fd → submit to thread pool
```

**Why not epoll on client sockets?** Each accepted connection is handled synchronously in a worker thread (read headers → process → write response). The overhead of epoll-per-connection only pays off when connections are long-lived and mostly idle. For a request-response gateway with short-lived connections, the threading model is simpler and equally performant.

**Worker pool:** fixed at `hardware_concurrency()` threads. Shared state uses the right primitive for each access pattern — see the table at the bottom.

---

## Request pipeline

```
accept() → parse HTTP/1.1 → run middleware chain → route match
         → select backend → acquire pooled connection
         → forward request → stream response back
         → log + metrics → release connection
```

Each middleware returns either `{continue: true}` or `{continue: false, response: R}`. A short-circuit skips all later stages and sends `R` directly to the client — important for auth (401 before rate limit check) and rate limiting (429 before any backend I/O).

**Middleware order:**
1. JWT auth — rejects unauthenticated before consuming rate limit budget
2. Token bucket — 429 if bucket empty
3. Circuit breaker — 503 if backend is open
4. CORS — injects headers; OPTIONS preflight short-circuits here

---

## Token bucket rate limiter

Each client (keyed by IP or path) owns a bucket of capacity `C` refilling at `R` tokens/sec.

```
on request:
  elapsed = now - last_check
  tokens  = min(C, tokens + elapsed × R)   // lazy refill — no background timer
  if tokens >= 1:
    tokens -= 1 → allow
    set X-RateLimit-{Limit, Remaining, Reset} headers
  else:
    return 429, Retry-After: ceil(1/R)
```

**Why token bucket over fixed window?** Fixed windows allow a boundary burst (full limit at T−1s, full limit again at T+0s). Token bucket is continuous — no boundary.

**Distributed mode (Redis):** bucket state stored in Redis, updated with an atomic Lua `HMGET → compute → HMSET` script in a single round-trip. This prevents the GET-then-SET race that would arise from application-level read/modify/write.

**Thread safety (local mode):** per-bucket `std::mutex`; bucket map protected by the same lock; cleanup removes buckets idle > 5 min.

---

## Circuit breaker

Three-state FSM: `Closed → Open → Half-Open → Closed/Open`.

```
Closed:    track failures; if consecutive_failures >= threshold → Open
Open:      reject all requests (503); after open_timeout ms → Half-Open
Half-Open: allow one probe; success → success_count++;
           if success_count >= threshold → Closed
           failure → Open (reset timer)
```

Two configurable trip strategies:
- **Count-based** — trips after N consecutive failures. Fast detection of hard outages.
- **Rate-based** — trips when `failed/total > threshold` over a sliding window. Catches degraded (not dead) backends.

State transitions use `compare_exchange_strong` — only one thread wins; others observe the already-updated value.

---

## Load balancing

Four strategies, selected via `load_balancer.strategy` in config:

**Round-robin** (default): atomic counter mod N, single `fetch_add`, lock-free.

**Weighted round-robin**: precomputed sequence, no per-request arithmetic.

**Least-connections**: linear scan for `min(active_requests)`; O(N) where N ≤ 10 in practice, faster than a mutex-protected heap.

**Consistent hashing**: 150 virtual nodes per backend on a 2³²-wide ring (FNV-1a hash). Request key hashed → `std::map::lower_bound` (binary search) → successor node. Adding or removing a backend remaps only 1/N of keys.

All strategies skip backends with an open circuit breaker or a failed health check. Health checks run on a dedicated background thread every 5 seconds, independent of the request path.

---

## Connection pool

Per-backend pool of persistent TCP connections — eliminates the TCP handshake cost (~1 ms on LAN) per request.

```
acquire:
  lock; if idle not empty → pop_back (LIFO)
  else if total < max → unlock, open new TCP connection
  else → wait on condition_variable (5 s timeout)

release:
  if connection healthy → push_back, notify_one
  else → close fd, decrement total
```

**LIFO** returns the most-recently-used connection — least likely to have been silently closed by the backend's TCP stack, and kernel buffers are still warm.

**Idle eviction:** background cleanup thread closes connections idle > 60 s, preventing RST errors on stale sockets.

---

## Hot reload (SIGHUP)

```
signal handler (async-signal-safe):
  g_reload_flag.store(true)   // only atomic store — no malloc, no mutex

accept loop (main thread), each iteration:
  if g_reload_flag.exchange(false):
    reload_config(config_path_)
```

`reload_config` updates components in-place under their own locks:
- Rate limiter: `update_config()` swaps config + clears buckets
- Router: `clear()` + re-add routes
- Backends: `set_backends()` preserves existing pools/circuit-breakers for unchanged hosts
- Config struct: replaced (copyable plain struct)

What requires a restart: listen port, TLS certificates (SSL_CTX can't be swapped under live connections).

---

## Graceful shutdown

On `SIGINT` / `SIGTERM`:
1. `running_` set to `false` — accept loop exits
2. Listen socket closed — no new connections accepted
3. Poll `active_connections_` every 50 ms for up to 30 s
4. Background threads joined, thread pool shut down

In-flight requests on worker threads complete normally; the socket is only closed by `handle_connection` when the response is sent.

---

## CORS

```cpp
const string origin = request.header("Origin").value_or("");
const auto& allowed = config.cors_origins;

if (wildcard)                               → Access-Control-Allow-Origin: *
else if origin in allowed                  → Access-Control-Allow-Origin: {origin}
                                              Vary: Origin
else                                        → omit header (browser blocks)
```

Preflight `OPTIONS` requests short-circuit the middleware chain and return 204 with CORS headers immediately.

---

## Observability

**Prometheus metrics** at `/metrics`: request counters, P50/P95/P99 latency histogram, active connections gauge, rate-limited counter, circuit breaker open counter. All counters are `std::atomic<uint64_t>` — lock-free in the hot path.

**Access logs** (spdlog, JSON format):
```json
{"timestamp":"...","request_id":"...","client_ip":"...","method":"GET",
 "path":"/api/v1/users","status":200,"latency_ms":0.87,"backend":"b1"}
```

**Admin API** at `/_admin/`: live per-backend metrics (active requests, circuit state, pool depth, error rate), route table, aggregate stats. No authentication by default — restrict with a network policy or reverse proxy in production.

---

## Concurrency reference

| Shared data | Mechanism | Reason |
|-------------|-----------|--------|
| Request counters | `std::atomic<uint64_t>` | Write-heavy hot path |
| Circuit breaker state | `std::atomic` + CAS | Wait-free transitions |
| Backend list | `std::shared_mutex` | Many readers, rare writers |
| Connection pool | `std::mutex` + `condition_variable` | Per-backend; acquire/release |
| Rate limit buckets | Per-bucket `std::mutex` | Bucket-granularity avoids global lock |
| Config pointer | replaced under per-component locks | Reload path only |
