# live555（统一依赖）

本目录只使用一套未经项目修改的 upstream 源码：`live.2021.05.03`。

- 来源：`https://github.com/lengfeld/live555-unofficial-git-archive`
- 固定 tag：`v2021.05.03-tree`
- 与 RK3568 板子原有 `libliveMedia.so.94` 的版本一致。
- 许可证：LGPL；原始许可证文件见上游源码包。

目录中的 `include/` 是该源码包原样导出的头文件，不能手工修改。
静态库以同一源码分别使用目标 SDK 的工具链构建：

```text
lib/aarch64-rk3568/   # RK3568 Buildroot toolchain
lib/aarch64-rv1126b/  # RV1126B SDK toolchain
```

构建脚本会显式传入 `MCR_LIVE555_TARGET`，因此不会再发生“新头文件配旧静态库”的混搭。
