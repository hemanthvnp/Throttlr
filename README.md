# Throttlr

<p align="center">
  <strong>High-Performance API Gateway</strong>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#quick-start">Quick Start</a> •
  <a href="#configuration">Configuration</a> •
  <a href="#api-reference">API</a> •
  <a href="#deployment">Deployment</a>
</p>

---

## Overview

Throttlr is a production-ready, high-performance API Gateway built with modern C++20. It provides essential gateway features for microservices architectures including load balancing, rate limiting, circuit breaker pattern, and comprehensive observability.

## Features

### Core Capabilities
- **High Performance**: Multi-threaded architecture with connection pooling
- **HTTP/1.1 Support**: Full HTTP/1.1 reverse proxy with keep-alive
- **Graceful Shutdown**: Signal handling with clean connection draining

### Load Balancing
- **Round-Robin**: Even distribution across backends
- **Weighted Round-Robin**: Priority-based traffic distribution
- **Health Checking**: Automatic HTTP health checks with configurable intervals
- **Connection Pooling**: Persistent backend connections with idle cleanup

### Security & Traffic Control
- **Rate Limiting**: Token bucket algorithm with configurable limits
- **Circuit Breaker**: Fault tolerance with automatic recovery
- **CORS**: Configurable cross-origin resource sharing
- **Request Tracing**: UUID-based X-Request-ID for distributed tracing

### Observability
- **Prometheus Metrics**: `/metrics` endpoint with request latency histograms
- **Health Endpoints**: Kubernetes-ready `/health`, `/healthz`, `/ready`, `/readyz`
- **Admin API**: Runtime statistics and backend status
- **Access Logging**: JSON and Combined log formats

## Quick Start

### Prerequisites
- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.16+
- OpenSSL

### Building from Source

```bash
# Clone the repository
git clone https://github.com/yourusername/throttlr.git
cd throttlr

# Build with CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Start a test backend (optional)
./bin/backend 9001 &

# Start the gateway
./bin/gateway -c ../config/gateway.json
```

### Verify Installation

```bash
# Health check
curl http://localhost:8080/health
# {"status":"healthy","uptime_seconds":10,"version":"2.0.0"}

# Metrics
curl http://localhost:8080/metrics

# Admin stats
curl http://localhost:8080/_admin/stats
```

## Configuration

Throttlr uses JSON configuration. See `config/gateway.json` for a complete example.

### Basic Configuration

```json
{
  "server": {
    "host": "0.0.0.0",
    "port": 8080,
    "worker_threads": 0,
    "max_connections": 10000
  },
  "rate_limit": {
    "enabled": true,
    "requests": 100,
    "window_seconds": 60
  },
  "cors": {
    "enabled": true,
    "origins": ["*"]
  },
  "backends": [
    {"name": "backend1", "host": "127.0.0.1", "port": 9001, "weight": 1},
    {"name": "backend2", "host": "127.0.0.1", "port": 9002, "weight": 1}
  ],
  "routes": [
    {"name": "api", "path": "/api/.*", "backend": "default"},
    {"name": "default", "path": "/.*", "backend": "default"}
  ]
}
```

### Configuration Options

| Option | Description | Default |
|--------|-------------|---------|
| `server.host` | Bind address | `0.0.0.0` |
| `server.port` | Listen port | `8080` |
| `server.worker_threads` | Thread pool size (0 = auto) | `0` |
| `rate_limit.requests` | Requests per window | `100` |
| `rate_limit.window_seconds` | Rate limit window | `60` |
| `backends[].weight` | Load balancer weight | `1` |
| `backends[].health_path` | Health check endpoint | `/health` |

### Command Line Options

```
Usage: gateway [OPTIONS]

Options:
  -c, --config <file>   Configuration file path (default: config/gateway.json)
  -p, --port <port>     Override listening port
  -w, --workers <num>   Number of worker threads
  -l, --log-level <lvl> Log level: trace, debug, info, warn, error
  -v, --version         Print version and exit
  -h, --help            Print help message
```

## API Reference

### Health Check Endpoints

```bash
# Liveness probe
curl http://localhost:8080/health
curl http://localhost:8080/healthz

# Readiness probe
curl http://localhost:8080/ready
curl http://localhost:8080/readyz
```

Response:
```json
{"status":"healthy","uptime_seconds":120,"version":"2.0.0"}
```

### Prometheus Metrics

```bash
curl http://localhost:8080/metrics
```

Available metrics:
- `throttlr_requests_total` - Total HTTP requests
- `throttlr_requests_success_total` - Successful requests (2xx)
- `throttlr_requests_client_error_total` - Client errors (4xx)
- `throttlr_requests_server_error_total` - Server errors (5xx)
- `throttlr_rate_limited_total` - Rate limited requests
- `throttlr_circuit_breaker_open_total` - Circuit breaker triggers
- `throttlr_active_connections` - Current active connections
- `throttlr_request_duration_seconds_bucket` - Latency histogram

### Admin API

```bash
# Get runtime statistics
curl http://localhost:8080/_admin/stats

# Get backend status
curl http://localhost:8080/_admin/backends

# Get route configuration
curl http://localhost:8080/_admin/routes
```

### Response Headers

All proxied responses include:
- `X-Request-ID` - Unique request identifier (UUID)
- `X-Response-Time` - Backend response time
- `Server: Throttlr/2.0.0`
- CORS headers (when enabled)

## Architecture

```
┌────────────┐      ┌─────────────────────────────────────────┐
│   Client   │─────▶│              Throttlr                   │
└────────────┘      │                                         │
                    │  ┌─────────────────────────────────┐    │
                    │  │         Request Pipeline        │    │
                    │  │  ┌──────┐  ┌──────┐  ┌───────┐  │    │
                    │  │  │ Rate │─▶│Circuit│─▶│ CORS  │  │    │
                    │  │  │Limit │  │Breaker│  │       │  │    │
                    │  │  └──────┘  └──────┘  └───────┘  │    │
                    │  └──────────────┬──────────────────┘    │
                    │                 │                       │
                    │  ┌──────────────▼──────────────────┐    │
                    │  │          Load Balancer          │    │
                    │  │   (Round-Robin / Weighted)      │    │
                    │  └──────────────┬──────────────────┘    │
                    │                 │                       │
                    │  ┌──────────────▼──────────────────┐    │
                    │  │       Connection Pool           │    │
                    │  └──────────────┬──────────────────┘    │
                    └─────────────────┼───────────────────────┘
                                      │
          ┌───────────────────────────┼───────────────────────┐
          │                           │                       │
   ┌──────▼──────┐             ┌──────▼──────┐         ┌──────▼──────┐
   │  Backend 1  │             │  Backend 2  │         │  Backend N  │
   │   (healthy) │             │  (healthy)  │         │ (unhealthy) │
   └─────────────┘             └─────────────┘         └─────────────┘
```

## Deployment

### Systemd Service

```ini
# /etc/systemd/system/throttlr.service
[Unit]
Description=Throttlr API Gateway
After=network.target

[Service]
Type=simple
User=throttlr
ExecStart=/usr/local/bin/gateway -c /etc/throttlr/gateway.json
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### Docker

```dockerfile
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y libssl3
COPY build/bin/gateway /usr/local/bin/
COPY config/gateway.json /etc/throttlr/
EXPOSE 8080
CMD ["gateway", "-c", "/etc/throttlr/gateway.json"]
```

### Kubernetes

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: throttlr
spec:
  replicas: 3
  selector:
    matchLabels:
      app: throttlr
  template:
    spec:
      containers:
      - name: throttlr
        image: throttlr:latest
        ports:
        - containerPort: 8080
        livenessProbe:
          httpGet:
            path: /healthz
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 10
        readinessProbe:
          httpGet:
            path: /ready
            port: 8080
          initialDelaySeconds: 5
          periodSeconds: 5
```

## Performance

Tested on a 4-core machine with 16GB RAM:

| Metric | Value |
|--------|-------|
| Requests/sec | 50,000+ |
| P50 Latency | < 1ms |
| P99 Latency | < 5ms |
| Memory (idle) | ~20MB |
| Memory (under load) | ~100MB |

## License

MIT License - see [LICENSE](LICENSE) for details.
