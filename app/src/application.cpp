// Application 实现：组合根 + 主循环 + Editor/Runtime 两种运行形态。
//
// 每帧的固定顺序（Runtime）：输入 -> 固定步长推进脚本/物理 -> 渲染提取 -> 渲染。
// 编辑模式额外插入 ImGui 构建与编辑器请求处理，Play 时复用同一条模拟路径。
//
// 关键约束：所有子系统都在主线程；模块间不互相持有，由这里编排。
#include "application.h"

#include <algorithm>
#include <cmath>

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"
#include "l3d/editor/editor.h"
#include "l3d/renderer/render_extraction.h"
#include "l3d/scene/components.h"
#include "l3d/scene/events.h"
#include "l3d/scene/scene_serializer.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/transform_system.h"
#include "l3d/scene/world.h"
#include "screenshot.h"

namespace l3d {

namespace {

// ImGui 停靠空间的唯一 ID，用于每帧找到同一个 DockSpace 节点。
constexpr const char* DockSpaceName = "L3DDockSpace";

// 由“朝向向量”构造四元数旋转；GLM 要求 up 不能与 direction 平行。
glm::quat lookRotation(const glm::vec3& direction) {
    return glm::quatLookAt(glm::normalize(direction), glm::vec3(0.0f, 1.0f, 0.0f));
}

} // namespace

// 构造时只做事件订阅：此时窗口/渲染器都还不存在，不能有任何副作用。
// 订阅的回调都只写日志，用来演示“事件总线把发布者与观察者解耦”。
Application::Application(ApplicationConfig config) : config_(std::move(config)) {
    eventBus_.subscribe<SceneLoadedEvent>([](const SceneLoadedEvent& event) {
        L3D_TRACE("SceneLoaded event: '{}' ({} entities)", pathToUtf8(event.path),
                  event.entityCount);
    });
    eventBus_.subscribe<EntityDestroyedEvent>([](const EntityDestroyedEvent& event) {
        L3D_TRACE("EntityDestroyed event: index={} generation={}", event.entity.index,
                  event.entity.generation);
    });
    eventBus_.subscribe<PlayModeChangedEvent>([](const PlayModeChangedEvent& event) {
        L3D_INFO("Play mode changed: {}", event.playing ? "playing" : "editing");
    });
}

Application::~Application() {
    shutdown();
}

// 按“显式参数 -> 源码树 -> exe 旁 -> 工作目录”的优先级查找资产根目录。
// 这样同一份程序在开发、构建目录和分发包中都能直接启动。
bool Application::resolveAssetRoot() {
    if (!config_.assetRoot.empty()) {
        if (std::filesystem::exists(config_.assetRoot)) {
            config_.assetRoot = std::filesystem::weakly_canonical(config_.assetRoot);
            return true;
        }
        L3D_ERROR("Requested asset directory does not exist: {}",
                  pathToUtf8(config_.assetRoot));
        return false;
    }

#ifdef L3D_SOURCE_ASSET_ROOT
    const std::filesystem::path sourceAssets(L3D_SOURCE_ASSET_ROOT);
    if (std::filesystem::exists(sourceAssets)) {
        config_.assetRoot = std::filesystem::weakly_canonical(sourceAssets);
        return true;
    }
#endif

    const auto executableAssets = executableDirectory() / "assets";
    if (std::filesystem::exists(executableAssets)) {
        config_.assetRoot = std::filesystem::weakly_canonical(executableAssets);
        return true;
    }
    const auto workingAssets = std::filesystem::current_path() / "assets";
    if (std::filesystem::exists(workingAssets)) {
        config_.assetRoot = std::filesystem::weakly_canonical(workingAssets);
        return true;
    }
    return false;
}

// 初始化顺序 = 依赖顺序：窗口（GL 上下文/输入）-> 资产 -> 渲染器 -> 脚本 -> 场景。
// 任一必需项失败都返回 false，让 run() 以非零退出码结束。
bool Application::initialize() {
    if (!resolveAssetRoot()) {
        L3D_ERROR("No asset directory found");
        return false;
    }

    WindowConfig windowConfig;
    windowConfig.width = config_.runtimeMode ? 1280 : 1440;
    windowConfig.height = config_.runtimeMode ? 720 : 900;
    windowConfig.title = config_.runtimeMode ? "L3D Engine Runtime" : "L3D Engine Editor";
    windowConfig.visible = !config_.hiddenWindow;
    windowConfig.vsync = config_.vsync;
    if (!window_.initialize(windowConfig)) {
        return false;
    }

    const auto scan = assets_.scan(config_.assetRoot);
    if (!scan) {
        L3D_ERROR("Asset scan failed: {}", scan.error());
        return false;
    }
    if (!renderer_.initialize(assets_)) {
        L3D_ERROR("Renderer initialization failed");
        return false;
    }

    if (!scripts_.load(executableDirectory() / "GameScripts.dll")) {
        // 脚本是可选能力：找不到 DLL 时降级为纯渲染/物理，场景仍可打开。
        L3D_WARN("Gameplay scripting is unavailable; scenes will still load and render");
    }
    if (!config_.scenePath.empty()) {
        currentScenePath_ = config_.scenePath;
    } else {
        currentScenePath_ = config_.assetRoot / "scenes" / "sample.scene.json";
    }
    if (!loadScene(currentScenePath_)) {
        L3D_WARN("Falling back to generated default scene");
        createDefaultScene();
    }

    if (!config_.runtimeMode) {
        // 编辑模式：创建 Editor 并初始化 ImGui 后端（GLFW + OpenGL3）。
        editor_ = std::make_unique<Editor>(assets_, renderer_, scripts_, window_.input());
        editor_->setScenePath(currentScenePath_.string());
        initializeImGui();
    } else {
        // Runtime 模式：没有编辑器 UI，直接以“已开始播放”的状态启动玩法。
        physics_.reset();
        timestep_.reset();
        if (!scripts_.start(*world_, physics_, window_.input())) {
            L3D_WARN("Runtime started without gameplay scripts");
        }
    }
    previousTime_ = window_.elapsedSeconds();
    return true;
}

void Application::initializeImGui() {
    // 创建 ImGui 上下文并启用 Docking；IniFilename=nullptr 表示不持久化窗口布局，
    // 每次启动都由 buildDockLayout() 重建固定布局。
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 5.0f;
    style.FrameRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.105f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.065f, 0.085f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.31f, 0.42f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window_.nativeHandle(), true);
    ImGui_ImplOpenGL3_Init("#version 410 core");
    imguiInitialized_ = true;
}

void Application::buildDockLayout() {
    // 用 DockBuilder 手工切分停靠区域：左侧 Hierarchy/Assets，右侧 Inspector，
    // 底部 Console/Toolbar，中间 Viewport。只在首帧构建一次。
    const ImGuiID dockspaceId = ImGui::GetID(DockSpaceName);
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID main = dockspaceId;
    ImGuiID left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.20f, nullptr, &main);
    ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.26f, nullptr, &main);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.26f, nullptr, &main);
    ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.48f, nullptr, &left);

    ImGui::DockBuilderDockWindow("Toolbar", bottom);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Assets", leftBottom);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", main);
    ImGui::DockBuilderFinish(dockspaceId);
    dockLayoutBuilt_ = true;
}

void Application::shutdown() {
    // 顺序很重要：先停脚本（回调不能再访问 World/物理），再拆 UI 后端，
    // 最后销毁渲染器和窗口（GL 上下文的拥有者）。析构函数也会调用这里，必须可重入。
    scripts_.stop();
    if (imguiInitialized_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized_ = false;
    }
    renderer_.shutdown();
    window_.shutdown();
    editor_.reset();
    world_.reset();
}

bool Application::loadScene(const std::filesystem::path& path) {
    // 先用“新 World 整体替换”的方式加载：加载失败时旧场景保持不变，
    // 避免半加载状态（强异常安全/错误隔离）。
    auto loaded = SceneSerializer::load(path);
    if (!loaded) {
        L3D_ERROR("Scene load failed: {}", loaded.error());
        return false;
    }
    world_ = std::move(loaded.value());
    currentScenePath_ = path;
    updateWorldTransforms(*world_);
    world_->setEventBus(&eventBus_);
    eventBus_.publish(SceneLoadedEvent{path, world_->entities().size()});
    if (editor_) {
        editor_->setScenePath(path.string());
        editor_->notifyWorldReplaced(*world_);
    }
    return true;
}

void Application::createDefaultScene() {
    // 兜底场景：相机、方向光、地面、一个立方体。只有这里直接 new/配置组件，
    // 用来保证资源缺失时程序仍能启动。
    world_ = std::make_unique<World>();

    const auto camera = world_->create();
    world_->add<NameComponent>(camera, {"Main Camera"});
    auto& cameraTransform = world_->add<Transform>(camera);
    cameraTransform.position = {0.0f, 3.2f, 9.0f};
    cameraTransform.rotation = lookRotation(glm::vec3(0.0f, 1.0f, 0.0f) - cameraTransform.position);
    world_->add<CameraComponent>(camera);

    const auto sun = world_->create();
    world_->add<NameComponent>(sun, {"Directional Light"});
    world_->add<Transform>(sun);
    world_->add<DirectionalLight>(sun, {{-0.45f, -1.0f, -0.3f}, {1.0f, 0.95f, 0.86f}, 1.1f});

    const auto ground = world_->create();
    world_->add<NameComponent>(ground, {"Ground"});
    auto& groundTransform = world_->add<Transform>(ground);
    groundTransform.position = {0.0f, -0.05f, 0.0f};
    groundTransform.scale = {24.0f, 1.0f, 24.0f};
    world_->add<MeshRenderer>(ground, {BuiltinPlaneGuid, BuiltinDefaultMaterialGuid, true});
    world_->add<BoxCollider>(ground, {{12.0f, 0.05f, 12.0f}, false});
    world_->add<TagComponent>(ground, {"Ground"});

    const auto cube = world_->create();
    world_->add<NameComponent>(cube, {"Cube"});
    auto& cubeTransform = world_->add<Transform>(cube);
    cubeTransform.position = {0.0f, 1.0f, 0.0f};
    world_->add<MeshRenderer>(cube, {BuiltinCubeGuid, BuiltinDefaultMaterialGuid, true});

    currentScenePath_ = config_.assetRoot / "scenes" / "untitled.scene.json";
    updateWorldTransforms(*world_);
    world_->setEventBus(&eventBus_);
    eventBus_.publish(SceneLoadedEvent{currentScenePath_, world_->entities().size()});
    if (editor_) {
        editor_->setScenePath(currentScenePath_.string());
        editor_->notifyWorldReplaced(*world_);
    }
}

void Application::beginPlay() {
    // EditorSession 先对当前 World 做深拷贝快照，Play 期间的修改都发生在这份副本上。
    if (!world_ || !session_.beginPlay(*world_)) {
        return;
    }
    if (!scripts_.start(*world_, physics_, window_.input())) {
        L3D_WARN("Play mode started without gameplay scripts");
    }
    physics_.reset();
    timestep_.reset();
    eventBus_.publish(PlayModeChangedEvent{true});
    if (!config_.runtimeMode) {
        window_.setTitle("L3D Engine Editor | PLAY");
    }
}

void Application::stopPlay() {
    // 先停脚本再恢复快照：脚本的 onDestroy 不能看到“半恢复”的世界。
    scripts_.stop();
    if (auto restored = session_.endPlay()) {
        world_ = std::move(restored);
        updateWorldTransforms(*world_);
        world_->setEventBus(&eventBus_);
        if (editor_) {
            editor_->notifyWorldReplaced(*world_);
        }
    }
    physics_.reset();
    timestep_.reset();
    eventBus_.publish(PlayModeChangedEvent{false});
    if (!config_.runtimeMode) {
        window_.setTitle("L3D Engine Editor");
    }
}

void Application::processEditorRequests() {
    // 编辑器 UI 不直接改动引擎，而是记录“请求”（保存/加载/播放/退出等），
    // 由组合根在 UI 构建完成后统一执行，避免在遍历/绘制中途销毁对象。
    if (!editor_ || !world_) {
        return;
    }
    const auto& requests = editor_->requests();
    if (requests.exit) {
        window_.requestClose();
    }
    if (requests.newScene) {
        if (session_.playing()) {
            stopPlay();
        }
        createDefaultScene();
    }
    if (requests.loadScene) {
        if (session_.playing()) {
            stopPlay();
        }
        const auto target = std::filesystem::path(editor_->scenePath());
        if (!loadScene(target)) {
            editor_->setScenePath(currentScenePath_.string());
        }
    }
    if (requests.saveScene) {
        if (session_.playing()) {
            L3D_WARN("Stop Play mode before saving the scene");
        } else if (currentScenePath_.empty()) {
            L3D_WARN("No scene path is selected");
        } else {
            const auto saved = SceneSerializer::save(*world_, currentScenePath_);
            if (!saved) {
                L3D_ERROR("Save failed: {}", saved.error());
            }
        }
    }
    if (requests.play && !session_.playing()) {
        beginPlay();
    }
    if (requests.stop && session_.playing()) {
        stopPlay();
    }
}

void Application::stepSimulation(double frameDelta) {
    if (!world_) {
        return;
    }
    // 固定时间步：frameDelta 可能对应 0..N 个 1/60s 步长。
    // 每个步长内先跑脚本再跑物理，保证脚本写入的意图能被同一帧物理消费。
    const auto step = timestep_.advance(frameDelta);
    std::vector<CollisionEvent> events;
    for (int index = 0; index < step.steps; ++index) {
        scripts_.update(*world_, window_.input(), static_cast<float>(timestep_.fixedDelta()));
        physics_.step(*world_, static_cast<float>(timestep_.fixedDelta()), events);
    }
    // 碰撞/触发事件在所有步长结束后统一分发，避免脚本在物理迭代中途修改世界。
    if (!events.empty()) {
        scripts_.dispatchEvents(*world_, events, window_.input());
    }
    // 物理与脚本只改局部 Transform；世界矩阵在这里统一重算一次。
    updateWorldTransforms(*world_);
}

RenderFrame Application::buildRuntimeFrame() const {
    // 找到标记为主相机的实体；场景没有相机时退回一个固定机位，保证画面不黑。
    const float aspect = window_.framebufferHeight() > 0
                             ? static_cast<float>(window_.framebufferWidth()) /
                                   window_.framebufferHeight()
                             : 1.0f;
    if (world_) {
        for (const auto entity : world_->entities()) {
            const auto* camera = world_->tryGet<CameraComponent>(entity);
            const auto* transform = world_->tryGet<Transform>(entity);
            if (camera && transform && camera->primary) {
                return extractRenderFrame(*world_, transform->position, transform->rotation,
                                          camera->fieldOfViewDegrees, camera->nearClip,
                                          camera->farClip, aspect);
            }
        }
    }
    const glm::vec3 position{0.0f, 3.2f, 9.0f};
    return extractRenderFrame(*world_, position, lookRotation(-position), 60.0f, 0.1f,
                              500.0f, aspect);
}

int Application::run() {
    if (!initialize()) {
        return 1;
    }

    // 主循环：所有工作都在主线程。frameDelta 上限 0.1s 防止断点/卡顿后
    // 固定步长追赶过多步（“死亡螺旋”），代价是极端情况下游戏时间变慢。
    int frameCount = 0;
    const double loopStart = window_.elapsedSeconds();
    while (!window_.shouldClose()) {
        window_.pollEvents();
        if (config_.runtimeMode && window_.input().wasKeyPressed(Key::Escape)) {
            window_.requestClose();
        }
        const double now = window_.elapsedSeconds();
        const double frameDelta = std::clamp(now - previousTime_, 0.0, 0.1);
        previousTime_ = now;

        if (config_.runtimeMode) {
            // Runtime：纯模拟 + 渲染，没有 ImGui。
            stepSimulation(frameDelta);
            const auto frame = buildRuntimeFrame();
            renderer_.render(frame, window_.framebufferWidth(), window_.framebufferHeight());
            renderer_.presentToDefaultFramebuffer(window_.framebufferWidth(),
                                                   window_.framebufferHeight());
        } else {
            // 编辑模式：先构建 UI，再根据 UI 产生的请求执行引擎操作。
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            if (!dockLayoutBuilt_) {
                buildDockLayout();
            }
            ImGui::DockSpaceOverViewport(ImGui::GetID(DockSpaceName), ImGui::GetMainViewport(),
                                         ImGuiDockNodeFlags_PassthruCentralNode);

            editor_->draw(*world_, session_.playing());
            processEditorRequests();
            if (session_.playing()) {
                // Play 模式复用与 Runtime 相同的固定步长路径。
                stepSimulation(frameDelta);
            } else {
                // 编辑态也要刷新世界矩阵，Inspector 里改了 Transform 才能立刻看到。
                updateWorldTransforms(*world_);
            }

            // 渲染器输出到离屏 FBO，再把 FBO 纹理作为 ImGui Viewport 面板内容显示。
            const auto frame = editor_->buildRenderFrame(*world_);
            renderer_.render(frame, editor_->viewportWidth(), editor_->viewportHeight());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, window_.framebufferWidth(), window_.framebufferHeight());
            glClearColor(0.045f, 0.055f, 0.075f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        if (!config_.screenshotPath.empty() && !screenshotCaptured_ && frameCount >= 2) {
            // 等到第 3 帧再截：前两帧可能还在初始化资源/布局。
            screenshotCaptured_ = writeDefaultFramebufferPng(
                config_.screenshotPath, window_.framebufferWidth(),
                window_.framebufferHeight());
        }
        window_.swapBuffers();
        ++frameCount;
        if (config_.maxFrames > 0 && frameCount >= config_.maxFrames) {
            window_.requestClose();
        }
    }
    const double loopSeconds = window_.elapsedSeconds() - loopStart;
    if (frameCount > 0 && loopSeconds > 0.0) {
        L3D_INFO("Rendered {} frames in {:.3f}s ({:.1f} FPS, vsync={})", frameCount,
                 loopSeconds, frameCount / loopSeconds, config_.vsync ? "on" : "off");
    }
    return 0;
}

} // namespace l3d
