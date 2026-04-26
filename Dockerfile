# Build stage
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    libhiredis-dev

WORKDIR /app
COPY . .

RUN mkdir -p build_cmake && cd build_cmake && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j2 gateway

RUN cd build_cmake && make -j2 backend || true

# Runtime stage
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    libssl3 \
    libhiredis0.14 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN useradd -m -s /bin/bash throttlr

WORKDIR /opt/throttlr
RUN mkdir -p /opt/throttlr/config /opt/throttlr/certs && chown -R throttlr:throttlr /opt/throttlr

COPY --from=builder --chown=throttlr:throttlr /app/build_cmake/bin/gateway /usr/local/bin/gateway
COPY --from=builder --chown=throttlr:throttlr /app/build_cmake/bin/backend /usr/local/bin/backend
COPY --from=builder --chown=throttlr:throttlr /app/config/*.json /opt/throttlr/config/

COPY start.sh /opt/throttlr/start.sh
RUN chmod +x /opt/throttlr/start.sh

USER throttlr

EXPOSE 8080 8443 9001 9002 9003

ENTRYPOINT ["/opt/throttlr/start.sh"]
