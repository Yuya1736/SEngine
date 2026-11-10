#pragma once

// 场景操作工具：按标签查找、维护父子关系和递归删除。
// 分层位置：engine_app 与编辑器通过这些函数修改 World，本模块不处理输入或渲染。
// 约束：所有函数先校验句柄存活；层级成环时 setParent 拒绝修改。
#include <string>
#include <string_view>
#include <vector>

#include "l3d/scene/entity.h"

namespace l3d {

class World;

// 线性查找第一个带指定 Tag 的存活实体；找不到时返回无效句柄。
[[nodiscard]] Entity findEntityByTag(const World& world, std::string_view tag);
// 把 child 挂到 parent 下；parent 无效表示设为根节点，成环或句柄失效返回 false。
[[nodiscard]] bool setParent(World& world, Entity child, Entity parent);
// 收集 root 的全部后代（不含 root）；内部记录已访问句柄，防止层级成环时死循环。
[[nodiscard]] std::vector<Entity> collectDescendants(const World& world, Entity root);
// 先递归销毁全部后代，再销毁自身，避免留下指向已删除父实体的子节点。
void destroyEntityRecursive(World& world, Entity entity);
// 优先返回 NameComponent 的名称，空名称回退为“Entity <index>”。
[[nodiscard]] std::string displayName(const World& world, Entity entity);

} // namespace l3d
