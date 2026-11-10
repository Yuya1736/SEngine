// 文件职责：SimplePhysicsWorld 的实现——角色移动、AABB/球体检测、射线检测与事件分发。
// 所属模块：engine/physics，属于 runtime 层实现：读取/写入 engine_scene 的 World，
// 不依赖渲染或窗口，也不拥有任何实体数据。
// 关键约束：按固定小步长设计；Enter/Exit 事件必须在本步全部检测完成后统一分发。
#include "l3d/physics/physics_world.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <glm/geometric.hpp>

#include "l3d/scene/components.h"
#include "l3d/scene/world.h"

namespace l3d {

namespace {

// 以下辅助函数只在本文件内部使用，负责几何计算与实体分类，不对外暴露。

// 轴对齐包围盒（AABB）：用两个对角顶点 min/max 表示的长方体。它的边永远平行于世界轴，
// 所以相交测试只是逐轴比较区间，很便宜；代价是无法表示旋转后的盒子。
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
};

// 三个轴上的区间都重叠时两个 AABB 才相交；任一轴分离就说明它们不可能碰撞。
bool intersects(const Aabb& left, const Aabb& right) {
    return left.min.x <= right.max.x && left.max.x >= right.min.x &&
           left.min.y <= right.max.y && left.max.y >= right.min.y &&
           left.min.z <= right.max.z && left.max.z >= right.min.z;
}

// 角色用“以 position 为中心、halfExtents 为半长”的盒子近似；旋转暂不参与。
Aabb characterBounds(const Transform& transform, const CharacterController& controller) {
    return {transform.position - controller.halfExtents,
            transform.position + controller.halfExtents};
}

// 把实体的碰撞体转换为世界坐标下的 AABB。
// 缩放取绝对值，避免负缩放把 min/max 的顺序颠倒。
std::optional<Aabb> colliderBounds(const World& world, Entity entity) {
    const auto* transform = world.tryGet<Transform>(entity);
    if (!transform) {
        return std::nullopt;
    }
    if (const auto* box = world.tryGet<BoxCollider>(entity)) {
        const glm::vec3 extent = glm::abs(box->halfExtents * transform->scale);
        return Aabb{transform->position - extent, transform->position + extent};
    }
    if (const auto* sphere = world.tryGet<SphereCollider>(entity)) {
        // 球体用最大缩放分量近似成外接立方体，仅用于粗略的包围盒判定。
        const float scale = std::max({std::abs(transform->scale.x),
                                      std::abs(transform->scale.y),
                                      std::abs(transform->scale.z)});
        const glm::vec3 extent(sphere->radius * scale);
        return Aabb{transform->position - extent, transform->position + extent};
    }
    return std::nullopt;
}

// 触发器标记只存在于碰撞体组件上；没有碰撞体的实体永远不是触发器。
bool isTrigger(const World& world, Entity entity) {
    if (const auto* box = world.tryGet<BoxCollider>(entity)) {
        return box->trigger;
    }
    if (const auto* sphere = world.tryGet<SphereCollider>(entity)) {
        return sphere->trigger;
    }
    return false;
}

// 带 CharacterController 的实体由物理系统驱动，这里称为“运动学角色”：
// 它按速度和重力移动，但不会被其他物体推动。
bool isCharacter(const World& world, Entity entity) {
    return world.tryGet<CharacterController>(entity) != nullptr;
}

// “实体碰撞体”= 角色、方块碰撞体或球体碰撞体；它们参与碰撞检测，但不一定阻挡移动。
bool isSolid(const World& world, Entity entity) {
    return world.tryGet<CharacterController>(entity) != nullptr ||
           world.tryGet<BoxCollider>(entity) != nullptr ||
           world.tryGet<SphereCollider>(entity) != nullptr;
}

// 判断两个实体此刻是否重叠。角色优先用控制器盒子，对方是球体时做球-AABB 精确测试，
// 否则退回 AABB-AABB 近似测试。只读取世界状态，不修改任何数据。
bool overlapsEntities(const World& world, Entity first, Entity second) {
    // 同一实体，或任一实体已销毁，直接视为不重叠。
    if (first == second || !world.alive(first) || !world.alive(second)) {
        return false;
    }
    const auto* firstController = world.tryGet<CharacterController>(first);
    const auto* secondController = world.tryGet<CharacterController>(second);
    const auto* firstTransform = world.tryGet<Transform>(first);
    const auto* secondTransform = world.tryGet<Transform>(second);
    const auto firstCollider = colliderBounds(world, first);
    const auto secondCollider = colliderBounds(world, second);
    if ((!firstController && !firstCollider) || (!secondController && !secondCollider) ||
        !firstTransform || !secondTransform) {
        return false;
    }

    const Aabb firstBounds = firstController ? characterBounds(*firstTransform, *firstController)
                                             : *firstCollider;
    if (const auto* sphere = world.tryGet<SphereCollider>(second)) {
        // 最近点法：把球心钳制到盒子范围内，得到盒子上离球心最近的点。
        // 球心到该点的距离 ≤ 半径即相交；球心在盒内时最近点就是自身，距离为 0。
        const float scale = std::max({std::abs(secondTransform->scale.x),
                                      std::abs(secondTransform->scale.y),
                                      std::abs(secondTransform->scale.z)});
        const glm::vec3 closest = glm::clamp(secondTransform->position, firstBounds.min,
                                             firstBounds.max);
        const auto delta = secondTransform->position - closest;
        const float radius = sphere->radius * scale;
        return glm::dot(delta, delta) <= radius * radius;
    }
    const Aabb secondBounds = secondController
                                  ? characterBounds(*secondTransform, *secondController)
                                  : *secondCollider;
    return intersects(firstBounds, secondBounds);
}

// 角色移动到 position 时是否被挡住：与任一“非角色、非触发器”的碰撞体相交就算被挡。
// 触发器只发事件、不阻挡移动；角色之间也互不阻挡，避免两个角色挤在一起时互相卡死。
bool blockedBy(const World& world, Entity character, const glm::vec3& position,
               const CharacterController& controller) {
    const Aabb bounds{position - controller.halfExtents, position + controller.halfExtents};
    for (const auto entity : world.entities()) {
        if (entity == character || isCharacter(world, entity) || isTrigger(world, entity)) {
            continue;
        }
        const auto collider = colliderBounds(world, entity);
        if (collider && intersects(bounds, *collider)) {
            return true;
        }
    }
    return false;
}

// 按 (index, generation) 排序实体对，保证同一对实体在任何遍历顺序下表示一致，
// 这样历史集合与当前集合才能直接比较。
std::pair<Entity, Entity> orderedPair(Entity first, Entity second) {
    if (second.index < first.index ||
        (second.index == first.index && second.generation < first.generation)) {
        std::swap(first, second);
    }
    return {first, second};
}

// 射线 vs AABB（“平板法”）：把射线分别投影到 x/y/z 轴，求出进入和离开每一对
// 平行“平板”的参数 t；所有轴的进入区间仍有交集时，射线就命中了盒子。
bool rayAabb(const glm::vec3& origin, const glm::vec3& direction, const Aabb& bounds,
             float& distance) {
    float near = 0.0f;
    float far = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(direction[axis]) < 1e-7f) {
            // 方向在该轴接近 0：射线与这对平板平行，原点不在区间内就必然打不中。
            if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis]) {
                return false;
            }
            continue;
        }
        float first = (bounds.min[axis] - origin[axis]) / direction[axis];
        float second = (bounds.max[axis] - origin[axis]) / direction[axis];
        if (first > second) {
            // 方向分量为负时两个 t 的顺序会颠倒，交换后 first 才是进入、second 是离开。
            std::swap(first, second);
        }
        // near 取各轴进入时刻的最大值，far 取离开时刻的最小值。
        near = std::max(near, first);
        far = std::min(far, second);
        if (near > far) {
            return false;
        }
    }
    // near 从 0 开始，因此原点在盒内时 distance 为 0，表示“从当前点立刻命中”。
    distance = near >= 0.0f ? near : far;
    return distance >= 0.0f;
}

// 射线 vs 球：把 |origin + t*dir - center|² = r² 展开成关于 t 的一元二次方程，
// 判别式 < 0 表示不相交。distance 取较小的非负根；两根都为负说明球在射线背后。
bool raySphere(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& center,
               float radius, float& distance) {
    // direction 已归一化，a 约为 1，这里仍保留一般形式以便复用。
    const glm::vec3 offset = origin - center;
    const float a = glm::dot(direction, direction);
    const float b = 2.0f * glm::dot(offset, direction);
    const float c = glm::dot(offset, offset) - radius * radius;
    const float discriminant = b * b - 4.0f * a * c;
    if (discriminant < 0.0f || std::abs(a) < 1e-7f) {
        return false;
    }
    const float root = std::sqrt(discriminant);
    float first = (-b - root) / (2.0f * a);
    float second = (-b + root) / (2.0f * a);
    if (first > second) {
        std::swap(first, second);
    }
    distance = first >= 0.0f ? first : second;
    return distance >= 0.0f;
}

} // namespace

// 一次 step 的顺序：移动角色 → 收集触发器重叠 → 收集实体重叠 → 与上一步比较并分发
// Enter/Exit → 保存本步快照。顺序固定，保证事件描述的是“这一步结束时”的状态。
void SimplePhysicsWorld::step(World& world, float delta, std::vector<CollisionEvent>& events) {
    // 把步长截断在 [0, 0.1] 秒：断点或卡顿后的大 delta 会让角色穿过薄墙，
    // 也会让重力的速度积分突然变得很大。
    const float clampedDelta = std::clamp(delta, 0.0f, 0.1f);

    for (const auto entity : world.entities()) {
        auto* controller = world.tryGet<CharacterController>(entity);
        auto* transform = world.tryGet<Transform>(entity);
        if (!controller || !transform) {
            continue;
        }

        // 半隐式欧拉：先用重力更新速度，再用新速度计算位移，固定小步长下比显式欧拉稳定。
        controller->velocity.y += controller->gravity * clampedDelta;
        const glm::vec3 movement = controller->velocity * clampedDelta;

        // 分轴移动：先 X、再 Z、最后 Y，每轴单独检测，被挡时只回退该轴。
        // 这样撞墙只失去法线方向的速度，沿墙的切向速度得以保留（可以“贴墙滑动”）。
        glm::vec3 candidateX = transform->position;
        candidateX.x += movement.x;
        if (std::abs(movement.x) > 0.0f &&
            blockedBy(world, entity, candidateX, *controller)) {
            controller->velocity.x = 0.0f;
        } else {
            transform->position.x = candidateX.x;
        }

        glm::vec3 candidateZ = transform->position;
        candidateZ.z += movement.z;
        if (std::abs(movement.z) > 0.0f &&
            blockedBy(world, entity, candidateZ, *controller)) {
            controller->velocity.z = 0.0f;
        } else {
            transform->position.z = candidateZ.z;
        }

        glm::vec3 candidateY = transform->position;
        candidateY.y += movement.y;
        if (std::abs(movement.y) > 0.0f &&
            blockedBy(world, entity, candidateY, *controller)) {
            // 向下被挡 => 站在地面或物体上；向上被挡（撞天花板）不算落地。
            controller->grounded = movement.y < 0.0f;
            controller->velocity.y = 0.0f;
        } else {
            transform->position.y = candidateY.y;
            // 竖直方向没有碰撞，说明角色正悬空。
            controller->grounded = false;
        }
    }

    // 触发器检测：只产生事件、不阻挡移动，所以放在角色移动之后，按当前姿态收集。
    // 角色是主动检测方，触发器可以是被动的静态物体。
    std::vector<std::pair<Entity, Entity>> currentTriggers;
    for (const auto character : world.entities()) {
        if (!isCharacter(world, character)) {
            continue;
        }
        for (const auto other : world.entities()) {
            if (other == character || !isTrigger(world, other)) {
                continue;
            }
            if (overlapsEntities(world, character, other)) {
                currentTriggers.push_back(orderedPair(character, other));
            }
        }
    }

    // 实体碰撞检测：两两组合，内层从 firstIndex + 1 开始，避免同一对重复检测。
    // 触发器被排除：它只发事件，不参与实体碰撞。角色也算实体碰撞体，所以角色之间的
    // 重叠会产生事件，但 blockedBy() 并不阻止角色互相移动。
    std::vector<std::pair<Entity, Entity>> currentCollisions;
    for (std::size_t firstIndex = 0; firstIndex < world.entities().size(); ++firstIndex) {
        const auto first = world.entities()[firstIndex];
        if (!isSolid(world, first) || isTrigger(world, first)) {
            continue;
        }
        for (std::size_t secondIndex = firstIndex + 1; secondIndex < world.entities().size();
             ++secondIndex) {
            const auto second = world.entities()[secondIndex];
            if (!isSolid(world, second) || isTrigger(world, second)) {
                continue;
            }
            if (overlapsEntities(world, first, second)) {
                currentCollisions.push_back(orderedPair(first, second));
            }
        }
    }

    // 在“已排序实体对”列表中查找某一对实体。
    auto containsPair = [](const std::vector<std::pair<Entity, Entity>>& pairs, Entity first,
                           Entity second) {
        const auto ordered = orderedPair(first, second);
        return std::ranges::find(pairs, ordered) != pairs.end();
    };

    // 事件必须等本步所有移动和检测完成后才统一分发：Enter/Exit 是拿“上一步快照”
    // 与本步结果比较得出的，提前分发会让同一帧出现先 Exit 后 Enter 的错乱。
    for (const auto& pair : currentCollisions) {
        if (!containsPair(previousCollisions_, pair.first, pair.second)) {
            events.push_back({pair.first, pair.second, TriggerPhase::Enter, false});
        }
    }
    for (const auto& pair : previousCollisions_) {
        // 实体已销毁时不再补发 Exit，避免上层收到无效句柄。
        if (!containsPair(currentCollisions, pair.first, pair.second) && world.alive(pair.first) &&
            world.alive(pair.second)) {
            events.push_back({pair.first, pair.second, TriggerPhase::Exit, false});
        }
    }

    for (const auto& pair : currentTriggers) {
        if (!containsPair(previousTriggers_, pair.first, pair.second)) {
            events.push_back({pair.first, pair.second, TriggerPhase::Enter, true});
        }
    }
    for (const auto& pair : previousTriggers_) {
        if (!containsPair(currentTriggers, pair.first, pair.second) && world.alive(pair.first) &&
            world.alive(pair.second)) {
            events.push_back({pair.first, pair.second, TriggerPhase::Exit, true});
        }
    }

    // 更新快照：本步的重叠对成为下一步比较用的“上一步”。
    previousTriggers_.clear();
    // 清空后把本步的触发器对复制进去；内层判重属于防御性写法。
    for (const auto& pair : currentTriggers) {
        if (!containsPair(previousTriggers_, pair.first, pair.second)) {
            previousTriggers_.push_back(pair);
        }
    }
    previousCollisions_ = std::move(currentCollisions);
}

std::optional<RayHit> SimplePhysicsWorld::raycast(const World& world, const glm::vec3& origin,
                                                   const glm::vec3& direction,
                                                   float maxDistance, Entity ignore) const {
    // 零长度方向向量无法定义射线。
    if (glm::dot(direction, direction) < 1e-12f) {
        return std::nullopt;
    }
    // 归一化后 t 才表示“世界单位距离”，可以直接与 maxDistance 比较。
    const auto normalized = glm::normalize(direction);
    std::optional<RayHit> result;
    // 射线会命中所有带碰撞体的实体（包括触发器），ignore 常用来忽略发射者自己。
    for (const auto entity : world.entities()) {
        if (entity == ignore) {
            continue;
        }
        const auto* transform = world.tryGet<Transform>(entity);
        if (!transform) {
            continue;
        }
        float distance = std::numeric_limits<float>::max();
        bool hit = false;
        if (const auto* sphere = world.tryGet<SphereCollider>(entity)) {
            const float scale = std::max({std::abs(transform->scale.x),
                                          std::abs(transform->scale.y),
                                          std::abs(transform->scale.z)});
            hit = raySphere(origin, normalized, transform->position, sphere->radius * scale,
                            distance);
        } else if (const auto bounds = colliderBounds(world, entity)) {
            hit = rayAabb(origin, normalized, *bounds, distance);
        }
        // 只接受 maxDistance 以内、且比当前命中更近的结果。
        if (hit && distance <= maxDistance && (!result || distance < result->distance)) {
            result = RayHit{entity, distance, origin + normalized * distance};
        }
    }
    return result;
}

// 公共查询接口：复用与 step() 相同的重叠判定，保证两处行为一致。
bool SimplePhysicsWorld::overlap(const World& world, Entity first, Entity second) const {
    return overlapsEntities(world, first, second);
}

// 重建场景或读档后应调用：否则旧快照可能把新实体对误判为“一直重叠”，漏发 Enter。
void SimplePhysicsWorld::reset() {
    previousCollisions_.clear();
    previousTriggers_.clear();
}

} // namespace l3d
