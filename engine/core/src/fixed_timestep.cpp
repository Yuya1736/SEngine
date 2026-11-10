// 文件职责：固定时间步长累加器的实现，核心是"累加时间 → 取整得步数 → 保留余数"。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：本文件只做时间换算，不驱动逻辑更新；单帧步数有上限，多余时间会被丢弃。

#include "l3d/core/fixed_timestep.h"

#include <algorithm>
#include <cmath>

namespace l3d {

// 参数兜底：至少追 1 步；单帧上限不得小于一个固定步，否则永远走不动。
FixedTimestep::FixedTimestep(double fixedDelta, int maxSteps, double maxFrameDelta)
    : fixedDelta_(fixedDelta), maxSteps_(std::max(1, maxSteps)),
      maxFrameDelta_(std::max(fixedDelta, maxFrameDelta)) {}

// 每渲染一帧调用一次。渲染帧间隔忽长忽短，而物理/游戏逻辑希望按固定节拍推进，
// 这里就是两者的"翻译器"：把不稳定的时间切成若干个等长的固定步。
StepResult FixedTimestep::advance(double frameDelta) {
    StepResult result;
    // 非法帧时间（NaN/Inf/非正）不推进逻辑，只回报插值系数；NaN 会永久污染累加器。
    if (!std::isfinite(frameDelta) || frameDelta <= 0.0) {
        result.alpha = accumulator_ / fixedDelta_;
        return result;
    }

    // 累加本帧耗时，但单帧最多只计 maxFrameDelta_ 秒，故意"忘掉"超出的部分。
    // 这能防止死亡螺旋：某帧特别卡（断点、加载）后，若把巨量时间全部补上，
    // 就要连续执行成百上千步，反而更卡，下一帧又继续积压，越追越远。
    accumulator_ += std::min(frameDelta, maxFrameDelta_);
    // 能凑出几个完整的固定步；这里只计数，不亲自调用 update，保持 core 不依赖上层业务。
    const auto availableSteps = static_cast<int>(std::floor(accumulator_ / fixedDelta_));
    // 本帧最多执行 maxSteps_ 步（追赶循环由调用方完成）。
    result.steps = std::min(availableSteps, maxSteps_);
    // 扣掉已执行的步，剩下的零头留在累加器里等下帧，这就是"余数"的含义。
    accumulator_ -= static_cast<double>(result.steps) * fixedDelta_;

    // 达到步数上限时把剩余积压直接清零，并标记 clamped，让上层知道逻辑被"压缩"了。
    // 否则累加器会一直超出上限，之后每一帧都只能追 maxSteps_ 步，状态永远追不上真实时间。
    if (availableSteps > maxSteps_) {
        accumulator_ = 0.0;
        result.clamped = true;
    }
    // alpha 是本帧余数占一个固定步的比例，渲染时用它在上一逻辑帧与下一逻辑帧之间插值。
    result.alpha = accumulator_ / fixedDelta_;
    return result;
}

void FixedTimestep::reset() noexcept {
    // 清空积压：切场景或从暂停恢复时，不能让老旧时间在恢复瞬间集中爆发。
    accumulator_ = 0.0;
}

} // namespace l3d
