#!/usr/bin/env bash
set -euo pipefail

# 仅重新生成 WS-Discovery 1.0 binding。Device/Media 的 WSDL binding 将在下一阶段
# 按真正需要的 service 集合单独生成，避免现在把整套 ONVIF 类型塞入构建。
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GENERATOR="${BASE}/third_party/gsoap/tools/linux-x86_64/soapcpp2"
IMPORT_DIR="${BASE}/third_party/gsoap/import"
OUTPUT_DIR="${BASE}/generated/onvif/ws-discovery"

if [[ ! -x "${GENERATOR}" ]]; then
    echo "缺少 gSOAP host generator: ${GENERATOR}" >&2
    exit 1
fi

mkdir -p "${OUTPUT_DIR}"
"${GENERATOR}" -c -a -L -pwsdd -I"${IMPORT_DIR}" -d "${OUTPUT_DIR}" "${IMPORT_DIR}/wsdd5.h"

# wsdd5.h itself has no ONVIF Network namespace, but ONVIF Discovery uses the
# QName dn:NetworkVideoTransmitter in Probe Types.  Declare it in the generated
# namespace table so outgoing Probes are valid XML instead of an unbound prefix.
sed -i '/{ "wsdd", "http:\/\/schemas.xmlsoap.org\/ws\/2005\/04\/discovery"/a\        { "dn", "http://www.onvif.org/ver10/network/wsdl", NULL, NULL },' \
    "${OUTPUT_DIR}/wsdd.nsmap"
