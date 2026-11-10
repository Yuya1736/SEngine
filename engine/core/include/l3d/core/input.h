#pragma once

// 文件职责：输入状态快照，保存每帧的按键/鼠标持续状态与按下、抬起边沿。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：边沿状态（pressed/released）只在发生变化的那一帧为 true，必须每帧调用 beginFrame。

#include <array>
#include <cstddef>

#include <glm/vec2.hpp>

namespace l3d {

// 键码。数值直接用作数组下标，因此必须保持连续且小于 InputSnapshot::KeyCapacity；
// 0（Unknown）被保留为无效键，setKey 会忽略它。
enum class Key : int {
    Unknown = 0,
    W = 1,
    A = 2,
    S = 3,
    D = 4,
    Q = 5,
    E = 6,
    Space = 7,
    LeftShift = 8,
    Escape = 9,
    Delete = 10,
    Enter = 11,
    Tab = 12,
    Num1 = 13,
    Num2 = 14,
    Num3 = 15,
    F = 16,
    R = 17,
};

// 鼠标键码，同样直接用作数组下标。
enum class MouseButton : std::size_t {
    Left = 0,
    Right = 1,
    Middle = 2,
};

// 每帧输入快照。Down 是"当前是否按住"的持续状态；
// Pressed/Released 是边沿事件：只在按下或抬起的那一帧为 true，适合做"按一次触发一次"。
struct InputSnapshot {
    static constexpr std::size_t KeyCapacity = 128;
    static constexpr std::size_t MouseCapacity = 8;

    std::array<bool, KeyCapacity> keysDown{};
    std::array<bool, KeyCapacity> keysPressed{};
    std::array<bool, KeyCapacity> keysReleased{};
    std::array<bool, MouseCapacity> mouseDown{};
    std::array<bool, MouseCapacity> mousePressed{};
    std::array<bool, MouseCapacity> mouseReleased{};
    // 本帧鼠标位移与滚轮增量，均为"本帧增量"而非累计值。
    glm::vec2 mouseDelta{0.0f, 0.0f};
    glm::vec2 mousePosition{0.0f, 0.0f};
    float scrollDelta = 0.0f;

    // 每帧开头调用：清零边沿事件和增量，Down 状态保持不变。
    void beginFrame();
    // 由平台事件回调写入最新状态，内部自动生成 Pressed/Released 边沿。
    void setKey(Key key, bool down);
    // 鼠标键的写入接口，逻辑与 setKey 相同。
    void setMouseButton(MouseButton button, bool down);

    // 越界的枚举值一律返回 false，调用方无需自己检查范围。
    [[nodiscard]] bool isKeyDown(Key key) const;
    [[nodiscard]] bool wasKeyPressed(Key key) const;
    [[nodiscard]] bool wasKeyReleased(Key key) const;
    [[nodiscard]] bool isMouseDown(MouseButton button) const;
    [[nodiscard]] bool wasMousePressed(MouseButton button) const;
    [[nodiscard]] bool wasMouseReleased(MouseButton button) const;
};

} // namespace l3d
