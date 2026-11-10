// World 的实现：实体槽位分配/回收、组件池生命周期和场景深拷贝。
// 所属 engine_scene 模块，是各 runtime 模块共享的场景容器，位于 engine_core 之上。
// 关键约束：销毁时递增 generation 并回收 index；拷贝必须克隆每个组件池。
// 事件总线只借用不拥有，复制后的 World 不继承原对象的事件总线。
#include "l3d/scene/world.h"

#include <algorithm>

#include "l3d/core/event_bus.h"
#include "l3d/scene/events.h"

namespace l3d {

World::World(const World& other)
    : slots_(other.slots_), alive_(other.alive_), freeList_(other.freeList_) {
    // 逐个 clone 组件池；组件按值复制，因此快照与源 World 不共享任何数据。
    for (const auto& [type, pool] : other.pools_) {
        pools_[type] = pool->clone();
    }
}

World& World::operator=(const World& other) {
    if (this == &other) {
        return *this;
    }
    // 先构造完整副本再替换成员：若 clone 抛出异常，原对象仍保持有效。
    World copy(other);
    slots_ = std::move(copy.slots_);
    alive_ = std::move(copy.alive_);
    freeList_ = std::move(copy.freeList_);
    pools_ = std::move(copy.pools_);
    // 事件总线属于组合根，显式清空，避免赋值后仍指向旧应用的总线。
    eventBus_ = nullptr;
    return *this;
}

Entity World::create() {
    std::uint32_t index = 0;
    if (!freeList_.empty()) {
        // 复用空闲槽位：index 不变，generation 保留上次销毁时递增后的值。
        index = freeList_.back();
        freeList_.pop_back();
        slots_[index].alive = true;
    } else {
        // 没有空闲槽位才扩展数组；新槽位的 generation 从 0 开始。
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(Slot{0, true});
    }
    const Entity entity{index, slots_[index].generation};
    alive_.push_back(entity);
    return entity;
}

void World::destroy(Entity entity) {
    if (!alive(entity)) {
        // 重复销毁或传入旧句柄：直接返回，避免同一槽位被二次回收。
        return;
    }
    // 先清组件，再让槽位失效，避免池中残留已销毁实体的数据。
    for (auto& [type, pool] : pools_) {
        (void)type;
        pool->remove(entity);
    }
    auto& slot = slots_[entity.index];
    slot.alive = false;
    // 关键一步：递增 generation，让外部仍持有的旧句柄变成过期句柄。
    ++slot.generation;
    freeList_.push_back(entity.index);
    // alive_ 也用 swap-and-pop 删除，所以 entities() 的遍历顺序并不稳定。
    if (const auto position = std::find(alive_.begin(), alive_.end(), entity); position != alive_.end()) {
        *position = alive_.back();
        alive_.pop_back();
    }
    // 槽位已经失效后再发布，订阅者不能拿这个句柄继续访问组件。
    if (eventBus_) {
        eventBus_->publish(EntityDestroyedEvent{entity});
    }
}

void World::clear() {
    // 清空实体与所有组件池，并解除事件总线绑定，使 World 可重新开始。
    slots_.clear();
    alive_.clear();
    freeList_.clear();
    pools_.clear();
    eventBus_ = nullptr;
}

bool World::alive(Entity entity) const {
    // 三个条件缺一不可：句柄格式有效、槽位存活、generation 完全一致。
    return entity.valid() && entity.index < slots_.size() && slots_[entity.index].alive &&
           slots_[entity.index].generation == entity.generation;
}

} // namespace l3d
