// Play 会话实现：Play 前深拷贝 World，Stop 时交回原样快照。
// 所属 engine_scene 模块；engine_app 组合根负责在 Stop 后把快照换回当前 World。
// 关键约束：Play 中运行的是原 World，快照独立保存，运行期修改不会写回快照。
#include "l3d/scene/editor_session.h"

#include "l3d/scene/world.h"

namespace l3d {

bool EditorSession::beginPlay(World& scene) {
    if (backup_) {
        return false;
    }
    // 深拷贝整个场景：槽位、存活列表和所有组件池都复制，与运行中的 World 隔离。
    backup_ = std::make_unique<World>(scene);
    return true;
}

std::unique_ptr<World> EditorSession::endPlay() {
    // move 后 backup_ 为空，playing() 回到 false；快照所有权交给调用者恢复。
    return std::move(backup_);
}

} // namespace l3d
