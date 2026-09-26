# gSOAP 2.8.144

- Upstream: https://sourceforge.net/projects/gsoap2/
- Version: `gsoap_2.8.144.zip` (2026-07-26)
- License: GPL-2.0-only. See [LICENSE](LICENSE).

This directory intentionally keeps only the cross-compiled runtime sources, WS-Discovery
plugin/import headers, and WSL x86_64 binding generators needed by this project. It does not
copy the upstream examples or documentation tree.

`tools/generate-onvif-wsdd.sh` regenerates the committed WS-Discovery binding under
`generated/onvif/ws-discovery/`.
