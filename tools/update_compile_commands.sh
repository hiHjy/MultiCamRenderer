#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_FILE="$ROOT_DIR/compile_commands.json"

inputs=()

qt_build_dirs=()
if grep -qi microsoft /proc/version 2>/dev/null; then
    qt_build_dirs=(
        "$ROOT_DIR/qt-demo/build-wsl-aarch64"
        "$ROOT_DIR/qt-demo/build"
    )
else
    qt_build_dirs=(
        "$ROOT_DIR/qt-demo/build"
        "$ROOT_DIR/qt-demo/build-wsl-aarch64"
    )
fi

for dir in "${qt_build_dirs[@]}"; do
    if [ -f "$dir/compile_commands.json" ]; then
        inputs+=("$dir/compile_commands.json")
    fi
done

if [ -f "$ROOT_DIR/compile_commands.local.json" ]; then
    inputs+=("$ROOT_DIR/compile_commands.local.json")
fi

if [ -f "$ROOT_DIR/compile_commands.base.json" ]; then
    inputs+=("$ROOT_DIR/compile_commands.base.json")
fi

if [ "${#inputs[@]}" -eq 0 ]; then
    echo "没有找到可合并的 compile_commands.json" >&2
    exit 1
fi

node - "$OUT_FILE" "${inputs[@]}" <<'NODE'
const fs = require("fs");

const out = process.argv[2];
const inputs = process.argv.slice(3);
const merged = [];
const seen = new Set();

for (const input of inputs) {
  let data;
  try {
    data = JSON.parse(fs.readFileSync(input, "utf8"));
  } catch (err) {
    console.error(`读取失败: ${input}: ${err.message}`);
    process.exit(1);
  }

  for (const entry of data) {
    if (!entry || !entry.file) continue;
    const key = entry.file;
    if (seen.has(key)) continue;
    seen.add(key);
    merged.push(entry);
  }
}

fs.writeFileSync(out, JSON.stringify(merged, null, 2) + "\n");
console.log(`merged ${merged.length} entries -> ${out}`);
NODE
