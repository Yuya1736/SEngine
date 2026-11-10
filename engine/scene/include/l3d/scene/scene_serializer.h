#pragma once

// 场景 JSON 读写接口：engine_scene 模块负责持久化 World 的可编辑状态。
// 分层位置：engine_app 组合根调用本接口加载/保存，并在加载成功后替换当前 World。
// 约束：失败通过 Result 返回而不抛出；加载失败不得产生半成品 World。
#include <filesystem>
#include <memory>

#include "l3d/core/result.h"

namespace l3d {

class World;

// 无状态序列化器：load 创建新 World，save 只读取现有 World；当前格式版本为 1。
class SceneSerializer {
public:
    // 读取并校验场景文件，成功时返回全新 World；任何数据错误都转为 failure。
    [[nodiscard]] static Result<std::unique_ptr<World>> load(const std::filesystem::path& path);
    // 以原子替换方式写回文件，成功时返回 true；不修改传入的 World。
    [[nodiscard]] static Result<bool> save(const World& world, const std::filesystem::path& path);
};

} // namespace l3d
