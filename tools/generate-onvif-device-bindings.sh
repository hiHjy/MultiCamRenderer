#!/usr/bin/env bash
# Generate one full C gSOAP binding shared by ONVIF Device Service, Media v1
# and WS-Discovery. The protocol definition is deliberately not hand-pruned.
set -euo pipefail

base_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tools_dir="${base_dir}/third_party/gsoap/tools/linux-x86_64"
gsoap_import_dir="${base_dir}/third_party/gsoap/import"
device_wsdl="${base_dir}/third_party/onvif-specs/wsdl/ver10/device/wsdl/devicemgmt.wsdl"
media_wsdl="${base_dir}/third_party/onvif-specs/wsdl/ver10/media/wsdl/media.wsdl"
media2_wsdl="${base_dir}/third_party/onvif-specs/wsdl/ver20/media/wsdl/media.wsdl"
output_dir="${base_dir}/generated/onvif/device"
interface_header="${output_dir}/onvif_device.h"

if [[ ! -x "${tools_dir}/wsdl2h" || ! -x "${tools_dir}/soapcpp2" ]]; then
    echo "gSOAP host generators are missing under ${tools_dir}" >&2
    exit 1
fi

mkdir -p "${output_dir}"

# Keep the official Device, Media v1 and Media2 WSDL surfaces intact. wsdl2h resolves the
# OASIS schema imports referenced by the upstream ONVIF schema on refresh.
"${tools_dir}/wsdl2h" -c -s -L -o "${interface_header}" \
    "${device_wsdl}" "${media_wsdl}" "${media2_wsdl}"

# One process must use one generated namespace table. Importing WS-Discovery
# here makes Device SOAP and UDP discovery use the same gSOAP serialization set.
sed -i '/#import "wsa5.h"/a #import "wsdd5.h"' "${interface_header}"

"${tools_dir}/soapcpp2" -c -a -L -x -ponvif \
    -I"${gsoap_import_dir}" \
    -d "${output_dir}" \
    "${interface_header}"

# Device/Media WSDL 本身不引用 ONVIF Discovery 的 Network 类型，所以 soapcpp2
# 不会把 "dn" 写进 namespace 表。但 WS-Discovery 的 Types 是 QName，响应里声明
# dn:NetworkVideoTransmitter 时必须输出对应 xmlns:dn，否则 ODM 会收到 UDP 包却拒绝
# 解析该 ProbeMatch。
sed -i '/{ "tds", "http:\/\/www.onvif.org\/ver10\/device\/wsdl"/i\        { "dn", "http://www.onvif.org/ver10/network/wsdl", NULL, NULL },' \
    "${output_dir}/DeviceBinding.nsmap"

# Every generated Device/Media operation must have a linkable handler. The few
# implemented operations live in OnvifSoapService.c; all remaining official
# operations return SOAP_NO_METHOD until implemented.
awk '
    BEGIN {
        implemented["__tds__GetServices"] = 1
        implemented["__tds__GetServiceCapabilities"] = 1
        implemented["__tds__GetDeviceInformation"] = 1
        implemented["__tds__GetSystemDateAndTime"] = 1
        implemented["__tds__GetScopes"] = 1
        implemented["__tds__GetCapabilities"] = 1
        implemented["__trt__GetServiceCapabilities"] = 1
        implemented["__trt__GetProfile"] = 1
        implemented["__trt__GetProfiles"] = 1
        implemented["__trt__GetStreamUri"] = 1
        implemented["__ns1__GetServiceCapabilities"] = 1
        implemented["__ns1__GetProfiles"] = 1
        implemented["__ns1__GetStreamUri"] = 1
        print "/* Generated fallback handlers for currently unsupported ONVIF Device/Media operations. */"
        print "#include \"stdsoap2.h\""
    }
    /^int __(tds|trt|tr2|ns1)__/ {
        name = $2
        sub(/\(.*/, "", name)
        if (!(name in implemented))
            printf "int %s() { return SOAP_NO_METHOD; }\n", name
    }
' "${interface_header}" > "${output_dir}/onvifUnimplementedOps.c"

echo "Generated full ONVIF Device + Media v1 + Media2 + WS-Discovery C bindings in ${output_dir}"
