#pragma once

// 实体句柄定义：engine_scene 模块最基础的标识类型。
// 分层位置：engine_app 作为组合根装配各 runtime 模块，本模块为它们提供场景数据；
// 上层模块只保存 Entity 句柄，不能缓存组件或实体的内存地址。
// 约束：Entity 是轻量值类型，是否仍存活必须交由 World 的槽位与 generation 校验。
#include <cstddef>
#include <cstdint>
#include <functional>

namespace l3d {

// 实体只是“槽位编号 + 版本号”，本身不包含任何组件数据。
// 类比：它像一张会员卡号，真正的数据都放在 World 的组件池里；拷贝句柄不会复制实体。
struct Entity {
    std::uint32_t index = 0xffffffffu; // 槽位下标；全 1 表示无效句柄
    std::uint32_t generation = 0; // 槽位复用计数，用于拒绝过期的旧句柄

    // 只检查编号是否有效；是否仍存活还必须交给 World::alive() 校验。
    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0xffffffffu; }
    constexpr bool operator==(const Entity&) const = default;
};

} // namespace l3d

namespace std {
// 让 Entity 能作为 unordered_map 的键：哈希同时混入 index 与 generation。
template <>
struct hash<l3d::Entity> {
    std::size_t operator()(const l3d::Entity& entity) const noexcept {
        return (static_cast<std::size_t>(entity.index) << 32) ^ entity.generation;
    }
};
} // namespace std
