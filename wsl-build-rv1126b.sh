#!/usr/bin/env bash
set -euo pipefail

# RV1126B 的统一构建入口：IPC App 与 RV1126B 可运行的非 Qt demo 共用同一份 CMake 配置和 build 目录。

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ROOT="/home/hjy/2026-07-18/rv1126b_linux_ipc_xiaoyu"
MEDIA_ROOT="${SDK_ROOT}/output/out/media_out"
BUILD_DIR="${BASE}/build/rv1126b-aarch64"
TOOLCHAIN_FILE="${BASE}/cmake/rv1126b-aarch64-toolchain.cmake"
MODE="${1:-build}"

if [[ "${MODE}" == "clean" ]]; then
    rm -rf "${BUILD_DIR}"
fi

cmake -S "${BASE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMCR_LIVE555_TARGET=rv1126b \
    -DMCR_BUILD_DEMOS=ON \
    -DMCR_BUILD_QT=OFF \
    -DMCR_BUILD_IPC_APP=ON \
    -DMCR_BUILD_FREETYPE_BITMAP_DEMO=ON \
    -DMCR_MEDIA_ROOT="${MEDIA_ROOT}"

# RV1126B SDK 当前缺少 OpenSSL 开发头，带 TLS 的 live555 拉流 demo 无法在该 sysroot 下
# 编译；IPC 推流服务、OSD demo 与独立音频采集/Opus 编码 demo 不依赖该路径。
cmake --build "${BUILD_DIR}" --target ipc_app freetype_bitmap_demo audio_pipeline_demo audio_pipeline_stress_demo audio_frame_pool_demo audio_playback_pipeline_pcm_demo audio_playback_pipeline_opus_demo audio_capture_apm_pcm_demo audio_opus_playback_demo audio_apm_gain_demo audio_apm_aec_smoke_demo audio_mixer_inspect_demo --parallel "$(nproc)"

if [[ -x "${BASE}/tools/update_compile_commands.sh" ]]; then
    "${BASE}/tools/update_compile_commands.sh" || true
fi

echo "=== built RV1126B targets (ipc_app, freetype_bitmap_demo, audio_pipeline_demo, audio_pipeline_stress_demo, audio_frame_pool_demo, audio_playback_pipeline_pcm_demo, audio_playback_pipeline_opus_demo, audio_capture_apm_pcm_demo, audio_opus_playback_demo, audio_apm_gain_demo, audio_apm_aec_smoke_demo, audio_mixer_inspect_demo): ${BUILD_DIR} ==="
