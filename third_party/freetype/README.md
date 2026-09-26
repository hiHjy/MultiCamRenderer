# FreeType

这里存放 FreeType 的公开头文件，供项目的 `core/osd` 模块使用。

- 来源：RV1126B SDK `output/out/media_out/include/freetype2`
- 运行时动态库：RV1126B SDK `media_out/lib/libfreetype.so`
- 不把 FreeType 的实现或库复制进这里；交叉构建时由 SDK 的 `media_out` 链接。

`demo/osd/FreeTypeBitmapDemo.cpp` 是最小验证程序：从一个 TTF/OTF 字体把字符 `A` 渲染为
8-bit 灰度 bitmap（PGM 文件）。这份灰度数据未来可直接作为 RGBA OSD 图层的 alpha 通道。

字体文件不是本项目的第三方源码依赖。demo 通过命令行接收字体路径；量产镜像或部署包应
按产品授权要求单独提供字体文件。
