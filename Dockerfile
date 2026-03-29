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

# Generate the CMake makefiles and compile using limited concurrency to prevent OOM
RUN mkdir -p build_cmake && cd build_cmake && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j2 gateway

# Provide the backend service tool compilation as well for integrated testing (optional)
RUN cd build_cmake && make -j2 backend || true

# Execution stage: Construct a minimal container strictly for deploying the hardened gateway
FROM ubuntu:22.04 AS runtime

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
COPY --from=builder --chown=throttlr:throttlr /app/build_cmake/bin/backend /usr/local/bin/backend
COPY --from=builder --chown=throttlr:throttlr /app/config/*.json /opt/throttlr/config/

# Set up the startup script wrapper for all-in-one deployment
COPY start.sh /opt/throttlr/start.sh
RUN chmod +x /opt/throttlr/start.sh

# Execute the container under the security of the standard non-root user
USER throttlr

# Present standard web gateway ports (8080 for HTTP / 8443 for HTTPS edge tunnel)
EXPOSE 8080 8443 9001 9002 9003

# Start the gateway alongside local backend servers using the wrapper script
ENTRYPOINT ["/opt/throttlr/start.sh"]
