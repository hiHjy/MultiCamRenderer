#!/bin/sh
# 在 RV1126B 板端执行：要求 multicam_ipc_app 已经启动。
# IPC App 收到 SIGUSR1 后录制未来约 30 秒的 raw PCM 与 AAC 编码包，
# 并把文件写到本脚本所在的 /root/audio-diagnostics/ 目录。

set -eu

app_pid=$(ps | grep '[m]ulticam_ipc_app' | awk 'NR == 1 { print $1 }')
if [ -z "$app_pid" ]; then
    echo "错误：没有找到正在运行的 multicam_ipc_app" >&2
    exit 1
fi

kill -USR1 "$app_pid"
echo "已向 multicam_ipc_app(pid=$app_pid) 请求一次 30 秒音频快照"
echo "完成后文件位于 /root/audio-diagnostics/；在 IPC App 日志中搜索 AudioDiagnosticCapture 可确认完成。"
