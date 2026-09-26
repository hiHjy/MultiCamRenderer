#!/bin/sh
#
# 板端 APM 听感 A/B：同一次麦克风采集同时保存原始 PCM 和 APM 处理后的 PCM，
# 然后严格按“原始 -> APM”顺序播放。这里刻意不走 Opus，避免编码器把比较变量混进来。
#
# 用法：
#   sh demo/AudioApmAbTest.sh [录音秒数] [输出目录]
#
# 例：
#   sh /root/nfs/MultiCamRenderer/demo/AudioApmAbTest.sh 12 /tmp/apm-ab
#
# 可用 MCR_AUDIO_BUILD_DIR 覆盖可执行文件目录；默认是 RV1126B 统一构建目录。

set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "${SCRIPT_DIR}/.." && pwd)
BUILD_DIR=${MCR_AUDIO_BUILD_DIR:-"${PROJECT_ROOT}/build/rv1126b-aarch64"}
RECORD_SECONDS=${1:-10}
OUTPUT_DIR=${2:-/tmp/mcr-audio-apm-ab}
RAW_WAV="${OUTPUT_DIR}/01-raw.wav"
APM_WAV="${OUTPUT_DIR}/02-apm.wav"

if [ ! -x "${BUILD_DIR}/audio_capture_apm_pcm_demo" ]; then
    echo "找不到 audio_capture_apm_pcm_demo: ${BUILD_DIR}" >&2
    echo "请先在工程根目录执行 ./wsl-build-rv1126b.sh" >&2
    exit 1
fi

case "${RECORD_SECONDS}" in
    ''|*[!0-9]*)
        echo "录音秒数必须是正整数，当前值: ${RECORD_SECONDS}" >&2
        exit 1
        ;;
esac
if [ "${RECORD_SECONDS}" -le 0 ]; then
    echo "录音秒数必须大于 0" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
rm -f "${RAW_WAV}" "${APM_WAV}"

echo "=== 开始录音 ${RECORD_SECONDS}s：请持续说话，录音来自同一份麦克风 PCM ==="
# 参数顺序：处理后 WAV、录音秒数、固定增益 dB、原始 WAV。
# 固定增益 0dB；默认 APM 仍启用高通和 High 降噪。
LD_LIBRARY_PATH=/oem/usr/lib "${BUILD_DIR}/audio_capture_apm_pcm_demo" \
    "${APM_WAV}" "${RECORD_SECONDS}" 0 "${RAW_WAV}"

echo
echo "=== A：播放原始 PCM（未经过 APM） ==="
aplay "${RAW_WAV}"

echo
echo "=== B：播放 APM PCM（高通 + 降噪，固定增益 0dB） ==="
aplay "${APM_WAV}"

echo
echo "A/B 文件已保留："
echo "  raw: ${RAW_WAV}"
echo "  apm: ${APM_WAV}"
