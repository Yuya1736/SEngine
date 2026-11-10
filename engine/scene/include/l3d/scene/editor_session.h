#pragma once

// Play 会话：进入 Play 前深拷贝当前场景，Stop 时把快照交还给组合根。
// 分层位置：engine_app 是组合根，负责持有会话并替换 World；本模块只负责保存快照。
// 约束：beginPlay/endPlay 成对使用；Play 期间的修改不会回写快照。
#include <memory>

namespace l3d {

class World;

class EditorSession {
public:
    // 深拷贝 scene 作为快照并进入 Play；已经处于 Play 中时返回 false。
    [[nodiscard]] bool beginPlay(World& scene);
    // 结束 Play 并移交快照所有权；未在 Play 中时返回 nullptr。
    [[nodiscard]] std::unique_ptr<World> endPlay();
    [[nodiscard]] bool playing() const noexcept { return backup_ != nullptr; }

private:
    std::unique_ptr<World> backup_;
};

} // namespace l3d
