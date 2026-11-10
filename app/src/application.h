#pragma once

// 应用组合根（composition root）的头文件。
// Application 是唯一知道并创建全部具体子系统的位置：窗口、资产库、渲染器、物理、
// 脚本宿主、场景和编辑器都在这里装配。运行时模块之间不互相持有，靠这里注入协作。
// 这条规则让各模块可以单独替换/测试，也让依赖方向始终指向 engine_core。

#include <filesystem>
#include <memory>
#include <string>

#include "l3d/assets/asset_database.h"
#include "l3d/core/event_bus.h"
#include "l3d/core/fixed_timestep.h"
#include "l3d/physics/physics_world.h"
#include "l3d/platform/window.h"
#include "l3d/renderer/renderer.h"
#include "l3d/scene/editor_session.h"
#include "l3d/scripting/script_host.h"

namespace l3d {

class Editor;
class World;

// 启动参数：由 main.cpp 解析命令行后填入，Application 只消费不修改语义。
struct ApplicationConfig {
    bool runtimeMode = false;   // true = 直接运行场景；false = 带编辑器的编辑模式
    bool hiddenWindow = false;  // 隐藏窗口，供 CI/冒烟测试使用
    bool vsync = true;          // 是否开启垂直同步（性能测试通常关闭）
    int maxFrames = 0;          // >0 时渲染指定帧数后自动退出
    std::filesystem::path assetRoot;     // 资产根目录；为空时自动探测
    std::filesystem::path scenePath;     // 启动场景；为空时使用默认样例场景
    std::filesystem::path screenshotPath; // 非空时在若干帧后写出截图
};

// 应用外壳：拥有主循环，并区分 Editor 与 Runtime 两种运行形态。
//
// 生命周期：构造（订阅事件） -> run() -> initialize() -> 逐帧 update/render -> shutdown()。
// 成员按声明顺序构造、按逆声明顺序析构；window_ 声明得早因而析构得晚，
// 可以保证依赖它的渲染器/编辑器对象先释放，避免访问已销毁的 GL 上下文。
class Application {
public:
    explicit Application(ApplicationConfig config);
    ~Application(); // RAII：即使 run() 提前返回也会调用 shutdown()

    // 初始化并进入主循环；返回进程退出码（0 成功，非 0 失败）。
    [[nodiscard]] int run();

private:
    [[nodiscard]] bool initialize(); // 按依赖顺序创建并装配各子系统
    void shutdown();                 // 与 initialize 相反顺序释放资源
    [[nodiscard]] bool loadScene(const std::filesystem::path& path); // 读场景并替换当前 World
    void createDefaultScene();       // 场景加载失败时的兜底场景
    void processEditorRequests();    // 把编辑器 UI 产生的“请求”翻译成引擎操作
    void beginPlay();                // 进入 Play：拍快照、启动脚本、重置物理/时间
    void stopPlay();                 // 退出 Play：停止脚本并恢复播放前快照
    void stepSimulation(double frameDelta); // 固定步长推进脚本与物理
    [[nodiscard]] RenderFrame buildRuntimeFrame() const; // Runtime 模式下的渲染提取
    [[nodiscard]] bool resolveAssetRoot(); // 依次尝试命令行、源码目录、可执行文件旁
    void initializeImGui();
    void buildDockLayout();          // 首次运行时创建 ImGui 停靠布局

    ApplicationConfig config_{};
    Window window_{};                // 平台层：GLFW 窗口 + 输入 + GL 上下文
    AssetDatabase assets_{};         // 资产 GUID -> 路径/数据
    OpenGLRenderer renderer_{};      // 只消费 RenderFrame，不接触 ECS
    SimplePhysicsWorld physics_{};   // 物理世界（Play/Runtime 时启用）
    ScriptHost scripts_{};           // C# 脚本 DLL 宿主
    std::unique_ptr<World> world_;   // 当前场景；Play 停止时整体换回快照
    EditorSession session_{};        // 管理 Play 前快照与播放状态
    std::unique_ptr<Editor> editor_; // 仅编辑模式存在
    FixedTimestep timestep_{};       // 固定 60Hz 累加器
    EventBus eventBus_{};            // 同步事件总线（单线程模型）
    std::filesystem::path currentScenePath_;
    double previousTime_ = 0.0;      // 上一帧时间，用于计算 frameDelta
    bool imguiInitialized_ = false;
    bool dockLayoutBuilt_ = false;
    bool screenshotCaptured_ = false; // 截图只写一次
};

} // namespace l3d
