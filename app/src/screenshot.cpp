// 截图实现：glReadPixels 读回像素 + stb_image_write 编码 PNG。
#include "screenshot.h"

#include <filesystem>
#include <vector>

#include <glad/gl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"

namespace l3d {

bool writeDefaultFramebufferPng(const std::filesystem::path& path, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            L3D_ERROR("Cannot create screenshot directory: {}", error.message());
            return false;
        }
    }

    // 每像素 RGBA 4 字节。绑定 READ_FRAMEBUFFER 到 0 表示“默认帧缓冲”，
    // GL_BACK 是尚未交换到屏幕的后缓冲，所以必须在 swapBuffers 之前调用。
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); // 行对齐 1 字节，避免 width*4 以外的填充假设
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glFinish(); // 等待 GPU 真正写完，防止读到半帧数据

    // OpenGL 像素原点在左下角，PNG 原点在左上角，写出前需要垂直翻转。
    stbi_flip_vertically_on_write(1);
    const auto encoded = pathToUtf8(path);
    if (stbi_write_png(encoded.c_str(), width, height, 4, pixels.data(), width * 4) == 0) {
        L3D_ERROR("Failed to write screenshot '{}'", encoded);
        return false;
    }
    L3D_INFO("Screenshot written: '{}' ({}x{})", encoded, width, height);
    return true;
}

} // namespace l3d
