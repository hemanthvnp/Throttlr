#!/bin/bash
echo "Starting backends..."
/usr/local/bin/backend 9001 &
/usr/local/bin/backend 9002 &
/usr/local/bin/backend 9003 &

CONFIG="${GATEWAY_CONFIG:-/opt/throttlr/config/local.json}"
echo "Starting Throttlr gateway with config: $CONFIG"
exec /usr/local/bin/gateway -c "$CONFIG"
