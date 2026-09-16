#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// 一张由字体渲染得到的单通道覆盖率图。
// alpha 的每个 byte 都对应一个像素：0=透明，255=字形完全覆盖。
// 它只处于 CPU 内存；需要给 RGA 使用时，由 OsdRenderer 写入自己的 RGBA DMA-BUF。
struct TextBitmap {
    int width = 0;
    int height = 0;
    int baseline = 0;
    std::vector<uint8_t> alpha;

    bool valid() const;
};

// 将 UTF-8 文本排版并栅格化为 TextBitmap 的轻量 FreeType 封装。
// 第一版支持常见的横排文字、中文、英文、数字与基础 kerning；阿拉伯语等复杂连写
// 以后若有需要，再在“UTF-8 -> glyph 序列”这层接入 HarfBuzz，不影响调用方。
class FreeTypeTextRenderer {
public:
    FreeTypeTextRenderer();
    ~FreeTypeTextRenderer();

    FreeTypeTextRenderer(const FreeTypeTextRenderer&) = delete;
    FreeTypeTextRenderer& operator=(const FreeTypeTextRenderer&) = delete;

    // 打开一份 TTF/OTF 字体，并指定目标像素字号。重复调用会安全关闭旧字体。
    bool open(const std::string& fontPath, unsigned pixelHeight);

    // 将一行 UTF-8 文本渲染到 output。output 自己持有结果，下一次 renderText() 不会影响它。
    bool renderText(const std::string& utf8Text, TextBitmap& output);

    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
