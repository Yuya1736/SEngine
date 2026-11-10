#pragma once

// engine/assets 模块：底层文件加载器接口（OBJ 网格、PNG 纹理）。
// 分层位置：位于 engine_core 之上、AssetDatabase 之下，只做“文件 → CPU 数据”的转换。
// 约定：不抛异常，失败统一用 Result 的 error 字符串返回；不做缓存，去重由调用方负责。

#include <filesystem>

#include "l3d/assets/asset_types.h"
#include "l3d/core/result.h"

namespace l3d {

// 解析 OBJ 为三角网格；文件无法打开、某行格式错误或没有面时返回 failure。
[[nodiscard]] Result<MeshData> loadObjMesh(const std::filesystem::path& path);
// 用 stb_image 解码图片，统一输出 4 通道 RGBA；读取或解码失败时返回 failure。
[[nodiscard]] Result<TextureData> loadPngTexture(const std::filesystem::path& path);

} // namespace l3d
