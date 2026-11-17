#pragma once

#include <cstdint>
#include <vector>

namespace pw {

// 渲染结果统一成 RGBA8888、行优先、自上而下。
// Qt 侧直接用 QImage::Format_RGBA8888 包一层，web 壳侧直接 memcpy 进 canvas/ImageData。
struct RasterImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
    std::size_t byteSize() const { return rgba.size(); }
    void reset(int w, int h) {
        width = w;
        height = h;
        rgba.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
    }
};

}  // namespace pw
