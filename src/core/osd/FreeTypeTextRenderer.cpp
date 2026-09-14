#include "FreeTypeTextRenderer.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

struct Utf8CodePoint {
    uint32_t value = 0;
};

bool decodeUtf8(const std::string& text,
                std::vector<Utf8CodePoint>& output,
                std::string& error)
{
    output.clear();
    for (size_t index = 0; index < text.size();) {
        const uint8_t first = static_cast<uint8_t>(text[index]);
        uint32_t codePoint = 0;
        size_t byteCount = 0;
        uint32_t minimumValue = 0;

        if ((first & 0x80U) == 0) {
            codePoint = first;
            byteCount = 1;
        } else if ((first & 0xE0U) == 0xC0U) {
            codePoint = first & 0x1FU;
            byteCount = 2;
            minimumValue = 0x80;
        } else if ((first & 0xF0U) == 0xE0U) {
            codePoint = first & 0x0FU;
            byteCount = 3;
            minimumValue = 0x800;
        } else if ((first & 0xF8U) == 0xF0U) {
            codePoint = first & 0x07U;
            byteCount = 4;
            minimumValue = 0x10000;
        } else {
            error = "UTF-8 首字节无效";
            return false;
        }

        if (index + byteCount > text.size()) {
            error = "UTF-8 字符不完整";
            return false;
        }
        for (size_t offset = 1; offset < byteCount; ++offset) {
            const uint8_t next = static_cast<uint8_t>(text[index + offset]);
            if ((next & 0xC0U) != 0x80U) {
                error = "UTF-8 连续字节无效";
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }

        if (codePoint < minimumValue || codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            error = "UTF-8 code point 无效";
            return false;
        }
        if (codePoint == '\n' || codePoint == '\r') {
            error = "第一版 TextBitmap 只支持单行文本";
            return false;
        }

        output.push_back(Utf8CodePoint {codePoint});
        index += byteCount;
    }
    return true;
}

int fromFixed26_6(FT_Pos value)
{
    return value >= 0 ? static_cast<int>((value + 32) >> 6)
                      : -static_cast<int>((-value + 32) >> 6);
}

} // namespace

bool TextBitmap::valid() const
{
    return width > 0 && height > 0 &&
           alpha.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
}

struct FreeTypeTextRenderer::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::string lastError;

    ~Impl()
    {
        if (face != nullptr) {
            FT_Done_Face(face);
        }
        if (library != nullptr) {
            FT_Done_FreeType(library);
        }
    }

    bool ensureLibrary()
    {
        if (library != nullptr) {
            return true;
        }
        const FT_Error error = FT_Init_FreeType(&library);
        if (error != 0) {
            lastError = "FT_Init_FreeType 失败，错误码=" + std::to_string(error);
            return false;
        }
        return true;
    }

    bool loadGlyph(FT_UInt glyphIndex)
    {
        FT_Error error = FT_Load_Glyph(face, glyphIndex, FT_LOAD_DEFAULT);
        if (error == 0) {
            error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
        }
        if (error != 0) {
            lastError = "FreeType 加载或栅格化字形失败，错误码=" + std::to_string(error);
            return false;
        }
        if (face->glyph->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
            lastError = "当前字体输出不是 8-bit 灰度 bitmap";
            return false;
        }
        return true;
    }

    FT_Pos kerning(FT_UInt previous, FT_UInt current) const
    {
        if (previous == 0 || current == 0 || !FT_HAS_KERNING(face)) {
            return 0;
        }
        FT_Vector value {};
        if (FT_Get_Kerning(face, previous, current, FT_KERNING_DEFAULT, &value) != 0) {
            return 0;
        }
        return value.x;
    }
};

FreeTypeTextRenderer::FreeTypeTextRenderer()
    : m_impl(std::make_unique<Impl>())
{
}

FreeTypeTextRenderer::~FreeTypeTextRenderer() = default;

bool FreeTypeTextRenderer::open(const std::string& fontPath, unsigned pixelHeight)
{
    if (fontPath.empty() || pixelHeight == 0) {
        m_impl->lastError = "字体路径为空或字号为 0";
        return false;
    }
    if (!m_impl->ensureLibrary()) {
        return false;
    }
    if (m_impl->face != nullptr) {
        FT_Done_Face(m_impl->face);
        m_impl->face = nullptr;
    }

    FT_Error error = FT_New_Face(m_impl->library, fontPath.c_str(), 0, &m_impl->face);
    if (error == 0) {
        error = FT_Set_Pixel_Sizes(m_impl->face, 0, pixelHeight);
    }
    if (error != 0) {
        m_impl->lastError = "打开字体或设置字号失败，错误码=" + std::to_string(error) +
                            " path=" + fontPath;
        if (m_impl->face != nullptr) {
            FT_Done_Face(m_impl->face);
            m_impl->face = nullptr;
        }
        return false;
    }

    m_impl->lastError.clear();
    return true;
}

bool FreeTypeTextRenderer::renderText(const std::string& utf8Text, TextBitmap& output)
{
    output = {};
    if (m_impl->face == nullptr) {
        m_impl->lastError = "尚未打开字体";
        return false;
    }

    std::vector<Utf8CodePoint> codePoints;
    if (!decodeUtf8(utf8Text, codePoints, m_impl->lastError) || codePoints.empty()) {
        if (codePoints.empty() && m_impl->lastError.empty()) {
            m_impl->lastError = "文本为空";
        }
        return false;
    }

    const int baseline = fromFixed26_6(m_impl->face->size->metrics.ascender);
    const int descender = fromFixed26_6(m_impl->face->size->metrics.descender);
    const int canvasHeight = std::max(1, baseline - descender);

    FT_Pos penX = 0;
    FT_UInt previousGlyph = 0;
    int minX = 0;
    int maxX = 0;
    for (const Utf8CodePoint& codePoint : codePoints) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(m_impl->face, codePoint.value);
        penX += m_impl->kerning(previousGlyph, glyphIndex);
        if (!m_impl->loadGlyph(glyphIndex)) {
            return false;
        }

        const FT_GlyphSlot glyph = m_impl->face->glyph;
        const int glyphLeft = fromFixed26_6(penX) + glyph->bitmap_left;
        const int glyphRight = glyphLeft + static_cast<int>(glyph->bitmap.width);
        minX = std::min(minX, glyphLeft);
        maxX = std::max(maxX, glyphRight);
        penX += glyph->advance.x;
        maxX = std::max(maxX, fromFixed26_6(penX));
        previousGlyph = glyphIndex;
    }

    const int canvasWidth = maxX - minX;
    if (canvasWidth <= 0 || canvasHeight <= 0) {
        m_impl->lastError = "文字没有可绘制区域";
        return false;
    }

    output.width = canvasWidth;
    output.height = canvasHeight;
    output.baseline = baseline;
    output.alpha.assign(static_cast<size_t>(canvasWidth) * static_cast<size_t>(canvasHeight), 0);

    penX = 0;
    previousGlyph = 0;
    for (const Utf8CodePoint& codePoint : codePoints) {
        const FT_UInt glyphIndex = FT_Get_Char_Index(m_impl->face, codePoint.value);
        penX += m_impl->kerning(previousGlyph, glyphIndex);
        if (!m_impl->loadGlyph(glyphIndex)) {
            output = {};
            return false;
        }

        const FT_GlyphSlot glyph = m_impl->face->glyph;
        const FT_Bitmap& bitmap = glyph->bitmap;
        const int dstLeft = fromFixed26_6(penX) + glyph->bitmap_left - minX;
        const int dstTop = baseline - glyph->bitmap_top;
        const int pitch = bitmap.pitch;

        for (unsigned sourceY = 0; sourceY < bitmap.rows; ++sourceY) {
            const unsigned char* sourceRow = pitch >= 0
                ? bitmap.buffer + static_cast<size_t>(sourceY) * static_cast<size_t>(pitch)
                : bitmap.buffer + static_cast<size_t>(bitmap.rows - 1 - sourceY) * static_cast<size_t>(-pitch);
            const int dstY = dstTop + static_cast<int>(sourceY);
            if (dstY < 0 || dstY >= output.height) {
                continue;
            }
            for (unsigned sourceX = 0; sourceX < bitmap.width; ++sourceX) {
                const int dstX = dstLeft + static_cast<int>(sourceX);
                if (dstX < 0 || dstX >= output.width) {
                    continue;
                }
                uint8_t& destination = output.alpha[static_cast<size_t>(dstY) * output.width + dstX];
                destination = std::max(destination, sourceRow[sourceX]);
            }
        }

        penX += glyph->advance.x;
        previousGlyph = glyphIndex;
    }

    m_impl->lastError.clear();
    return true;
}

const std::string& FreeTypeTextRenderer::lastError() const
{
    return m_impl->lastError;
}
