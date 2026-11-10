#pragma once

// 文件职责：物理世界的公共接口与数据类型（射线命中结果、碰撞/触发事件）。
// 所属模块：engine/physics。在 engine_app 组合根 → 各 runtime 模块 → engine_core 分层中，
// 依赖 engine_scene 的实体与组件，向上为 runtime 提供与具体实现无关的物理抽象。
// 关键约束：接口不持有世界数据，World 由调用方传入；事件统一在每个时间步结束后产生。

#include <optional>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "l3d/scene/entity.h"

namespace l3d {

class World;

// 一条射线的命中结果：entity 是命中的实体，distance 是沿射线方向的距离（世界单位），
// point 是命中点（= origin + direction * distance）。
struct RayHit {
    Entity entity{};
    float distance = 0.0f;
    glm::vec3 point{0.0f};
};

// 重叠状态的阶段：Enter 表示本步从“不重叠”变为“重叠”，Exit 表示从“重叠”变为“不重叠”。
enum class TriggerPhase { Enter, Exit };

// 一步内产生的碰撞/触发事件。trigger 为 true 表示这只是“触发器”事件（不阻挡移动）。
// first/second 已按实体代号排序，同一对实体的事件顺序固定，便于上层去重。
struct CollisionEvent {
    Entity first{};
    Entity second{};
    TriggerPhase phase = TriggerPhase::Enter;
    bool trigger = true;
};

// 物理世界抽象：上层只依赖这个接口，因此可以替换实现，或在测试中使用空实现。
class IPhysicsWorld {
public:
    virtual ~IPhysicsWorld() = default;
    // 推进一个时间步：移动运动学角色、检测重叠，并把本步产生的 Enter/Exit 事件追加到
    // events（不会清空调用方已有的内容）。
    virtual void step(World& world, float delta, std::vector<CollisionEvent>& events) = 0;
    // 从 origin 沿 direction 发射长度不超过 maxDistance 的射线，返回最近的命中；
    // direction 不必预先归一化，ignore 指定的实体被跳过。
    virtual std::optional<RayHit> raycast(const World& world, const glm::vec3& origin,
                                          const glm::vec3& direction, float maxDistance,
                                          Entity ignore = {}) const = 0;
    // 查询两个实体此刻是否重叠（实体碰撞体与触发器都参与）。
    virtual bool overlap(const World& world, Entity first, Entity second) const = 0;
    // 清空“上一步的重叠状态”，使下一步重新产生 Enter 事件。
    virtual void reset() = 0;
};

// 基于世界轴 AABB（轴对齐包围盒）与球体的简化物理实现，不处理旋转和角速度。
// 不变量：previousCollisions_ / previousTriggers_ 保存上一步结束时的重叠对，
// 且每对都经过 orderedPair() 排序，因此可以逐个比较来推导 Enter/Exit。
class SimplePhysicsWorld final : public IPhysicsWorld {
public:
    void step(World& world, float delta, std::vector<CollisionEvent>& events) override;
    std::optional<RayHit> raycast(const World& world, const glm::vec3& origin,
                                  const glm::vec3& direction, float maxDistance,
                                  Entity ignore = {}) const override;
    bool overlap(const World& world, Entity first, Entity second) const override;
    void reset() override;

private:
    // 上一步结束时的实体-实体重叠对，用来与本步结果比较。
    std::vector<std::pair<Entity, Entity>> previousCollisions_;
    // 上一步结束时的“角色-触发器”重叠对。
    std::vector<std::pair<Entity, Entity>> previousTriggers_;
};

} // namespace l3d
