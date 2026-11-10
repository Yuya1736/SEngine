#pragma once

// 文件职责：声明脚本宿主 IScriptHost 与实现 ScriptHost——加载 GameScripts.dll 并驱动脚本实例。
// 所属模块：engine/scripting，夹在组合根 engine_app 与外部脚本 DLL 之间。
// 分层位置：Application 每帧调用它；它通过 script_abi.h 的函数表把调用转给 DLL，
// 脚本只能经函数表反向访问引擎，看不到 World/物理/输入的真实 C++ 类型。
// 关键约束：DLL 生命周期、实例句柄与 World 指针必须严格配对，回调异常不得跨越 DLL 边界。
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "l3d/core/input.h"
#include "l3d/physics/physics_world.h"
#include "l3d/scripting/script_abi.h"
#include "l3d/scene/entity.h"

namespace l3d {

class World;

// 脚本宿主的抽象接口：Application 与 Editor 只依赖它，从而可以替换实现（如无脚本模式）。
// 生命周期不变量：start() 成功后 running() 为 true；stop() 后所有实例销毁且世界指针断开。
class IScriptHost {
public:
    virtual ~IScriptHost() = default;

    // 加载 DLL 并校验 ABI；已经加载时直接返回 true，不重复加载。
    [[nodiscard]] virtual bool load(const std::filesystem::path& dllPath) = 0;
    // 为世界内所有带 ScriptComponent 的实体创建脚本实例；若已有会话先 stop()。
    [[nodiscard]] virtual bool start(World& world, const IPhysicsWorld& physics,
                                     const InputSnapshot& input) = 0;
    // 每帧调用：先清理失效实例，再对存活实例调用脚本 update 回调。
    virtual void update(World& world, const InputSnapshot& input, float delta) = 0;
    // 把本帧物理碰撞/触发事件分发给相关实体上的脚本。
    virtual void dispatchEvents(World& world, const std::vector<CollisionEvent>& events,
                                const InputSnapshot& input) = 0;
    // 销毁全部实例并解除 World/Input 绑定；可重复调用，之后 running() 变为 false。
    virtual void stop() = 0;
    [[nodiscard]] virtual bool loaded() const noexcept = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    [[nodiscard]] virtual std::size_t instanceCount() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<std::string>& catalog() const noexcept = 0;
};

// 具体实现：一个 DLL 句柄 + 一组导出函数指针；每个回调都靠 userData 找回 ScriptHost 实例。
// 不变量：library_ 非空才可能调用导出函数；instances_ 与 handles_ 记录的实例集合始终一致。
class ScriptHost final : public IScriptHost {
public:
    ScriptHost() = default;
    // 析构顺序：先 stop() 销毁实例，再调用插件 Shutdown，最后卸载 DLL。
    ~ScriptHost() override;

    // 禁止拷贝：本类持有裸 DLL 句柄和外部指针，拷贝会导致重复卸载与双重所有权。
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    [[nodiscard]] bool load(const std::filesystem::path& dllPath) override;
    [[nodiscard]] bool start(World& world, const IPhysicsWorld& physics,
                             const InputSnapshot& input) override;
    void update(World& world, const InputSnapshot& input, float delta) override;
    void dispatchEvents(World& world, const std::vector<CollisionEvent>& events,
                        const InputSnapshot& input) override;
    void stop() override;

    [[nodiscard]] bool loaded() const noexcept override { return library_ != nullptr; }
    [[nodiscard]] bool running() const noexcept override { return world_ != nullptr; }
    [[nodiscard]] std::size_t instanceCount() const noexcept override {
        return instances_.size();
    }
    [[nodiscard]] const std::vector<std::string>& catalog() const noexcept override {
        return catalog_;
    }

    // 实体在 ABI 中只能以 uint64 传递：高 32 位 generation、低 32 位 index，
    // 这样解码时能还原完整句柄，也能让已销毁并复用的 index 不误命中旧实体。
    [[nodiscard]] static std::uint64_t encodeEntity(Entity entity);
    [[nodiscard]] static Entity decodeEntity(std::uint64_t value);

private:
    // 与 script_abi.h 导出函数一一对应的函数指针类型；签名必须逐字一致，否则调用约定会错位。
    using GetAbiVersionFn = std::uint32_t (*)();
    using InitializeFn = std::int32_t (*)(const L3D_EngineApiV1*);
    using CatalogFn = std::uint32_t (*)(L3D_ScriptInfo*, std::uint32_t);
    using CreateFn = std::int64_t (*)(const char*, std::uint32_t, std::uint64_t, const char*,
                                      std::uint32_t);
    using UpdateFn = void (*)(std::int64_t, float);
    using CollisionFn = void (*)(std::int64_t, std::uint64_t, std::int32_t);
    using TriggerFn = void (*)(std::int64_t, std::uint64_t, std::int32_t);
    using DestroyFn = void (*)(std::int64_t);
    using ShutdownFn = void (*)();

    // 一个脚本实例 = 所属实体 + 插件返回的实例句柄（0 表示创建失败，不会存入）。
    struct Instance {
        Entity entity{};
        std::int64_t handle = 0;
    };

    // 回调收到的是 void*，这里把它还原成 ScriptHost*；只有 initializeApi 填入的 this 才会传回。
    static ScriptHost* fromUserData(void* userData);
    // 记录当前帧可访问的 World/Input，函数表回调依赖它们；stop() 时传 nullptr 断开。
    void bind(World* world, const InputSnapshot* input);
    // 遍历实例并销毁“实体已不存在”的脚本实例，保持 instances_/handles_ 一致。
    void removeDestroyedInstances();
    // 填充 api_：把每个 C 函数指针指向实现文件中的静态函数/lambda，并设置 userData=this。
    void initializeApi();

    // DLL 句柄；下面的导出函数指针全部解析成功，才认为插件加载完成。
    void* library_ = nullptr;
    InitializeFn initialize_ = nullptr;
    CatalogFn catalogFn_ = nullptr;
    CreateFn create_ = nullptr;
    UpdateFn update_ = nullptr;
    CollisionFn collision_ = nullptr;
    TriggerFn trigger_ = nullptr;
    DestroyFn destroy_ = nullptr;
    ShutdownFn shutdown_ = nullptr;

    // 交给插件的函数表；userData 在 initializeApi 里指向 this。
    L3D_EngineApiV1 api_{};
    // DLL 报告的脚本类名，供编辑器下拉框和按名创建使用。
    std::vector<std::string> catalog_;
    // instances_ 保持顺序用于遍历；handles_ 用实体编码快速查找句柄（事件分发时需要）。
    std::vector<Instance> instances_;
    std::unordered_map<std::uint64_t, std::int64_t> handles_;
    // 这三个指针只在一次 start/update 会话内有效，由 bind()/stop() 维护。
    World* world_ = nullptr;
    const IPhysicsWorld* physics_ = nullptr;
    const InputSnapshot* input_ = nullptr;
};

} // namespace l3d
