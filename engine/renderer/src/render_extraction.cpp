// engine/renderer 模块：渲染提取实现。
// 分层位置：engine_app → runtime 模块 engine_renderer → engine_core；
// 本文件从 engine_scene 的 World 读取 ECS 组件，生成与 GPU 无关的 RenderFrame。
// 关键约束：只读场景、不创建 GL 资源；决定“哪些实体进入本帧”和光照数量上限。
#include "l3d/renderer/render_extraction.h"

#include <glm/gtc/matrix_transform.hpp>

#include "l3d/scene/components.h"
#include "l3d/scene/transform_system.h"
#include "l3d/scene/world.h"

namespace l3d {

namespace {

// 提取核心：遍历全部实体，把组件翻译成 RenderFrame 中的纯数据。
// 之所以不把 World 直接交给渲染器，是为了让渲染器不依赖 ECS 内部布局，
// 场景结构变化时只需改这里，渲染器可原样替换。
RenderFrame collect(World& world, const glm::mat4& view, const glm::mat4& projection,
                    const glm::vec3& cameraPosition) {
    // 实体可能挂有父子层级，这里先把 Transform 的 worldMatrix 统一刷新一遍，
    // 之后每个实体只需读取缓存好的世界矩阵，避免重复解算。
    updateWorldTransforms(world);
    RenderFrame frame;
    frame.view = view;
    frame.projection = projection;
    frame.cameraPosition = cameraPosition;
    // 场景没有方向光时的经验默认值，保证画面不会全黑。
    frame.directionalIntensity = 0.35f;
    bool foundDirectional = false;

    for (const auto entity : world.entities()) {
        const auto* transform = world.tryGet<Transform>(entity);
        if (!transform) {
            continue;
        }
        // 只采用场景里的第一盏方向光；多盏会互相覆盖，这里简单起见直接忽略其余。
        if (const auto* light = world.tryGet<DirectionalLight>(entity); light && !foundDirectional) {
            frame.directionalDirection = light->direction;
            frame.directionalColor = light->color;
            frame.directionalIntensity = light->intensity;
            foundDirectional = true;
        }
        // 点光源数量受 RenderFrame 固定数组和着色器 uniform 数组（8）限制，
        // 超出部分静默丢弃，因此场景中最多 8 盏点光真正参与光照。
        if (const auto* light = world.tryGet<PointLight>(entity);
            light && frame.pointLightCount < static_cast<int>(frame.pointLights.size())) {
            auto& output = frame.pointLights[frame.pointLightCount++];
            // 注意：Transform::position 是相对父实体的局部坐标，而光照需要世界坐标；
            // 仅当光源是根节点时两者相同，嵌套场景下这里的位置会偏（疑似缺陷，未修改）。
            output.position = transform->position;
            output.color = light->color;
            output.intensity = light->intensity;
            output.range = light->range;
        }
        // 只有带 MeshRenderer 且 visible 的实体才进入绘制列表；每个条目携带世界矩阵副本，
        // 渲染器拿到后无需再回查 ECS，也就与场景的存储方式解耦。
        if (const auto* renderer = world.tryGet<MeshRenderer>(entity);
            renderer && renderer->visible) {
            frame.items.push_back({renderer->meshGuid, renderer->materialGuid,
                                   transform->worldMatrix});
        }
    }
    return frame;
}

} // namespace

RenderFrame extractRenderFrame(World& world, const glm::vec3& cameraPosition,
                               const glm::quat& cameraRotation, float fovDegrees,
                               float nearClip, float farClip, float aspectRatio) {
    // 相机的局部坐标系遵循 OpenGL 惯例：看向 -Z，上方向为 +Y。
    // 用朝向四元数旋转这两个基向量，就得到世界空间中的前向和上方向。
    const auto forward = cameraRotation * glm::vec3(0.0f, 0.0f, -1.0f);
    const auto up = cameraRotation * glm::vec3(0.0f, 1.0f, 0.0f);
    // lookAt 构造视图矩阵：把世界坐标变换到“相机在原点、看向 -Z”的相机空间。
    const auto view = glm::lookAt(cameraPosition, cameraPosition + forward, up);
    // perspective 构造投影矩阵：负责近大远小的透视效果，把视锥压到 NDC（-1..1）。
    // fovDegrees 是垂直视场角（度），glm 要求弧度；宽高比非法时兜底为 1，避免除零。
    const auto projection = glm::perspective(glm::radians(fovDegrees),
                                             aspectRatio > 0.0f ? aspectRatio : 1.0f,
                                             nearClip, farClip);
    return collect(world, view, projection, cameraPosition);
}

// 直接使用外部传入的矩阵，适用于相机逻辑已自行维护的调用方。
// 前置条件：view/projection 与 cameraPosition 描述同一台相机，否则高光位置会偏。
RenderFrame extractRenderFrameWithCamera(World& world, const glm::mat4& view,
                                         const glm::mat4& projection,
                                         const glm::vec3& cameraPosition) {
    return collect(world, view, projection, cameraPosition);
}

} // namespace l3d
