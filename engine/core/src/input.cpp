// 文件职责：输入快照的实现，维护"持续状态"与"本帧边沿"两套数组。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：所有写入都先做下标越界检查；边沿只在状态真正变化时置位，避免重复触发。

#include "l3d/core/input.h"

namespace l3d {

namespace {
// 把枚举值直接当作数组下标使用；因此 Key/MouseButton 的数值必须连续且小于容量。
constexpr std::size_t keyIndex(Key key) {
    return static_cast<std::size_t>(key);
}

constexpr std::size_t mouseIndex(MouseButton button) {
    return static_cast<std::size_t>(button);
}
} // namespace

void InputSnapshot::beginFrame() {
    // 只清"本帧增量"：边沿事件、鼠标位移、滚轮增量都只属于刚刚结束的那一帧。
    // keysDown/mouseDown 是持续状态，必须保留，否则按住键会被误判为每帧新按下。
    keysPressed.fill(false);
    keysReleased.fill(false);
    mousePressed.fill(false);
    mouseReleased.fill(false);
    mouseDelta = {0.0f, 0.0f};
    scrollDelta = 0.0f;
}

void InputSnapshot::setKey(Key key, bool down) {
    // 0 是 Unknown，视为无效键，直接忽略。
    const auto index = keyIndex(key);
    if (index == 0 || index >= KeyCapacity) {
        return;
    }
    // 边沿检测：只在状态真正翻转时记录。按住不放只会让 keysDown 为 true，
    // 不会重复产生 pressed；连续两帧抬起同理。
    if (down && !keysDown[index]) {
        keysPressed[index] = true;
    } else if (!down && keysDown[index]) {
        keysReleased[index] = true;
    }
    keysDown[index] = down;
}

void InputSnapshot::setMouseButton(MouseButton button, bool down) {
    // 与键盘相同的边沿检测逻辑，只是下标范围更小。
    const auto index = mouseIndex(button);
    if (index >= MouseCapacity) {
        return;
    }
    if (down && !mouseDown[index]) {
        mousePressed[index] = true;
    } else if (!down && mouseDown[index]) {
        mouseReleased[index] = true;
    }
    mouseDown[index] = down;
}

// 下面一组查询函数都先做边界检查：越界枚举返回 false，而不是触发未定义行为。
bool InputSnapshot::isKeyDown(Key key) const {
    const auto index = keyIndex(key);
    return index < KeyCapacity && keysDown[index];
}

bool InputSnapshot::wasKeyPressed(Key key) const {
    const auto index = keyIndex(key);
    return index < KeyCapacity && keysPressed[index];
}

bool InputSnapshot::wasKeyReleased(Key key) const {
    const auto index = keyIndex(key);
    return index < KeyCapacity && keysReleased[index];
}

bool InputSnapshot::isMouseDown(MouseButton button) const {
    const auto index = mouseIndex(button);
    return index < MouseCapacity && mouseDown[index];
}

bool InputSnapshot::wasMousePressed(MouseButton button) const {
    const auto index = mouseIndex(button);
    return index < MouseCapacity && mousePressed[index];
}

bool InputSnapshot::wasMouseReleased(MouseButton button) const {
    const auto index = mouseIndex(button);
    return index < MouseCapacity && mouseReleased[index];
}

} // namespace l3d
