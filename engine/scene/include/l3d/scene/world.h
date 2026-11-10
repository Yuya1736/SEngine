#pragma once

// ECS 核心：实体槽位管理、类型擦除组件池与 SparseSet 稀疏集合。
// 分层位置：engine_app 组合根装配各 runtime 模块，本模块把 World 提供给物理、
// 渲染、脚本等模块；World 本身不拥有任何子系统。
// 约束：句柄查询先校验 alive；组件池按类型延迟创建；拷贝 World 必须是深拷贝。
#include <cstdint>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "l3d/scene/entity.h"

namespace l3d {

class EventBus;

// 组件池的统一接口，让 World 在不知道具体组件类型时也能删除和复制组件。
// 具体池由 SparseSet<Component> 实现；pools_ 保存 type_index 到该接口的映射。
class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual bool has(Entity entity) const = 0;
    // 删除组件；实体没有该组件时返回 false。
    virtual bool remove(Entity entity) = 0;
    // 深拷贝整个池；World 拷贝（Play 快照）依赖它。
    [[nodiscard]] virtual std::unique_ptr<IComponentPool> clone() const = 0;
};

// 稀疏集合：每种组件类型一个实例，提供 O(1) 的增、删、查。
// 三个数组的关系：sparse_[实体 index] 给出紧凑数组下标 dense；
// entities_[dense] 与 components_[dense] 属于同一实体，一一对应。
// 查询先经 sparse_ 定位，再直接访问 components_；删除用 swap-and-pop 保持紧凑。
template <typename Component>
class SparseSet final : public IComponentPool {
public:
    // 添加组件：已有则覆盖数据，没有则追加到紧凑数组尾部。
    void add(Entity entity, Component component) {
        // 实体的 index 可能超过当前稀疏数组长度，按需扩展并填空位标记。
        if (entity.index >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(entity.index) + 1, InvalidIndex);
        }
        if (sparse_[entity.index] != InvalidIndex) {
            components_[sparse_[entity.index]] = std::move(component);
            return;
        }
        sparse_[entity.index] = static_cast<std::uint32_t>(entities_.size());
        entities_.push_back(entity);
        components_.push_back(std::move(component));
    }

    // swap-and-pop：把最后一个元素搬到被删位置，再弹出尾部，避免数组出现空洞。
    bool remove(Entity entity) override {
        if (!has(entity)) {
            return false;
        }
        const auto index = sparse_[entity.index];
        const auto last = static_cast<std::uint32_t>(entities_.size() - 1);
        // 被删者不是尾项时，用尾项填补空位，并更新尾项在 sparse_ 中的反向下标。
        if (index != last) {
            entities_[index] = entities_[last];
            components_[index] = std::move(components_[last]);
            sparse_[entities_[index].index] = index;
        }
        entities_.pop_back();
        components_.pop_back();
        sparse_[entity.index] = InvalidIndex;
        return true;
    }

    // 三重校验：下标在范围内、sparse_ 有效，且紧凑数组里的句柄版本完全一致。
    [[nodiscard]] bool has(Entity entity) const override {
        return entity.index < sparse_.size() && sparse_[entity.index] != InvalidIndex &&
               entities_[sparse_[entity.index]] == entity;
    }

    [[nodiscard]] Component* tryGet(Entity entity) {
        return has(entity) ? &components_[sparse_[entity.index]] : nullptr;
    }

    [[nodiscard]] const Component* tryGet(Entity entity) const {
        return has(entity) ? &components_[sparse_[entity.index]] : nullptr;
    }

    [[nodiscard]] Component& get(Entity entity) { return *tryGet(entity); }
    [[nodiscard]] const Component& get(Entity entity) const { return *tryGet(entity); }
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return entities_; }
    [[nodiscard]] std::vector<Component>& components() noexcept { return components_; }
    [[nodiscard]] const std::vector<Component>& components() const noexcept { return components_; }
    [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }

    // 依靠 SparseSet 的默认拷贝构造复制三个数组，得到互不共享的深拷贝。
    [[nodiscard]] std::unique_ptr<IComponentPool> clone() const override {
        return std::make_unique<SparseSet<Component>>(*this);
    }

private:
    static constexpr std::uint32_t InvalidIndex = 0xffffffffu; // 稀疏数组的空位标记
    std::vector<std::uint32_t> sparse_; // 实体 index -> 紧凑数组下标
    std::vector<Entity> entities_; // 紧凑排列的实体句柄
    std::vector<Component> components_; // 与 entities_ 一一对应的组件数据
};

// 一个 World 就是一份独立场景：管理实体生命周期、组件存储和销毁事件。
// 不变量：alive_ 只含仍存活的句柄；slots_ 按 index 保存 generation 与存活标记；
// 组件池只为实际用过的类型创建；销毁实体会同步移除它的全部组件。
class World {
public:
    World() = default;
    // 深拷贝：Play 前用它保存快照，组件池通过 clone() 复制，不与实时场景共享数据。
    World(const World& other);
    World(World&&) noexcept = default;
    World& operator=(const World& other);
    World& operator=(World&&) noexcept = default;
    ~World() = default;

    // 创建实体：优先复用 freeList_ 的空闲槽位，否则追加新槽位；generation 取槽位当前值。
    [[nodiscard]] Entity create();
    // 销毁实体：移除全部组件、递增 generation、回收槽位并发布 EntityDestroyedEvent。
    void destroy(Entity entity);
    // 清空实体和所有组件池，同时解除与事件总线的绑定。
    void clear();
    // 存活判定：index 在范围内、槽位存活，且 generation 与句柄完全一致。
    [[nodiscard]] bool alive(Entity entity) const;
    // 返回存活实体列表；顺序不保证稳定（删除使用 swap-and-pop）。
    [[nodiscard]] const std::vector<Entity>& entities() const noexcept { return alive_; }
    // 注入事件总线；World 不拥有它，调用者需保证总线寿命长于 World。
    void setEventBus(EventBus* eventBus) noexcept { eventBus_ = eventBus; }

    // 添加或覆盖组件，返回池内引用；前置条件：entity 必须仍然存活。
    template <typename Component>
    Component& add(Entity entity, Component component = {}) {
        auto& pool = componentPool<Component>();
        pool.add(entity, std::move(component));
        return pool.get(entity);
    }

    // 移除组件；实体没有该组件时返回 false。
    template <typename Component>
    bool remove(Entity entity) {
        return componentPool<Component>().remove(entity);
    }

    // 同时检查实体存活和组件存在，避免用已失效的旧句柄误判。
    template <typename Component>
    [[nodiscard]] bool has(Entity entity) const {
        return alive(entity) && componentPool<Component>().has(entity);
    }

    // 可选访问：实体或组件不存在时返回 nullptr。
    template <typename Component>
    [[nodiscard]] Component* tryGet(Entity entity) {
        return alive(entity) ? componentPool<Component>().tryGet(entity) : nullptr;
    }

    template <typename Component>
    [[nodiscard]] const Component* tryGet(Entity entity) const {
        return alive(entity) ? componentPool<Component>().tryGet(entity) : nullptr;
    }

    // 直接访问：前置条件是实体存活且确有该组件，否则解引用空指针属于未定义行为。
    template <typename Component>
    [[nodiscard]] Component& get(Entity entity) { return componentPool<Component>().get(entity); }

    template <typename Component>
    [[nodiscard]] const Component& get(Entity entity) const {
        return componentPool<Component>().get(entity);
    }

    // 按 type_index 延迟创建组件池：某类型首次使用时才分配 SparseSet。
    template <typename Component>
    [[nodiscard]] SparseSet<Component>& componentPool() {
        const auto type = std::type_index(typeid(Component));
        auto position = pools_.find(type);
        if (position == pools_.end()) {
            auto pool = std::make_unique<SparseSet<Component>>();
            // 先保留裸指针再 move，unique_ptr 转移所有权后仍能返回池的引用。
            auto* raw = pool.get();
            pools_.emplace(type, std::move(pool));
            return *raw;
        }
        return *static_cast<SparseSet<Component>*>(position->second.get());
    }

    // const 查询不创建池；该类型从未使用时返回一个共享的空池。
    template <typename Component>
    [[nodiscard]] const SparseSet<Component>& componentPool() const {
        const auto type = std::type_index(typeid(Component));
        const auto position = pools_.find(type);
        if (position == pools_.end()) {
            static const SparseSet<Component> empty;
            return empty;
        }
        return *static_cast<const SparseSet<Component>*>(position->second.get());
    }

private:
    // 实体槽位可反复复用；generation 每销毁一次加一，旧句柄随之失效。
    struct Slot {
        std::uint32_t generation = 0;
        bool alive = false;
    };
    std::vector<Slot> slots_; // index -> 槽位状态
    std::vector<Entity> alive_; // 当前存活实体，用于快速遍历
    std::vector<std::uint32_t> freeList_; // 可复用的空闲槽位 index
    // 类型擦除：key 是组件类型，value 是对应的 SparseSet<Component>。
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> pools_;
    EventBus* eventBus_ = nullptr; // 非拥有指针，只用于发布实体销毁事件
};

} // namespace l3d
