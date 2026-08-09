#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
g++ -std=c++17 -O2 -Wall -Wextra -pthread main.cpp -o frame_lease_demo
echo "built: $(pwd)/frame_lease_demo"
