// 文件职责：Window 的实现——GLFW 初始化、上下文创建、输入回调转发与 GLAD 加载。
// 所属模块：engine/platform。处在 engine_app → runtime → engine_core 分层的最底层，
// 只依赖 GLFW/GLAD 与 engine_core 的日志，不依赖场景或渲染内容。
// 关键约束：必须先销毁窗口再终止 GLFW（glfwTerminate 会销毁所有窗口并解绑上下文）。
#include "l3d/platform/window.h"

#include <algorithm>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include "l3d/core/logging.h"

namespace l3d {

// 析构时自动释放窗口，保证提前返回或异常路径也不会泄漏。
Window::~Window() {
    shutdown();
}

bool Window::initialize(const WindowConfig& config) {
    // GLFW 是进程级全局库：初始化一次即可创建多个窗口。失败时直接返回，
    // 不设置 glfwInitialized_，避免后续误调用 glfwTerminate。
    if (!glfwInit()) {
        L3D_ERROR("GLFW initialization failed");
        return false;
    }
    glfwInitialized_ = true;

    // 所有 window hint 都必须在 glfwCreateWindow 之前设置，之后修改不会影响已有窗口。
    // 请求 OpenGL 4.1 Core Profile：Core 只暴露现代着色器接口，不含旧的固定管线；
    // 4.1 是 macOS 等平台能提供的较新版本，便于跨平台。
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // forward-compatible 会移除废弃 API，这里保持关闭，以兼容更多驱动实现。
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);
    // 隐藏窗口仍有完整的 GL 上下文，可用于自动化冒烟测试而不弹窗打扰。
    glfwWindowHint(GLFW_VISIBLE, config.visible ? GLFW_TRUE : GLFW_FALSE);

    // 给宽高设下限，防止传入 0 或负数导致非法视口或除零。
    width_ = std::max(config.width, 320);
    height_ = std::max(config.height, 240);
    window_ = glfwCreateWindow(width_, height_, config.title.c_str(), nullptr, nullptr);
    if (!window_) {
        L3D_ERROR("Failed to create GLFW window");
        // 走统一的清理路径，确保已经初始化的 GLFW 被终止。
        shutdown();
        return false;
    }

    // 把该窗口的上下文设为当前线程的上下文，之后所有 GL 调用都作用于它。
    glfwMakeContextCurrent(window_);
    // 把 this 存进窗口，回调里通过 glfwGetWindowUserPointer 取回。
    glfwSetWindowUserPointer(window_, this);
    // VSync：1 开启（等待垂直回扫），0 关闭（立即交换，帧率不受刷新率限制）。
    glfwSwapInterval(config.vsync ? 1 : 0);
    // 注册回调；GLFW 会在 pollEvents() 内部同步调用它们。
    glfwSetKeyCallback(window_, &Window::onKey);
    glfwSetMouseButtonCallback(window_, &Window::onMouseButton);
    glfwSetCursorPosCallback(window_, &Window::onCursorPosition);
    glfwSetScrollCallback(window_, &Window::onScroll);
    glfwSetFramebufferSizeCallback(window_, &Window::onFramebufferSize);
    glfwSetWindowSizeCallback(window_, &Window::onWindowSize);

    // GLAD 借助 glfwGetProcAddress 在运行时解析 OpenGL 函数地址；
    // Windows 的 opengl32 只导出 1.1 的旧符号，不能直接使用。
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        L3D_ERROR("Failed to load OpenGL functions");
        shutdown();
        return false;
    }

    // 帧缓冲尺寸必须单独查询：窗口刚创建时，它可能因 DPI 缩放不等于逻辑尺寸。
    glfwGetFramebufferSize(window_, &framebufferWidth_, &framebufferHeight_);
    L3D_INFO("Window created: {}x{}, OpenGL {}", framebufferWidth_, framebufferHeight_,
             reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}

// 先销毁窗口释放上下文，再终止 GLFW；两处判断让本函数可以重复调用。
void Window::shutdown() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (glfwInitialized_) {
        glfwTerminate();
        glfwInitialized_ = false;
    }
}

// 先推进输入帧（清掉上一帧的瞬时状态），再处理事件，保证回调写入属于本帧。
void Window::pollEvents() {
    input_.beginFrame();
    glfwPollEvents();
}

// 初始化失败或已 shutdown 时窗口为空，直接跳过，避免向 GLFW 传空指针。
// 典型主循环顺序：pollEvents() → 更新/渲染 → swapBuffers()。
void Window::swapBuffers() {
    if (window_) {
        glfwSwapBuffers(window_);
    }
}

// 窗口为空（未初始化或已销毁）也视为应当关闭，调用方无需额外判空。
bool Window::shouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

// 只设置关闭标志，不会立即跳出事件循环。
void Window::requestClose() {
    if (window_) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void Window::setTitle(const std::string& title) {
    if (window_) {
        glfwSetWindowTitle(window_, title.c_str());
    }
}

// 从 glfwInit 起算的单调时间，不受系统时钟回拨影响。
double Window::elapsedSeconds() const {
    return glfwGetTime();
}

void Window::onKey(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/) {
    // 回调的通用模式：先用 user pointer 找回 Window 实例；为空说明窗口已销毁。
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    const Key translated = translateKey(key);
    if (translated == Key::Unknown) {
        return;
    }
    // 只处理按下/释放；GLFW_REPEAT（长按自动重复）被忽略，避免重复触发“按下”。
    if (action == GLFW_PRESS) {
        self->input_.setKey(translated, true);
    } else if (action == GLFW_RELEASE) {
        self->input_.setKey(translated, false);
    }
}

void Window::onMouseButton(GLFWwindow* window, int button, int action, int /*mods*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    MouseButton translated{};
    // 只映射常用的三个按键，其余按键（例如侧键）被忽略。
    switch (button) {
    case GLFW_MOUSE_BUTTON_LEFT:
        translated = MouseButton::Left;
        break;
    case GLFW_MOUSE_BUTTON_RIGHT:
        translated = MouseButton::Right;
        break;
    case GLFW_MOUSE_BUTTON_MIDDLE:
        translated = MouseButton::Middle;
        break;
    default:
        return;
    }
    self->input_.setMouseButton(translated, action == GLFW_PRESS);
}

void Window::onCursorPosition(GLFWwindow* window, double x, double y) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    // 第一帧只记录基准位置，否则窗口出现瞬间的指针跳变会被当成巨大的鼠标位移。
    if (self->firstMouse_) {
        self->lastMouseX_ = x;
        self->lastMouseY_ = y;
        self->firstMouse_ = false;
    }
    const auto deltaX = static_cast<float>(x - self->lastMouseX_);
    const auto deltaY = static_cast<float>(y - self->lastMouseY_);
    self->lastMouseX_ = x;
    self->lastMouseY_ = y;
    // mousePosition 保留窗口坐标系（原点在左上角）的绝对位置。
    self->input_.mousePosition = {static_cast<float>(x), static_cast<float>(y)};
    // mouseDelta 是像素位移，y 取负：GLFW 的 y 轴向下，而引擎习惯向上为正。
    self->input_.mouseDelta += glm::vec2(deltaX, -deltaY);
}

void Window::onScroll(GLFWwindow* window, double xOffset, double yOffset) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    // 只使用垂直滚动量；xOffset 用 (void) 显式标记未使用，避免编译警告。
    self->input_.scrollDelta += static_cast<float>(yOffset);
    (void)xOffset;
}

void Window::onFramebufferSize(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    // 高 DPI 下帧缓冲像素尺寸可能大于逻辑尺寸（例如 2 倍缩放），两者分开保存。
    self->framebufferWidth_ = width;
    self->framebufferHeight_ = height;
}

void Window::onWindowSize(GLFWwindow* window, int width, int height) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (!self) {
        return;
    }
    // 窗口逻辑尺寸随拖动边框/最大化变化，用于宽高比与坐标换算。
    self->width_ = width;
    self->height_ = height;
}

// 把 GLFW 键码翻译成引擎自己的 Key 枚举，让上层不必依赖 GLFW 头文件。
// 未收录的键返回 Key::Unknown，由 onKey() 过滤掉，不会进入输入快照。
Key Window::translateKey(int key) {
    switch (key) {
    case GLFW_KEY_W:
        return Key::W;
    case GLFW_KEY_A:
        return Key::A;
    case GLFW_KEY_S:
        return Key::S;
    case GLFW_KEY_D:
        return Key::D;
    case GLFW_KEY_Q:
        return Key::Q;
    case GLFW_KEY_E:
        return Key::E;
    case GLFW_KEY_SPACE:
        return Key::Space;
    case GLFW_KEY_LEFT_SHIFT:
        return Key::LeftShift;
    case GLFW_KEY_ESCAPE:
        return Key::Escape;
    case GLFW_KEY_DELETE:
        return Key::Delete;
    case GLFW_KEY_ENTER:
        return Key::Enter;
    case GLFW_KEY_TAB:
        return Key::Tab;
    case GLFW_KEY_1:
        return Key::Num1;
    case GLFW_KEY_2:
        return Key::Num2;
    case GLFW_KEY_3:
        return Key::Num3;
    case GLFW_KEY_F:
        return Key::F;
    case GLFW_KEY_R:
        return Key::R;
    default:
        return Key::Unknown;
    }
}

} // namespace l3d
