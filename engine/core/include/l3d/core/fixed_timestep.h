#pragma once

// 文件职责：固定时间步长累加器，把不稳定的渲染帧间隔换算成固定数量的逻辑步。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：advance 只负责"该走几步"，真正调用 update 的循环由上层完成，core 不依赖业务。

namespace l3d {

// 一次 advance 的产出：调用方按 steps 执行固定步，再用 alpha 做渲染插值。
struct StepResult {
    // 本帧应执行的固定步数。
    int steps = 0;
    // 剩余时间占一个固定步的比例 [0,1)，用于两次逻辑更新之间的平滑插值。
    double alpha = 0.0;
    // 为 true 表示时间被截断（长卡顿/断点），部分时间已被丢弃。
    bool clamped = false;
};

// 固定步长累加器：逻辑以固定的 fixedDelta_ 秒推进，与渲染帧率解耦。
// 不变量：maxSteps_ >= 1；每次 advance 返回后 accumulator_ 都落在 [0, fixedDelta_)。
class FixedTimestep {
public:
    // fixedDelta：一个逻辑步的长度；maxSteps：单帧最多追赶几步；
    // maxFrameDelta：单帧允许计入的最大时间，超出部分丢弃，防止"死亡螺旋"。
    explicit FixedTimestep(double fixedDelta = 1.0 / 60.0, int maxSteps = 5,
                           double maxFrameDelta = 0.25);

    // 喂入本帧真实耗时（秒），返回应执行的逻辑步数与插值系数；非法值会被忽略。
    StepResult advance(double frameDelta);
    // 清空累加器。切场景或暂停恢复后调用，避免一次性补上积压的大量步数。
    void reset() noexcept;

    [[nodiscard]] double fixedDelta() const noexcept { return fixedDelta_; }
    [[nodiscard]] double accumulator() const noexcept { return accumulator_; }

private:
    double fixedDelta_;
    int maxSteps_;
    double maxFrameDelta_;
    double accumulator_ = 0.0;
};

} // namespace l3d
