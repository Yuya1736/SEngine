#pragma once

// 文件职责：窗口与输入采集的公共接口（GLFW 窗口、OpenGL 上下文、原始输入快照）。
// 所属模块：engine/platform。在 engine_app 组合根 → 各 runtime 模块 → engine_core 分层中，
// 向下封装操作系统/GLFW，向上为渲染与 runtime 提供窗口、帧缓冲尺寸和输入对象。
// 关键约束：必须先 initialize() 成功再调用其他非静态接口；窗口是独占资源，不可拷贝。

#include <cstdint>
#include <string>

#include "l3d/core/input.h"

struct GLFWwindow;

namespace l3d {

// 窗口创建参数。visible = false 时创建隐藏窗口：仍会创建 OpenGL 上下文，
// 适合自动化冒烟测试或不希望弹窗的启动方式。
struct WindowConfig {
    int width = 1280;
    int height = 720;
    std::string title = "L3D Engine";
    // 垂直同步：开启后交换缓冲要等待屏幕刷新，避免画面撕裂并把帧率限制在刷新率附近。
    bool vsync = true;
    bool visible = true;
};

// GLFW 窗口的 RAII 封装：负责 GL 上下文、输入采集与生命周期。
// 不变量：window_ 非空表示上下文已创建；析构函数会自动调用 shutdown()。
// 资源独占，因此禁止拷贝，否则会出现双重销毁。
class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // 初始化 GLFW、创建窗口、绑定 OpenGL 4.1 Core 上下文并加载 GLAD。
    // 失败时返回 false，并且已把申请到的资源清理干净。
    [[nodiscard]] bool initialize(const WindowConfig& config);
    // 销毁窗口并终止 GLFW；可重复调用。
    void shutdown();

    // 处理系统事件队列，并在同一处推进输入帧（清理上一帧的瞬时状态）。
    void pollEvents();
    // 交换前后缓冲；开启 VSync 时该调用会阻塞到下一次屏幕刷新。
    void swapBuffers();

    // 窗口收到关闭请求（或窗口不存在）时返回 true。
    [[nodiscard]] bool shouldClose() const;
    // 请求关闭窗口，效果在下一次 shouldClose() 时可见。
    void requestClose();
    // 修改窗口标题，可用于显示帧率等调试信息。
    void setTitle(const std::string& title);

    // 逻辑（窗口）尺寸，受 DPI 缩放影响，不一定等于帧缓冲像素尺寸。
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }
    // 帧缓冲像素尺寸：高 DPI 屏幕上可能是逻辑尺寸的 2 倍，glViewport 应使用这两个值。
    [[nodiscard]] int framebufferWidth() const noexcept { return framebufferWidth_; }
    [[nodiscard]] int framebufferHeight() const noexcept { return framebufferHeight_; }
    // GLFW 的单调计时（秒），常用于计算帧间隔。
    [[nodiscard]] double elapsedSeconds() const;
    // 输入快照的所有权归窗口：返回内部对象的引用，生命周期与 Window 相同。
    [[nodiscard]] InputSnapshot& input() noexcept { return input_; }
    [[nodiscard]] const InputSnapshot& input() const noexcept { return input_; }
    // 原生 GLFW 句柄，只在需要直接调用 GLFW 的底层模块中使用。
    [[nodiscard]] GLFWwindow* nativeHandle() const noexcept { return window_; }

private:
    // GLFW 回调是 C 函数指针，不能直接绑定成员函数，因此用静态函数接收，
    // 再通过 user pointer 找回 Window 实例。
    static void onKey(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow* window, int button, int action, int mods);
    static void onCursorPosition(GLFWwindow* window, double x, double y);
    static void onScroll(GLFWwindow* window, double xOffset, double yOffset);
    static void onFramebufferSize(GLFWwindow* window, int width, int height);
    static void onWindowSize(GLFWwindow* window, int width, int height);
    static Key translateKey(int key);

    GLFWwindow* window_ = nullptr;
    InputSnapshot input_{};
    int width_ = 0;
    int height_ = 0;
    int framebufferWidth_ = 0;
    int framebufferHeight_ = 0;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    // 首次收到鼠标移动时只记录基准位置，避免产生一个巨大的初始位移。
    bool firstMouse_ = true;
    bool glfwInitialized_ = false;
};

} // namespace l3d
