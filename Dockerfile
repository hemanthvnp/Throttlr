# Multi-stage Dockerfile for OS Gateway
# Enterprise-grade C++ API Gateway

# ============================================
# Stage 1: Build dependencies
# ============================================
FROM ubuntu:24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++-13 \
    cmake \
    ninja-build \
    git \
    curl \
    ca-certificates \
    python3-pip \
    libssl-dev \
    libhiredis-dev \
    libnghttp2-dev \
    libzstd-dev \
    libbrotli-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Conan
RUN pip3 install --break-system-packages conan && \
    conan profile detect

WORKDIR /build

# Copy only dependency files first for caching
COPY conanfile.py CMakeLists.txt ./

# Install dependencies
RUN conan install . --build=missing -of=deps \
    -s compiler=gcc \
    -s compiler.version=13 \
    -s compiler.cppstd=20 \
    -s build_type=Release || true

# ============================================
# Stage 2: Build
# ============================================
FROM deps AS builder

COPY . /build/

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc-13 \
    -DCMAKE_CXX_COMPILER=g++-13 \
    -DGATEWAY_BUILD_TESTS=OFF \
    -DGATEWAY_BUILD_BENCHMARKS=OFF && \
    cmake --build build --parallel || \
    (echo "Build with full deps" && make -C build)

# ============================================
# Stage 3: Runtime
# ============================================
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    libssl3 \
    libhiredis1.1.0 \
    libnghttp2-14 \
    libbrotli1 \
    libzstd1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -s /bin/false gateway

WORKDIR /app

# Copy binaries from builder
COPY --from=builder /build/build/bin/gateway /app/ 2>/dev/null || \
     COPY --from=builder /build/build/gateway /app/ 2>/dev/null || true
COPY --from=builder /build/build/bin/backend /app/ 2>/dev/null || \
     COPY --from=builder /build/build/backend /app/ 2>/dev/null || true

# Copy configuration
COPY config/ /app/config/

# Create directories
RUN mkdir -p /var/log/gateway /var/run/gateway && \
    chown -R gateway:gateway /app /var/log/gateway /var/run/gateway

# Switch to non-root user
USER gateway

# Expose ports
EXPOSE 8080 9090 9091

# Health check
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8080/health || exit 1

# Environment variables
ENV GATEWAY_CONFIG=/app/config/gateway.yaml \
    GATEWAY_LOG_LEVEL=info

# Entry point
ENTRYPOINT ["/app/gateway"]
CMD ["-c", "/app/config/gateway.yaml"]
