# Build stage: Compile the Gateway binaries natively
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install core build dependencies required for Throttlr
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    libssl-dev \
    libhiredis-dev \
    curl

WORKDIR /app

# Copy the full source codebase
COPY . .

# Build the gateway using the Makefile
RUN make gateway backend

# Execution stage
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
RUN mkdir -p /opt/throttlr/config /opt/throttlr/logs && chown -R throttlr:throttlr /opt/throttlr

# Copy solely the compiled gateway engine and its internal configs from the builder layer
COPY --from=builder --chown=throttlr:throttlr /app/build/gateway /usr/local/bin/gateway
COPY --from=builder --chown=throttlr:throttlr /app/build/backend /usr/local/bin/backend
COPY --from=builder --chown=throttlr:throttlr /app/config/*.json /opt/throttlr/config/
COPY --from=builder --chown=throttlr:throttlr /app/config/gateway.conf /opt/throttlr/config/

# Execute the container under the security of the standard non-root user
USER throttlr

# Present standard web gateway ports (8080 for HTTP)
EXPOSE 8080

# Start the gateway
ENTRYPOINT ["/usr/local/bin/gateway"]
