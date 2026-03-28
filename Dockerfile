# Build stage: Compile the Gateway binaries natively
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install core build dependencies required for Throttlr (C++20, CMake, OpenSSL, Hiredis)
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libhiredis-dev

WORKDIR /app

# Copy the full C++ source codebase from the repository
COPY . .

# Generate the CMake makefiles and compile using all available CPU threads
RUN mkdir -p build_cmake && cd build_cmake && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc) gateway

# Provide the backend service tool compilation as well for integrated testing (optional)
RUN cd build_cmake && make -j$(nproc) backend || true

# Execution stage: Construct a minimal container strictly for deploying the hardened gateway
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install only the runtime libraries necessary for the system
RUN apt-get update && apt-get install -y \
    libssl3 \
    libhiredis0.14 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Establish a secure non-root user specifically for running the open port service
RUN useradd -m -s /bin/bash throttlr

WORKDIR /opt/throttlr
RUN mkdir -p /opt/throttlr/config && chown -R throttlr:throttlr /opt/throttlr

# Copy solely the compiled gateway engine and its internal configs from the builder layer
COPY --from=builder --chown=throttlr:throttlr /app/build_cmake/bin/gateway /usr/local/bin/gateway
COPY --from=builder --chown=throttlr:throttlr /app/config/*.json /opt/throttlr/config/

# Execute the container under the security of the standard non-root user
USER throttlr

# Present standard web gateway ports (8080 for HTTP / 8443 for HTTPS edge tunnel)
EXPOSE 8080 8443

# Start the gateway natively pulling from its deployment working directory configuration
ENTRYPOINT ["/usr/local/bin/gateway", "-c", "config/gateway.json"]
