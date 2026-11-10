#pragma once

// engine/assets 模块：资产的 CPU 端数据表示与内置资产 GUID 常量。
// 分层位置：engine_app → runtime 模块（renderer/editor/scene）→ 本文件（与 engine_core 同层）；
// 只依赖 glm 与标准库，不持有 GPU 资源，也不反向依赖 core，因此可被各层安全包含。
// 约束：MeshVertex 的字段顺序须与渲染器的顶点属性布局一致，改动时两边要同步。

#include <cstdint>
#include <string>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace l3d {

// 单个顶点：位置、法线、UV；默认法线朝上，方便没有法线数据的程序化网格。
struct MeshVertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
};

// CPU 端网格：三角化后的顶点/索引与轴对齐包围盒；索引值必须都小于 vertices.size()。
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};
};

// CPU 端纹理：stb_image 解码后的 RGBA8 像素，行优先排列；本模块加载器的 channels 恒为 4。
struct TextureData {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> pixels;
};

// 材质参数（简化 Blinn-Phong）：贴图 GUID 非空时与 baseColor 相乘，否则只用 baseColor。
struct MaterialAsset {
    std::string name = "Default";
    // 基础色：RGB 参与光照，alpha 原样写入片元输出。
    glm::vec4 baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    glm::vec3 specularColor{1.0f};
    float shininess = 32.0f;
    std::string albedoTexGuid; // 反照率贴图 GUID，空串表示不使用贴图
};

// 内置资产 GUID：不落盘、没有 .meta.json，由本模块或渲染器按字符串识别后直接提供。
// "builtin:" 前缀与磁盘资产的标准 UUID 形式天然区分，不会与扫描结果冲突。
inline constexpr const char* BuiltinDefaultMaterialGuid = "builtin:default-material";
inline constexpr const char* BuiltinCubeGuid = "builtin:cube";
inline constexpr const char* BuiltinPlaneGuid = "builtin:plane";
inline constexpr const char* BuiltinSphereGuid = "builtin:sphere";

} // namespace l3d
