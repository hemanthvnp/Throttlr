#!/usr/bin/env bash
# Reproducible load test for the Throttlr gateway.
#
# Note on backends: src/backend.cpp is a single-threaded, non-keep-alive
# stub (accept -> read -> respond -> close, one connection at a time). It
# cannot sustain concurrent load and is not representative of a real
# upstream, so this script fronts the gateway with nginx:alpine instead to
# measure the gateway/proxy path in isolation. Point BACKEND_IMAGE at
# something else if you want to test a different upstream.
#
# Requires: Docker.
set -euo pipefail

NET=throttlr-bench-net
IMAGE=throttlr:bench
WRK_IMAGE=williamyeh/wrk

cleanup() {
  docker rm -f gw-redis gw-b1 gw-b2 gw-b3 gw-gateway >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cleanup
docker network create "$NET" >/dev/null

docker build -t "$IMAGE" "$(dirname "$0")/.." >/dev/null

docker run -d --name gw-redis --network "$NET" redis:7-alpine >/dev/null

for i in 1 2 3; do
  # keepalive_requests is raised from nginx's low stock default (100) —
  # left at the default, all 3 backends' pooled connections tend to hit
  # that cap around the same wall-clock time (they're established in the
  # same narrow window at load-test start) and get recycled in a
  # synchronized burst, which reads as a correlated failure spike and can
  # trip Throttlr's circuit breaker even though nothing is actually wrong.
  # That's a benchmark-harness artifact of the stock nginx defaults, not a
  # Throttlr bug — see RESULTS.md.
  docker run -d --name gw-b$i --network "$NET" nginx:alpine \
    sh -c "mkdir -p /usr/share/nginx/html && echo OK > /usr/share/nginx/html/health && echo 'keepalive_requests 1000000;' > /etc/nginx/conf.d/keepalive.conf && nginx -g 'daemon off;'" >/dev/null
done

docker run -d --name gw-gateway --network "$NET" -p 18080:8080 \
  -v "$(cd "$(dirname "$0")" && pwd)/bench.json:/opt/throttlr/config/bench.json:ro" \
  -e GATEWAY_CONFIG=/opt/throttlr/config/bench.json \
  -e REDIS_URL=redis://gw-redis:6379 \
  --entrypoint /usr/local/bin/gateway \
  "$IMAGE" -c /opt/throttlr/config/bench.json >/dev/null

sleep 6

echo "=== sanity check ==="
curl -s -o /dev/null -w "status: %{http_code}\n" http://localhost:18080/

echo "=== c=1 baseline (no concurrency) ==="
docker run --rm --network "$NET" "$WRK_IMAGE" -t1 -c1 -d10s --latency http://gw-gateway:8080/

echo "=== c=200 ==="
docker run --rm --network "$NET" "$WRK_IMAGE" -t4 -c200 -d15s --latency http://gw-gateway:8080/

echo "=== c=400, d=30s (matches originally-claimed benchmark parameters) ==="
docker run --rm --network "$NET" "$WRK_IMAGE" -t4 -c400 -d30s --latency http://gw-gateway:8080/
