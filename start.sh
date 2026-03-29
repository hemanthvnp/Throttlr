#!/bin/bash
echo "Starting backends..."
/usr/local/bin/backend 9001 &
/usr/local/bin/backend 9002 &
/usr/local/bin/backend 9003 &

echo "Starting Throttlr gateway..."
exec /usr/local/bin/gateway -c config/gateway.json
