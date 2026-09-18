# Opus

音频编码器，供 `core/audio` 的 Opus 编解码路径使用。

- 来源：`https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz`
- 版本：`1.5.2`
- 许可证：BSD 3-Clause（Xiph.Org / Skype Limited / Octasic 等）；原始许可证文件见上游源码包

目录中的 `include/` 是上游原样导出的公开头文件，不能手工修改。
静态库由同一份源码分别用目标 SDK 的工具链构建：

```text
lib/aarch64-rk3568/   # RK3568 Buildroot toolchain
lib/aarch64-rv1126b/  # RV1126B SDK toolchain
```

**用静态库而非共享库**：板端不必额外部署 `libopus.so.0`，也省掉 rpath 处理。
这与 `third_party/live555` 的做法一致。

源码不在仓库里，见 `~/third_party-src/opus/`；重新生成用 `tools/build-third-party.sh`。
