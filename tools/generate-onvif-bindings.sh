#!/usr/bin/env bash
# 一键重新生成本项目全部 ONVIF gSOAP binding。
#
# 仅在 WSL/x86_64 开发主机运行：它调用项目随附的 wsdl2h、soapcpp2 host 工具，
# 不在 RK3568/RV1126B 板端运行，也不参与板端运行期。
set -euo pipefail

base_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

"${base_dir}/tools/generate-onvif-wsdd.sh"
"${base_dir}/tools/generate-onvif-device-bindings.sh"

echo "ONVIF gSOAP bindings regenerated under ${base_dir}/generated/onvif"
