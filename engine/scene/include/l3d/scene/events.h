#pragma once

// 场景层事件类型：由 engine_core 的 EventBus 同步发布。
// 分层位置：engine_app 组合根负责订阅并驱动编辑器，本模块只定义事件数据。
// 约束：事件是瞬时值，不携带裸指针；具体发布时机见各结构体注释。
#include <cstddef>
#include <filesystem>

#include "l3d/scene/entity.h"

namespace l3d {

// World::destroy 真正完成销毁后发布；此时句柄已失效，订阅者只能清理外部引用。
struct EntityDestroyedEvent {
    Entity entity{};
};

// 成功加载场景文件或建立默认场景后发布，记录路径和当时的实体数量。
struct SceneLoadedEvent {
    std::filesystem::path path;
    std::size_t entityCount = 0;
};

// 进入 Play 后发布 true；结束 Play 并恢复快照后发布 false。
struct PlayModeChangedEvent {
    bool playing = false;
};

} // namespace l3d
