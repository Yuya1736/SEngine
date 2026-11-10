// 层级变换实现：由局部 TRS 生成矩阵，再沿父链左乘得到世界矩阵。
// 所属 engine_scene 模块；渲染、物理和编辑器在帧初或编辑后调用。
// 关键约束：局部旋转使用四元数；父矩阵在子矩阵左侧，列向量约定下先应用子变换。
#include "l3d/scene/transform_system.h"

#include <functional>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "l3d/core/logging.h"
#include "l3d/scene/components.h"
#include "l3d/scene/world.h"

namespace l3d {

glm::mat4 localMatrix(const Transform& transform) {
    // 从单位矩阵开始组合，固定顺序为 T * R * S，不能随意交换。
    glm::mat4 matrix = glm::translate(glm::mat4(1.0f), transform.position);
    matrix *= glm::mat4_cast(transform.rotation); // 四元数转旋转矩阵
    matrix = glm::scale(matrix, transform.scale);
    // 列向量约定：顶点先缩放、再旋转、最后平移，即 local * v。
    return matrix;
}

void updateWorldTransforms(World& world) {
    // 每个实体三种状态：0 未访问、1 正在解析（用于检测环）、2 已完成并缓存。
    std::unordered_map<std::uint32_t, std::uint8_t> state;
    bool cycleReported = false;

    std::function<glm::mat4(Entity)> resolve = [&](Entity entity) -> glm::mat4 {
        // 没有 Transform 或句柄已失效时返回单位矩阵，不阻断子节点的独立计算。
        if (!world.alive(entity) || !world.has<Transform>(entity)) {
            return glm::mat4(1.0f);
        }
        auto& stateValue = state[entity.index];
        // 已完成：直接返回已缓存的世界矩阵，避免重复向上解析。
        if (stateValue == 2) {
            return world.get<Transform>(entity).worldMatrix;
        }
        // 再次遇到“正在解析”的节点说明父链成环；告警一次并切断这次回溯。
        if (stateValue == 1) {
            if (!cycleReported) {
                L3D_WARN("Detected transform hierarchy cycle at entity {}", entity.index);
                cycleReported = true;
            }
            return glm::mat4(1.0f);
        }

        // 先标记“正在解析”，再递归父节点，保证合成时父矩阵已经就绪。
        stateValue = 1;
        auto& transform = world.get<Transform>(entity);
        // 根节点没有父矩阵，局部矩阵就是最终世界矩阵。
        glm::mat4 worldMatrix = localMatrix(transform);
        if (const auto* hierarchy = world.tryGet<Hierarchy>(entity);
            hierarchy && hierarchy->parent.valid() && hierarchy->parent != entity) {
            // 父矩阵在左、子局部矩阵在右：列向量约定下先应用子变换，再应用父变换。
            worldMatrix = resolve(hierarchy->parent) * worldMatrix;
        }
        // 写回缓存并标记完成，供兄弟分支和外部模块直接读取。
        transform.worldMatrix = worldMatrix;
        stateValue = 2;
        return worldMatrix;
    };

    // 从每个存活实体出发自然覆盖所有根和子树；已完成的节点会命中状态缓存。
    for (const auto entity : world.entities()) {
        resolve(entity);
    }
}

} // namespace l3d
