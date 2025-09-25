#!/bin/bash

# This script will start the terminal server in the background

cd "$(dirname "$0")"

./wangemu-terminal-server --web-config &

echo "Wang 2200 Terminal Server started with web config (PID: $!)"
echo "To stop, use: pkill wangemu-terminal-server"