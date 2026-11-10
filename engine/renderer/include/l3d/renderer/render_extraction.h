#pragma once

// engine/renderer 模块：渲染数据提取（render extraction）接口。
// 分层位置：engine_app（组合根）→ 各 runtime 模块（engine_scene、engine_renderer 等）
// → engine_core。本文件属于 renderer，但只负责“ECS 场景 → 纯渲染数据”的转换。
// 关键约束：只读 World，不创建任何 OpenGL 对象，因此可脱离 GPU 环境直接测试。
#include <glm/gtc/quaternion.hpp>

#include "l3d/renderer/render_types.h"

namespace l3d {

class World;

// 从 ECS 世界提取一帧可绘制数据。相机用“位置 + 四元数朝向 + 视锥参数”描述，
// 函数内部换算成 view/projection 矩阵。副作用仅是按需刷新 Transform 的世界矩阵缓存。
[[nodiscard]] RenderFrame extractRenderFrame(World& world, const glm::vec3& cameraPosition,
                                             const glm::quat& cameraRotation, float fovDegrees,
                                             float nearClip, float farClip, float aspectRatio);
// 变体：调用方已经算好 view/projection（如编辑器自由相机或后处理），此处不再换算。
[[nodiscard]] RenderFrame extractRenderFrameWithCamera(World& world, const glm::mat4& view,
                                                       const glm::mat4& projection,
                                                       const glm::vec3& cameraPosition);

} // namespace l3d
