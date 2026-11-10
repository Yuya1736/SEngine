#pragma once

// 文件职责：声明编辑器 Editor 的公开接口；所属模块 engine/editor，只做 UI 逻辑，不实现引擎功能。
// 分层位置：engine_app（组合根）创建并驱动 Editor；Editor 站在 core/scene/renderer 之上，
// 只通过抽象接口依赖这些模块，并把用户操作翻译成请求交回 Application 执行。
// 关键约束：绘制阶段不得直接销毁实体或切换场景，只能写入 EditorRequests 请求队列。
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "l3d/renderer/render_types.h"
#include "l3d/scene/entity.h"

namespace l3d {

class IRenderer;
class IAssetDatabase;
class IScriptHost;
class World;
struct InputSnapshot;

// 一帧绘制中累积的“用户意图”。请求队列是 UI 与引擎之间的单向缓冲：
// UI 只写标志，Application 在绘制结束后读取并执行真正会改变引擎状态的操作。
struct EditorRequests {
    bool play = false;
    bool stop = false;
    bool saveScene = false;
    bool loadScene = false;
    bool newScene = false;
    bool exit = false;
};

// 编辑器主类：拥有全部 ImGui 面板和视口自由相机，但不拥有 World 与渲染资源。
// 不变量：requests_ 在每次 draw() 开始时清空，只反映本帧操作；
// selected_ 可能指向已销毁实体，使用前必须用 World::alive 校验。
class Editor {
public:
    // 依赖均以引用保存，生命周期由组合根 Application 保证长于 Editor。
    Editor(IAssetDatabase& assets, IRenderer& renderer, const IScriptHost& scripts,
           const InputSnapshot& input);

    // 每帧调用一次：绘制菜单栏、工具栏和各个面板，并写入 requests()。
    void draw(World& world, bool playing);
    // 用视口相机从 World 提取一帧渲染数据（可能刷新世界变换），不改变场景结构。
    [[nodiscard]] RenderFrame buildRenderFrame(World& world) const;
    // 场景被整体替换（新建/加载）后通知编辑器；若选中实体已失效则清空选中。
    void notifyWorldReplaced(World& world);

    // 返回本帧累积的请求；Application 读取后应自行清空或等待下一帧覆盖。
    [[nodiscard]] EditorRequests& requests() noexcept { return requests_; }
    [[nodiscard]] const EditorRequests& requests() const noexcept { return requests_; }
    [[nodiscard]] int viewportWidth() const noexcept { return viewportWidth_; }
    [[nodiscard]] int viewportHeight() const noexcept { return viewportHeight_; }
    // 当前选中实体，可能为无效句柄（未选中或原实体已销毁）。
    [[nodiscard]] Entity selectedEntity() const noexcept { return selected_; }
    // 外部（如 Application 或测试）同步选中项；只改选择，不触发任何引擎操作。
    void select(Entity entity) noexcept { selected_ = entity; }

    // 当前场景文件路径，仅用于显示和保存；可为空。
    [[nodiscard]] const std::string& scenePath() const noexcept { return scenePath_; }
    void setScenePath(std::string path) { scenePath_ = std::move(path); }

private:
    // 各面板绘制函数：读 World 生成控件，用户操作只写 requests_ 或选中项。
    void drawMenuBar(World& world, bool playing);
    void drawToolbar(World& world, bool playing);
    void drawHierarchy(World& world);
    void drawInspector(World& world);
    void drawAssetBrowser(World& world);
    void drawConsole();
    void drawViewport(World& world);
    // 递归绘制层级树；删除意图先写入 deleteRequest，稍后统一执行，避免边遍历边删。
    void drawEntityTree(World& world, Entity entity, Entity& deleteRequest);
    void drawAddComponentMenu(World& world, Entity entity);
    // 右键按住时按 WASD/QE 移动视口相机；hovered 表示鼠标确实位于视口图像上。
    void updateCamera(World& world, bool hovered);
    // 鼠标左键点击视口时，以相机射线对所有可见 MeshRenderer 的 AABB 做最近命中拾取。
    void pickEntity(World& world, float normalizedX, float normalizedY, float aspect);

    // 视口相机的旋转/视图/投影矩阵，三者必须使用同一组 yaw/pitch/fov 参数。
    [[nodiscard]] glm::quat cameraRotation() const;
    [[nodiscard]] glm::mat4 viewMatrix() const;
    [[nodiscard]] glm::mat4 projectionMatrix(float aspect) const;

    // 引用成员：生命周期由 Application 保证，Editor 不拥有它们。
    IAssetDatabase& assets_;
    IRenderer& renderer_;
    const IScriptHost& scripts_;
    const InputSnapshot& input_;
    // 请求队列与选中状态；deleteRequest_ 仅在单次 draw() 内有效。
    EditorRequests requests_{};
    Entity selected_{};
    Entity deleteRequest_{};

    // 视口自由相机：yaw 绕世界 Y 轴，pitch 为俯仰角，均以角度保存便于 UI 显示。
    glm::vec3 cameraPosition_{0.0f, 4.0f, 10.0f};
    float cameraYawDegrees_ = -90.0f;
    float cameraPitchDegrees_ = -15.0f;
    float cameraSpeed_ = 7.0f;
    float fovDegrees_ = 60.0f;
    float nearClip_ = 0.1f;
    float farClip_ = 500.0f;
    int viewportWidth_ = 960;
    int viewportHeight_ = 540;
    bool viewportHovered_ = false;
    std::string scenePath_;
    std::string selectedAssetGuid_;
};

} // namespace l3d
