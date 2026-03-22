# OS Gateway

<p align="center">
  <strong>Enterprise-grade C++ API Gateway</strong>
</p>

<p align="center">
  <a href="#features">Features</a> •
  <a href="#quick-start">Quick Start</a> •
  <a href="#configuration">Configuration</a> •
  <a href="#architecture">Architecture</a> •
  <a href="#api">API</a> •
  <a href="#deployment">Deployment</a>
</p>

---

## Overview

OS Gateway is a high-performance, production-ready API Gateway built with modern C++20. It provides enterprise-grade features comparable to Kong, Envoy, and Traefik, with the performance benefits of native code.

## Features

### Core Capabilities
- **High Performance**: Event-driven I/O with epoll, capable of handling 100k+ requests/second
- **HTTP/1.1 & HTTP/2**: Full protocol support with multiplexing and server push
- **WebSocket Proxying**: Bidirectional WebSocket support
- **TLS/mTLS**: OpenSSL-based TLS termination with mutual TLS support

### Load Balancing
- **Multiple Strategies**: Round-robin, weighted round-robin, least connections, consistent hashing, IP hash
- **Health Checking**: Automatic backend health monitoring
- **Circuit Breaker**: Fault tolerance with configurable thresholds
- **Connection Pooling**: Persistent backend connections with health-aware selection

### Security
- **JWT Authentication**: RS256/ES256/HS256 with JWKS rotation
- **OAuth 2.0/OIDC**: Token introspection support
- **API Key Authentication**: Header and query parameter extraction
- **Rate Limiting**: Token bucket algorithm with Redis support for distributed limiting
- **WAF**: SQL injection, XSS, and path traversal detection
- **CORS**: Fine-grained cross-origin control
- **Security Headers**: HSTS, CSP, X-Frame-Options, and more

### Observability
- **Prometheus Metrics**: Request latency, error rates, connection pools
- **Distributed Tracing**: OpenTelemetry with Jaeger/Zipkin export
- **Structured Logging**: JSON-formatted logs with request context
- **Admin API**: Runtime configuration and monitoring

### Advanced Features
- **Request/Response Transformation**: Header manipulation, URL rewriting
- **Caching**: In-memory LRU and Redis-backed response caching
- **Compression**: gzip and Brotli support
- **Hot Reload**: Configuration updates without restart

## Quick Start

### Prerequisites
- C++20 compiler (GCC 12+ or Clang 15+)
- CMake 3.20+
- OpenSSL 3.0+
- libhiredis (for Redis support)
- libnghttp2 (for HTTP/2)

### Building from Source

```bash
# Clone the repository
git clone https://github.com/os-gateway/os-gateway.git
cd os-gateway

# Build with CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Start the gateway
./bin/gateway -c ../config/gateway.yaml
```

### Using Docker

```bash
# Build and run with Docker Compose
docker-compose up -d

# Or build the image directly
docker build -t os-gateway .
docker run -p 8080:8080 -v $(pwd)/config:/app/config os-gateway
```

### Using Helm (Kubernetes)

```bash
# Add the Helm repository
helm repo add os-gateway https://charts.os-gateway.io

# Install with default values
helm install my-gateway os-gateway/os-gateway

# Or with custom values
helm install my-gateway os-gateway/os-gateway -f values.yaml
```

## Configuration

OS Gateway uses YAML configuration. See `config/gateway.yaml` for a complete example.

### Basic Configuration

```yaml
server:
  host: "0.0.0.0"
  port: 8080
  worker_threads: 0  # 0 = auto (CPU cores)
  enable_http2: true

backends:
  - name: "api-server-1"
    host: "10.0.0.1"
    port: 8080
    weight: 1
    health_check_path: "/health"

routes:
  - name: "api-v1"
    path_pattern: "/api/v1/.*"
    backend_group: "default"
    load_balancer: "round_robin"
    rate_limit_requests: 100
    rate_limit_window_seconds: 60
```

### Rate Limiting

```yaml
rate_limit:
  enabled: true
  storage: "redis"
  redis_url: "redis://localhost:6379"
  default_requests: 100
  default_window_seconds: 60
```

### JWT Authentication

```yaml
jwt:
  enabled: true
  algorithm: "RS256"
  jwks_url: "https://auth.example.com/.well-known/jwks.json"
  issuer: "https://auth.example.com"
  verify_exp: true
```

### TLS Configuration

```yaml
tls:
  enabled: true
  cert_file: "/etc/gateway/certs/server.crt"
  key_file: "/etc/gateway/certs/server.key"
  ca_file: "/etc/gateway/certs/ca.crt"
  verify_client: true  # Enable mTLS
  min_version: "TLSv1.2"
```

## Architecture

```
                                    ┌─────────────────────────────────────┐
                                    │           OS Gateway                │
┌──────────┐                        │                                     │
│  Client  │◄───────────────────────┤  ┌─────────────────────────────┐   │
└──────────┘        HTTP/2          │  │      Middleware Chain       │   │
                    TLS             │  │  ┌──────┐ ┌──────┐ ┌──────┐ │   │
                                    │  │  │ Auth │→│ Rate │→│ WAF  │ │   │
                                    │  │  └──────┘ └──────┘ └──────┘ │   │
                                    │  └─────────────┬───────────────┘   │
                                    │                │                   │
                                    │  ┌─────────────▼───────────────┐   │
                                    │  │        Router               │   │
                                    │  └─────────────┬───────────────┘   │
                                    │                │                   │
                                    │  ┌─────────────▼───────────────┐   │
                                    │  │     Load Balancer          │   │
                                    │  │  ┌────┐ ┌────┐ ┌────┐       │   │
                                    │  │  │ RR │ │ LC │ │ CH │       │   │
                                    │  │  └────┘ └────┘ └────┘       │   │
                                    │  └─────────────┬───────────────┘   │
                                    │                │                   │
                                    └────────────────┼───────────────────┘
                                                     │
                    ┌────────────────────────────────┼────────────────────────────────┐
                    │                                │                                │
             ┌──────▼──────┐                  ┌──────▼──────┐                  ┌──────▼──────┐
             │  Backend 1  │                  │  Backend 2  │                  │  Backend 3  │
             └─────────────┘                  └─────────────┘                  └─────────────┘
```

## API Reference

### Health Check
```bash
curl http://localhost:8080/health
```

### Metrics (Prometheus)
```bash
curl http://localhost:8080/metrics
```

### Admin API
```bash
# List routes
curl http://localhost:9091/admin/routes

# Get backend health
curl http://localhost:9091/admin/backends

# Reload configuration
curl -X POST http://localhost:9091/admin/config/reload
```

## Performance

Benchmarks on a 4-core machine:

| Metric | Value |
|--------|-------|
| Requests/sec | 150,000+ |
| P50 Latency | 0.5ms |
| P99 Latency | 2ms |
| Memory (idle) | 50MB |
| Memory (load) | 200MB |

## Deployment

### Kubernetes

```bash
# Install with Helm
helm install gateway ./deploy/helm \
  --set replicaCount=3 \
  --set autoscaling.enabled=true \
  --set redis.enabled=true
```

### Docker Compose

```bash
docker-compose up -d
```

### Systemd

```bash
sudo cp deploy/os-gateway.service /etc/systemd/system/
sudo systemctl enable os-gateway
sudo systemctl start os-gateway
```

## Monitoring

### Grafana Dashboards

Import the pre-built dashboards from `deploy/grafana/dashboards/`:
- Gateway Overview
- Request Latency
- Error Rates
- Backend Health

### Alerts

Example Prometheus alerting rules in `deploy/prometheus-alerts.yml`.

## Development

### Building

```bash
# Debug build with sanitizers
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_ENABLE_ASAN=ON
cmake --build build

# Run tests
cd build && ctest --output-on-failure

# Run benchmarks
./bin/gateway_benchmarks
```

### Code Style

```bash
# Format code
find src include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

# Static analysis
cppcheck --enable=all src/
```

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

MIT License - see [LICENSE](LICENSE) for details.

## Support

- Documentation: https://docs.os-gateway.io
- Issues: https://github.com/os-gateway/os-gateway/issues
- Discussions: https://github.com/os-gateway/os-gateway/discussions
