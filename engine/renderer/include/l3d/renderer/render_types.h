#pragma once

// engine/renderer 模块：CPU 侧渲染数据结构（接近 POD，可直接拷贝/序列化）。
// 分层位置：engine_app → runtime 模块 → engine_core；这里是 runtime 层内部的数据契约。
// 关键约束：只描述“画什么”，不出现 GLuint、VAO、着色器等 GPU 句柄，
// 因此渲染提取与具体图形 API（OpenGL/D3D/Vulkan）彻底解耦。
#include <array>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace l3d {

// 提取后的点光源快照。注意类型名是 PointerLight（历史拼写），语义就是 PointLight。
// range 是影响半径，超过它的片元不受该光源照射。
struct RenderPointerLight {
    glm::vec3 position{0.0f};
    glm::vec3 color{1.0f};
    float intensity = 0.0f;
    float range = 1.0f;
};

// 一次 draw call 所需的最小信息：用 GUID 引用网格/材质，用世界矩阵定位。
// 这里只存字符串 ID 而非指针，渲染器再用资产库延迟解析成 GPU 资源。
struct RenderItem {
    std::string meshGuid;
    std::string materialGuid;
    glm::mat4 transform{1.0f};
};

// 一帧完整的渲染输入：相机矩阵 + 光照 + 可见物体列表。
// 不变量：pointLightCount ∈ [0, pointLights.size()]，渲染器按这个数量上传光源。
struct RenderFrame {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec3 cameraPosition{0.0f};
    glm::vec3 directionalDirection{-0.5f, -1.0f, -0.3f};
    glm::vec3 directionalColor{1.0f};
    float directionalIntensity = 1.0f;
    std::array<RenderPointerLight, 8> pointLights{};
    int pointLightCount = 0;
    std::vector<RenderItem> items;
};

// 网格在局部空间的轴对齐包围盒（AABB），供拾取、视锥裁剪和编辑器显示使用。
struct MeshBounds {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

} // namespace l3d
