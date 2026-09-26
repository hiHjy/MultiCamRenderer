#!/usr/bin/env python3
"""
ONVIF Multi-Stream Discovery and Compatibility Test Tool
Tests WS-Discovery, Device Service, Media v1 (Profile S) and Media2 (Profile T).
"""

import sys
import uuid
import socket
import argparse
import xml.etree.ElementTree as ET
import urllib.request
import urllib.error

# XML Namespaces
NS = {
    "soap": "http://www.w3.org/2003/05/soap-envelope",
    "soap11": "http://schemas.xmlsoap.org/soap/envelope/",
    "wsa": "http://www.w3.org/2005/08/addressing",
    "wsdd": "http://schemas.xmlsoap.org/ws/2005/04/discovery",
    "tds": "http://www.onvif.org/ver10/device/wsdl",
    "trt": "http://www.onvif.org/ver10/media/wsdl",
    "tr2": "http://www.onvif.org/ver20/media/wsdl",
    "tt": "http://www.onvif.org/ver10/schema",
}

def post_soap(url, action, body_xml, timeout=5):
    envelope = f"""<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:tds="http://www.onvif.org/ver10/device/wsdl"
               xmlns:trt="http://www.onvif.org/ver10/media/wsdl"
               xmlns:tr2="http://www.onvif.org/ver20/media/wsdl"
               xmlns:tt="http://www.onvif.org/ver10/schema">
  <soap:Body>
    {body_xml}
  </soap:Body>
</soap:Envelope>"""
    headers = {
        "Content-Type": f'application/soap+xml; charset=utf-8; action="{action}"',
    }
    req = urllib.request.Request(url, data=envelope.encode("utf-8"), headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
            return ET.fromstring(data)
    except urllib.error.HTTPError as e:
        err_data = e.read()
        try:
            return ET.fromstring(err_data)
        except Exception:
            raise RuntimeError(f"HTTP {e.code}: {err_data.decode('utf-8', errors='replace')}")

def test_probe(device_ip, port=3702, timeout=2.5):
    print(f"\n[1] 正在测试 WS-Discovery Probe (UDP {device_ip}:{port})...")
    msg_id = f"urn:uuid:{uuid.uuid4()}"
    probe_xml = f"""<?xml version="1.0" encoding="utf-8"?>
<soap:Envelope xmlns:soap="http://www.w3.org/2003/05/soap-envelope"
               xmlns:wsa="http://www.w3.org/2005/08/addressing"
               xmlns:wsdd="http://schemas.xmlsoap.org/ws/2005/04/discovery"
               xmlns:dn="http://www.onvif.org/ver10/network/wsdl">
  <soap:Header>
    <wsa:MessageID>{msg_id}</wsa:MessageID>
    <wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>
    <wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>
  </soap:Header>
  <soap:Body>
    <wsdd:Probe>
      <wsdd:Types>dn:NetworkVideoTransmitter</wsdd:Types>
    </wsdd:Probe>
  </soap:Body>
</soap:Envelope>"""

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(probe_xml.encode("utf-8"), (device_ip, port))
        data, addr = sock.recvfrom(65535)
        root = ET.fromstring(data)
        matches = root.findall(".//wsdd:ProbeMatches/wsdd:ProbeMatch", NS)
        if not matches:
            # Fallback if namespace prefix in response differs
            matches = [elem for elem in root.iter() if elem.tag.endswith("ProbeMatch")]
        print(f"  -> 收到 ProbeMatches 来自 {addr}, 匹配数: {len(matches)}")
        xaddrs = []
        for m in matches:
            xaddr_elem = m.find("wsdd:XAddrs", NS)
            if xaddr_elem is None:
                xaddr_elem = [e for e in m.iter() if e.tag.endswith("XAddrs")][0]
            if xaddr_elem is not None and xaddr_elem.text:
                xaddrs.extend(xaddr_elem.text.split())
        print(f"  -> 设备服务地址 (XAddrs): {xaddrs}")
        return True, xaddrs[0] if xaddrs else None
    except socket.timeout:
        print("  -> Probe 超时 (未收到响应)")
        return False, None
    finally:
        sock.close()

def test_device_service(device_url):
    print(f"\n[2] 正在测试 Device Service ({device_url})...")
    # GetDeviceInformation
    root = post_soap(device_url, "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
                     "<tds:GetDeviceInformation/>")
    mfr = root.find(".//tds:Manufacturer", NS)
    model = root.find(".//tds:Model", NS)
    fw = root.find(".//tds:FirmwareVersion", NS)
    sn = root.find(".//tds:SerialNumber", NS)
    print(f"  -> Manufacturer: {mfr.text if mfr is not None else 'N/A'}")
    print(f"  -> Model       : {model.text if model is not None else 'N/A'}")
    print(f"  -> Firmware    : {fw.text if fw is not None else 'N/A'}")
    print(f"  -> SerialNumber: {sn.text if sn is not None else 'N/A'}")

    # GetServices
    root = post_soap(device_url, "http://www.onvif.org/ver10/device/wsdl/GetServices",
                     "<tds:GetServices><tds:IncludeCapability>false</tds:IncludeCapability></tds:GetServices>")
    services = root.findall(".//tds:Service", NS)
    print(f"  -> 发现已宣告服务数量: {len(services)}")
    media1_url = None
    media2_url = None
    for s in services:
        ns_elem = s.find("tds:Namespace", NS)
        xaddr_elem = s.find("tds:XAddr", NS)
        ver_elem = s.find("tds:Version", NS)
        ver_str = ""
        if ver_elem is not None:
            major = ver_elem.find("tt:Major", NS)
            minor = ver_elem.find("tt:Minor", NS)
            if major is not None and minor is not None:
                ver_str = f"v{major.text}.{minor.text}"
        ns_text = ns_elem.text if ns_elem is not None else ""
        xaddr_text = xaddr_elem.text if xaddr_elem is not None else ""
        print(f"     - 命名空间: {ns_text:<42} 版本: {ver_str:<6} XAddr: {xaddr_text}")
        if "ver10/media" in ns_text:
            media1_url = xaddr_text
        elif "ver20/media" in ns_text:
            media2_url = xaddr_text

    return media1_url, media2_url

def test_media1_profiles(media_url):
    print(f"\n[3] 正在测试 Media v1 (Profile S / ver10) ({media_url})...")
    root = post_soap(media_url, "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                     "<trt:GetProfiles/>")
    fault = root.find(".//soap:Fault", NS)
    if fault is not None:
        reason = root.find(".//soap:Reason/soap:Text", NS)
        print(f"  [ERROR] Media1 GetProfiles 报错: {reason.text if reason is not None else 'Unknown fault'}")
        return False, []

    profiles = root.findall(".//trt:Profiles", NS)
    print(f"  -> 发现 Profiles 数量: {len(profiles)}")
    found_profiles = []
    for p in profiles:
        token = p.attrib.get("token", "")
        name = p.find("tt:Name", NS)
        name_str = name.text if name is not None else ""
        v_enc = p.find("tt:VideoEncoderConfiguration", NS)
        a_enc = p.find("tt:AudioEncoderConfiguration", NS)

        v_info = "无视频配置 (客户端会当成纯音频过滤!)"
        v_ok = False
        if v_enc is not None:
            enc_type = v_enc.find("tt:Encoding", NS)
            res = v_enc.find("tt:Resolution", NS)
            w = res.find("tt:Width", NS).text if res is not None and res.find("tt:Width", NS) is not None else "?"
            h = res.find("tt:Height", NS).text if res is not None and res.find("tt:Height", NS) is not None else "?"
            v_info = f"视频编码: {enc_type.text if enc_type is not None else '?'} 分辨率: {w}x{h}"
            v_ok = True

        a_info = "无音频配置"
        if a_enc is not None:
            a_codec = a_enc.find("tt:Encoding", NS)
            a_br = a_enc.find("tt:Bitrate", NS)
            a_sr = a_enc.find("tt:SampleRate", NS)
            a_info = f"音频: {a_codec.text if a_codec is not None else '?'} {a_sr.text if a_sr is not None else '?'}kHz {a_br.text if a_br is not None else '?'}kbps"

        # GetStreamUri
        uri_root = post_soap(media_url, "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                             f"""<trt:GetStreamUri>
                                   <trt:StreamSetup>
                                     <tt:Stream>RTP-Unicast</tt:Stream>
                                     <tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>
                                   </trt:StreamSetup>
                                   <trt:ProfileToken>{token}</trt:ProfileToken>
                                 </trt:GetStreamUri>""")
        uri_elem = uri_root.find(".//tt:Uri", NS)
        rtsp_uri = uri_elem.text if uri_elem is not None else "获取失败"

        status_tag = "[OK 正常发现]" if v_ok else "[FAIL 缺失视频]"
        print(f"     {status_tag} Token: {token:<6} 名称: {name_str:<22} | {v_info} | {a_info}")
        print(f"            RTSP URI: {rtsp_uri}")
        found_profiles.append({"token": token, "has_video": v_ok, "uri": rtsp_uri})

    return True, found_profiles

def test_media2_profiles(media2_url):
    print(f"\n[4] 正在测试 Media2 (Profile T / ver20) ({media2_url})...")
    # GetServiceCapabilities
    root = post_soap(media2_url, "http://www.onvif.org/ver20/media/wsdl/GetServiceCapabilities",
                     "<tr2:GetServiceCapabilities/>")
    fault = root.find(".//soap:Fault", NS)
    if fault is not None:
        reason = root.find(".//soap:Reason/soap:Text", NS)
        print(f"  [ERROR] Media2 GetServiceCapabilities 报错: {reason.text if reason is not None else 'Unknown'}")
        return False, []
    else:
        print("  -> Media2 GetServiceCapabilities 响应正常")

    # GetProfiles
    root = post_soap(media2_url, "http://www.onvif.org/ver20/media/wsdl/GetProfiles",
                     "<tr2:GetProfiles><tr2:Type>All</tr2:Type></tr2:GetProfiles>")
    fault = root.find(".//soap:Fault", NS)
    if fault is not None:
        reason = root.find(".//soap:Reason/soap:Text", NS)
        print(f"  [ERROR] Media2 GetProfiles 报错: {reason.text if reason is not None else 'Unknown'}")
        return False, []

    profiles = root.findall(".//tr2:Profiles", NS)
    print(f"  -> 发现 Profiles 数量: {len(profiles)}")
    found_profiles = []
    for p in profiles:
        token = p.attrib.get("token", "")
        name = p.find("tr2:Name", NS)
        name_str = name.text if name is not None else ""
        configs = p.find("tr2:Configurations", NS)
        v_enc = configs.find("tr2:VideoEncoder", NS) if configs is not None else None
        a_enc = configs.find("tr2:AudioEncoder", NS) if configs is not None else None

        v_info = "无视频配置"
        v_ok = False
        codec_name = ""
        if v_enc is not None:
            enc_type = v_enc.find("tt:Encoding", NS)
            codec_name = enc_type.text if enc_type is not None else ""
            res = v_enc.find("tt:Resolution", NS)
            w = res.find("tt:Width", NS).text if res is not None and res.find("tt:Width", NS) is not None else "?"
            h = res.find("tt:Height", NS).text if res is not None and res.find("tt:Height", NS) is not None else "?"
            v_info = f"原生编码: {codec_name} 分辨率: {w}x{h}"
            v_ok = True

        a_info = "无音频配置"
        if a_enc is not None:
            a_codec = a_enc.find("tt:Encoding", NS)
            a_br = a_enc.find("tt:Bitrate", NS)
            a_sr = a_enc.find("tt:SampleRate", NS)
            a_info = f"音频: {a_codec.text if a_codec is not None else '?'} {a_sr.text if a_sr is not None else '?'}kHz {a_br.text if a_br is not None else '?'}kbps"

        # GetStreamUri
        uri_root = post_soap(media2_url, "http://www.onvif.org/ver20/media/wsdl/GetStreamUri",
                             f"""<tr2:GetStreamUri>
                                   <tr2:Protocol>RTSP</tr2:Protocol>
                                   <tr2:ProfileToken>{token}</tr2:ProfileToken>
                                 </tr2:GetStreamUri>""")
        uri_elem = uri_root.find(".//tr2:Uri", NS)
        rtsp_uri = uri_elem.text if uri_elem is not None else "获取失败"

        status_tag = f"[OK {codec_name}]" if v_ok else "[FAIL 缺失视频]"
        print(f"     {status_tag} Token: {token:<6} 名称: {name_str:<22} | {v_info} | {a_info}")
        print(f"            RTSP URI: {rtsp_uri}")
        found_profiles.append({"token": token, "has_video": v_ok, "codec": codec_name, "uri": rtsp_uri})

    return True, found_profiles

def main():
    parser = argparse.ArgumentParser(description="ONVIF Multi-Stream Discovery Test Tool")
    parser.add_argument("--ip", default="192.168.1.4", help="Camera IP address (default: 192.168.1.4)")
    parser.add_argument("--port", type=int, default=8899, help="ONVIF HTTP port (default: 8899)")
    args = parser.parse_args()

    print("=================================================================")
    print(f"   ONVIF 多码流发现诊断测试工具 (目标设备: {args.ip}:{args.port})")
    print("=================================================================")

    # 1. Probe
    test_probe(args.ip)

    # 2. Device Service
    device_url = f"http://{args.ip}:{args.port}/onvif/device_service"
    try:
        media1_url, media2_url = test_device_service(device_url)
    except Exception as e:
        print(f"  [ERROR] 连接 Device Service 失败: {e}")
        return 1

    if not media1_url:
        media1_url = f"http://{args.ip}:{args.port}/onvif/media_service"
    if not media2_url:
        media2_url = f"http://{args.ip}:{args.port}/onvif/media2_service"

    # 3. Media1
    m1_ok, m1_profiles = test_media1_profiles(media1_url)

    # 4. Media2
    m2_ok, m2_profiles = test_media2_profiles(media2_url)

    # Summary
    print("\n========================= 兼容性测试结论 =========================")
    m1_tokens = {p["token"]: p["has_video"] for p in m1_profiles}
    m2_tokens = {p["token"]: p.get("codec", "") for p in m2_profiles}

    print(f"1. Media v1 (旧版手机 App / 传统 NVR / Profile S):")
    print(f"   - 主码流 (main): {'[PASS 正常被识别为视频码流]' if m1_tokens.get('main') else '[FAIL 仍被作为音频流忽略]'}")
    print(f"   - 子码流 (sub) : {'[PASS 正常被识别为视频码流]' if m1_tokens.get('sub') else '[FAIL 缺失]'}")

    print(f"2. Media2 (现代 ONVIF Profile T 规范):")
    main_codec = m2_tokens.get('main', '')
    sub_codec = m2_tokens.get('sub', '')
    print(f"   - 主码流 (main): {'[PASS 声明为原生 ' + main_codec + ']' if main_codec == 'H265' else ('[FAIL 未能发现或不是H265]' if main_codec else '[未实现]')}")
    print(f"   - 子码流 (sub) : {'[PASS 声明为原生 ' + sub_codec + ']' if sub_codec == 'H264' else ('[FAIL 未能发现或不是H264]' if sub_codec else '[未实现]')}")
    print("=================================================================\n")

if __name__ == "__main__":
    sys.exit(main() or 0)
