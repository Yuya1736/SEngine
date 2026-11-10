// 场景操作工具实现：标签查找、父链校验、后代收集和递归销毁。
// 所属 engine_scene 模块，为 engine_app/编辑器提供不依赖界面的场景编辑操作。
// 关键约束：修改前先检查 alive；遍历层级时用 visited 防止环导致死循环。
#include "l3d/scene/scene_utils.h"

#include <algorithm>
#include <unordered_set>

#include "l3d/scene/components.h"
#include "l3d/scene/world.h"

namespace l3d {

Entity findEntityByTag(const World& world, std::string_view tag) {
    // 全场景可能有重复标签，按存活列表顺序返回找到的第一个。
    for (const auto entity : world.entities()) {
        if (const auto* component = world.tryGet<TagComponent>(entity);
            component && component->value == tag) {
            return entity;
        }
    }
    return {};
}

bool setParent(World& world, Entity child, Entity parent) {
    // parent 无效表示解除父子关系（设为根）；child 失效、parent 失效或自挂接都拒绝。
    if (!world.alive(child) || (parent.valid() && !world.alive(parent)) || child == parent) {
        return false;
    }
    // 从目标父节点沿父链向上走；若途中遇到 child，说明挂接后会形成环。
    Entity cursor = parent;
    std::unordered_set<std::uint32_t> visited;
    // visited 兼作环保护：即使现有数据已损坏，也不会让 while 无限循环。
    while (cursor.valid() && world.alive(cursor) && visited.insert(cursor.index).second) {
        if (cursor == child) {
            return false;
        }
        const auto* hierarchy = world.tryGet<Hierarchy>(cursor);
        cursor = hierarchy ? hierarchy->parent : Entity{};
    }
    // 所有校验通过后才写入；没有 Hierarchy 组件时补建，已有则只改 parent。
    auto* hierarchy = world.tryGet<Hierarchy>(child);
    if (!hierarchy) {
        hierarchy = &world.add<Hierarchy>(child);
    }
    hierarchy->parent = parent;
    return true;
}

std::vector<Entity> collectDescendants(const World& world, Entity root) {
    std::vector<Entity> result;
    std::unordered_set<std::uint32_t> visited;
    // 显式栈做深度优先遍历，避免深层场景递归导致调用栈溢出。
    std::vector<Entity> frontier{root};
    while (!frontier.empty()) {
        const auto current = frontier.back();
        frontier.pop_back();
        // 已处理的节点不再扩展，既防重复也防父子关系成环时死循环。
        if (!visited.insert(current.index).second) {
            continue;
        }
        // 朴素扫描所有存活实体找直接子节点；场景规模有限时可接受。
        for (const auto entity : world.entities()) {
            const auto* hierarchy = world.tryGet<Hierarchy>(entity);
            if (hierarchy && hierarchy->parent == current) {
                result.push_back(entity);
                frontier.push_back(entity);
            }
        }
    }
    return result;
}

void destroyEntityRecursive(World& world, Entity entity) {
    // 旧句柄没有可销毁的内容，提前返回。
    if (!world.alive(entity)) {
        return;
    }
    auto descendants = collectDescendants(world, entity);
    // 收集顺序保证父在子之前，反转后即可先销毁后代、再销毁根节点。
    std::reverse(descendants.begin(), descendants.end());
    for (const auto child : descendants) {
        world.destroy(child);
    }
    // 子树清理完后再删除根节点，避免子节点暂时指向已失效的父句柄。
    world.destroy(entity);
}

std::string displayName(const World& world, Entity entity) {
    // 空名称也视为缺失，保证编辑器层级和日志里始终有可读标识。
    if (const auto* name = world.tryGet<NameComponent>(entity); name && !name->value.empty()) {
        return name->value;
    }
    return "Entity " + std::to_string(entity.index);
}

} // namespace l3d
