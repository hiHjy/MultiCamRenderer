#!/usr/bin/env bash
#
# 重新生成 third_party 下 opus 与 webrtc-audio-processing 的预编译库。
#
# 这两个依赖以"头文件 + 预编译静态库"的形式入库（同 third_party/live555），
# 源码放在 $THIRD_PARTY_SRC_ROOT（默认 ~/third_party-src）下，不在仓库里。
#
# 什么时候需要跑：
#   - 升级 opus / webrtc-audio-processing / abseil 版本
#   - 改了 webrtc-audio-processing/CMakeLists.txt 里的编译选项或源码筛选规则
#   - 新增目标板架构
#
# 跑完之后把 third_party 下的变更一起提交即可。
#
set -euo pipefail

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_ROOT="${THIRD_PARTY_SRC_ROOT:-$HOME/third_party-src}"
BUILD_ROOT="${THIRD_PARTY_BUILD_ROOT:-/tmp/mcr-third-party-build}"
ARCHES=(rv1126b rk3568)

log() { printf '\n=== %s ===\n' "$*"; }

# ---------------------------------------------------------------------------
# 前置检查
# ---------------------------------------------------------------------------
for dir in opus abseil-cpp webrtc-audio-processing; do
    if [[ ! -d "${SRC_ROOT}/${dir}" ]]; then
        echo "缺少源码 ${SRC_ROOT}/${dir}" >&2
        echo "把 opus / abseil-cpp / webrtc-audio-processing 的源码放到该目录下再跑。" >&2
        exit 1
    fi
done

if [[ ! -f "${SRC_ROOT}/webrtc-audio-processing/CMakeLists.txt" ]]; then
    echo "缺少 ${SRC_ROOT}/webrtc-audio-processing/CMakeLists.txt（从源码构建的配方）" >&2
    exit 1
fi

for arch in "${ARCHES[@]}"; do
    toolchain="${BASE}/cmake/${arch}-aarch64-toolchain.cmake"
    if [[ ! -f "${toolchain}" ]]; then
        echo "缺少工具链文件 ${toolchain}" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# 编译
# ---------------------------------------------------------------------------
for arch in "${ARCHES[@]}"; do
    toolchain="${BASE}/cmake/${arch}-aarch64-toolchain.cmake"

    log "opus  ${arch}"
    rm -rf "${BUILD_ROOT}/opus-${arch}"
    cmake -S "${SRC_ROOT}/opus" -B "${BUILD_ROOT}/opus-${arch}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPUS_BUILD_SHARED_LIBRARY=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF \
        -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_BUILD_DOCS=OFF >/dev/null
    cmake --build "${BUILD_ROOT}/opus-${arch}" --target opus --parallel "$(nproc)"

    log "webrtc-audio-processing + abseil  ${arch}"
    rm -rf "${BUILD_ROOT}/webrtc-${arch}"
    cmake -S "${SRC_ROOT}/webrtc-audio-processing" -B "${BUILD_ROOT}/webrtc-${arch}" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
        -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "${BUILD_ROOT}/webrtc-${arch}" --target mcr_webrtc_apm --parallel "$(nproc)"
done

# ---------------------------------------------------------------------------
# 安装：库
# ---------------------------------------------------------------------------
for arch in "${ARCHES[@]}"; do
    for pkg in opus webrtc-audio-processing; do
        mkdir -p "${BASE}/third_party/${pkg}/lib/aarch64-${arch}"
    done
    cp "${BUILD_ROOT}/opus-${arch}/libopus.a" \
       "${BASE}/third_party/opus/lib/aarch64-${arch}/"
    cp "${BUILD_ROOT}/webrtc-${arch}/libmcr_webrtc_apm.a" \
       "${BASE}/third_party/webrtc-audio-processing/lib/aarch64-${arch}/"
done

# ---------------------------------------------------------------------------
# 安装：头文件
#
# webrtc 的头文件有 300 多个、abseil 有 350 多个，但项目只用到其中很小一部分。
# 这里从 AudioApm.cpp 实际 include 的入口出发，递归求出传递闭包，只落盘真正需要的
# 那些（实测 600 多个里只要 52 个），仓库里就不会堆满用不上的上游头文件。
# 以后如果项目 include 了新的 webrtc 头，重跑本脚本即可自动带上。
# ---------------------------------------------------------------------------
log "生成头文件（传递闭包）"
python3 - "$SRC_ROOT" "${BASE}/third_party/webrtc-audio-processing/include" <<'PYEOF'
import os, re, shutil, sys

src_root, dst = sys.argv[1], sys.argv[2]
W = os.path.join(src_root, 'webrtc-audio-processing', 'webrtc')
A = os.path.join(src_root, 'abseil-cpp')

ENTRY_POINTS = ['api/audio/audio_processing.h', 'api/scoped_refptr.h']
SEARCH_DIRS = [W, A, os.path.join(W, 'modules', 'audio_processing', 'include')]

inc_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
seen, queue, missing = set(), [os.path.join(W, e) for e in ENTRY_POINTS], set()

while queue:
    path = os.path.normpath(queue.pop())
    if path in seen:
        continue
    if not os.path.exists(path):
        missing.add(path)
        continue
    seen.add(path)
    with open(path, encoding='utf-8', errors='ignore') as fp:
        text = fp.read()
    for name in inc_re.findall(text):
        for base in [os.path.dirname(path)] + SEARCH_DIRS:
            candidate = os.path.normpath(os.path.join(base, name))
            if os.path.exists(candidate):
                queue.append(candidate)
                break

shutil.rmtree(dst, ignore_errors=True)
count = 0
for path in sorted(seen):
    if path.startswith(W + os.sep):
        rel = os.path.relpath(path, W)          # api/... rtc_base/... absl/...
    elif path.startswith(A + os.sep):
        rel = os.path.relpath(path, A)
    else:
        continue
    out = os.path.join(dst, rel)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.copy2(path, out)
    count += 1

print(f"  拷入 {count} 个头文件")
if missing:
    print(f"  注意：有 {len(missing)} 个被引用但未找到的文件（通常是被平台宏包住的，可忽略）")
PYEOF

# opus 的头很少，直接全拷
mkdir -p "${BASE}/third_party/opus/include"
cp "${SRC_ROOT}"/opus/include/*.h "${BASE}/third_party/opus/include/"

# ---------------------------------------------------------------------------
log "完成"
du -sh "${BASE}/third_party/opus" "${BASE}/third_party/webrtc-audio-processing"
echo
echo "third_party 下的变更已就绪，确认无误后提交即可。"
