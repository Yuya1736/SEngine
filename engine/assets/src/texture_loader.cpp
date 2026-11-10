// engine/assets 模块：基于 stb_image 的图片解码器（对外接口名为 loadPngTexture）。
// 分层位置：engine_core 之上、AssetDatabase 之下，只负责“文件字节 → CPU 纹理数据”。
// 约束：不抛异常；统一输出 4 通道 RGBA，并把行序翻转为 OpenGL 的“左下角为原点”。

#include "l3d/assets/loaders.h"

#include <stb_image.h>

#include "l3d/core/file_utils.h"

namespace l3d {

Result<TextureData> loadPngTexture(const std::filesystem::path& path) {
    // 先整份读入内存：既能按 UTF-8 路径统一读取，也方便交给 stb 的内存解码接口。
    auto bytes = readBinaryFile(path);
    if (!bytes) {
        return Result<TextureData>::failure("Cannot read texture: " + pathToUtf8(path));
    }

    // OpenGL 的纹理坐标原点在左下角，而多数图片的第一行是顶部，因此加载时上下翻转。
    // 注意这是 stb 的全局开关，会影响同一线程后续的所有解码。
    stbi_set_flip_vertically_on_load(1);
    int width = 0;
    int height = 0;
    int channels = 0;
    // 最后一个参数 4 表示强制输出 RGBA：灰度/RGB 源图都会补齐 alpha，渲染器只处理一种布局。
    stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width,
                                            &height, &channels, 4);
    // 解码失败返回 nullptr；转成 Result 错误，避免把空指针交给上层。
    if (!pixels) {
        return Result<TextureData>::failure("Cannot decode PNG texture: " + pathToUtf8(path));
    }

    TextureData texture;
    texture.width = width;
    texture.height = height;
    // 虽然请求了 4 通道，channels 仍是 stb 的输出参数，这里按实际布局记录。
    texture.channels = 4;
    // 拷贝一份由 TextureData 持有；之后可以安全释放 stb 的缓冲区。
    texture.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return Result<TextureData>::success(std::move(texture));
}

} // namespace l3d
