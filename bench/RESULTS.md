# Benchmark results — 2026-08-11

Reproduce with `bench/run_benchmark.sh` (requires Docker).

**Test environment:** Docker Desktop on Windows, 24 vCPUs / ~7.6GB allocated
to the VM (not the "4-core/16GB" previously cited in the README — that
figure was not backed by any script or logged run and could not be
reproduced from anything in the repo).

**Setup:** gateway built from current `main` (image `throttlr:bench`),
rate limiting disabled (isolating raw proxy throughput), 3 backend
instances, Redis for the rate limiter's Lua script path. Backends are
`nginx:alpine`, not `src/backend.cpp` — the shipped backend is a
single-threaded, non-keep-alive stub that cannot sustain concurrent load
and bottlenecks (or trips the circuit breaker) almost immediately under
any real concurrency, so it does not exercise the gateway's own proxying
path. nginx isolates gateway performance from that stub's limitations.

## Original finding: the previously-cited "50K+ req/s" number was fake

Running `wrk -t4 -c400 -d30s` — the exact parameters in the original
claim — against the unmodified gateway returned ~41-52K req/s with
**99.999% of responses being 503s returned in microseconds**. The circuit
breaker was open and fast-failing almost every request; the number
measured rejection speed, not proxied throughput.

## Root causes found and fixed

Diagnosis (stepping concurrency up from c=1, then inspecting actual
response bodies and per-backend admin stats mid-load) turned up five
distinct, real bugs, all fixed on this branch:

1. **Stale pooled connections raced the liveness check.**
   `ConnectionPool::is_connection_alive()` (`include/connection_pool.h`)
   uses `MSG_PEEK`, which can report a connection alive moments before the
   peer closes it. A reused connection then failed `send()`/`read()`
   almost instantly (~0.1ms) — a false failure, not a backend problem.
   **Fix:** one transparent retry on a fresh connection when a *reused*
   connection fails on first use (`src/gateway.cpp`, `proxy_request`).

2. **Circuit breaker half-open state had no cap.** `CircuitBreaker::allow()`
   (`include/circuit_breaker.h`) let *every* pending request through the
   instant the breaker went half-open — one unlucky request among a flood
   immediately re-opened it for a fresh timeout window, so a trip could
   never actually recover under load. **Fix:** half-open now admits at
   most `success_threshold` concurrent trial requests.

3. **Half-open used a different (stricter) failure rule than Closed.** A
   *single* failed trial during half-open re-opened the breaker
   immediately, versus the Closed state's tolerance for
   `failure_threshold` *consecutive* failures. At real request volume,
   some trial was statistically guaranteed to land on ordinary background
   noise before a recovery batch cleared. **Fix:** unified both states
   under the same consecutive-failure threshold.

4. **Connection pool held its mutex across a blocking `connect()`.**
   `ConnectionPool::acquire()` serialized all concurrent pool growth
   through one lock while a thread was inside the `connect()` syscall,
   which can manufacture correlated timeout bursts under load — many
   threads queued behind one slow connect. **Fix:** the lock is released
   before `connect()` and re-acquired only to register the result.

5. **The `circuit_breaker` config block was dead.** `config.h` never
   parsed it, and `LoadBalancer` hardcoded `CircuitBreaker(5, 2, 30s)` at
   both construction sites — the JSON block in `gateway.json` /
   `production.json` did nothing. **Fix:** wired through
   `CircuitBreakerConfig`, and the default `open_timeout_ms` was lowered
   from 30000 to 3000 — a trip should shed load for a moment, not lock a
   backend out for 30 seconds over a handful of transient errors.

A sixth issue was found but wasn't the active cause in this environment:
the health-check loop (`Gateway::health_check_loop`) marked a backend down
after a *single* failed check with no hysteresis, which could take all
three backends offline at once if a check round happened to line up badly
under load. Fixed the same way as the circuit breaker (3 consecutive
misses required, immediate recovery on one success), but admin-stats
inspection during a live run showed `circuit_state` was the actual
mechanism behind the remaining failures in this environment, not health
flapping. This fix stands on its own merits regardless.

## A benchmark-harness artifact, not a Throttlr bug

After fixes 1-5, failures at `c=50` dropped sharply but didn't fully
disappear — a ~5-6 second block of correlated failures appeared a few
seconds into every run, at almost exactly the new 3s `open_timeout_ms`
(and multiples of it). That pattern pointed at synchronized connection
churn: all of a pool's connections to a given nginx backend get
established in the same narrow window at test start, and stock nginx's
default `keepalive_requests` (100) closes a keep-alive connection after
serving that many requests on it — so many pooled connections recycle at
once, producing a correlated burst that looks like a real outage.
Raising `keepalive_requests` on the nginx stand-in backends to a large
number eliminated the remaining failures entirely. This is a property of
using stock nginx as a benchmarking stand-in, not a bug in Throttlr —
`bench/run_benchmark.sh` now sets this for the same reason.

## Honest numbers (after all fixes)

**c=1, no concurrency, zero errors:**
- 3,754 req/s
- P50 250µs, P75 280µs, P90 329µs, P99 519µs

**c=200, `wrk -t4 -c200 -d15s`, zero non-2xx/3xx responses:**
- 28,319 req/s
- P50 648µs, P75 1.02ms, P90 1.48ms, P99 2.89ms

**c=400, `wrk -t4 -c400 -d30s` — matches the original claim's exact
parameters — zero non-2xx/3xx responses out of 857,345 requests:**
- 28,548 req/s
- P50 647µs, P75 1.02ms, P90 1.48ms, **P99 2.82ms**

This is real, reproducible throughput: every one of the 857,345 requests
in the c=400 run got a genuine 2xx response from a backend, not a
fast-fail. It lands below the originally-claimed 50K+ req/s but in the
same ballpark on P99 latency (2.82ms vs. the claimed 1.8ms), and unlike
the original number, it's backed by a script anyone can re-run
(`bench/run_benchmark.sh`).

## What this means for the README

Replace the "50K+ req/s / 1.8ms P99" line with the c=400 numbers above,
and note the test environment (24 vCPU / ~7.6GB Docker Desktop VM, not
"4-core/16GB") and that the number reflects gateway+Redis-rate-limiter
overhead proxying to nginx, not the shipped `src/backend.cpp` stub.
