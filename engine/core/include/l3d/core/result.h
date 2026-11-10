#pragma once

// 文件职责：Result<T> 值或错误的轻量容器，用返回值传播失败而不依赖异常。
// 所属模块：engine_core。
// 分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core，模块边界上的错误靠它传递。
// 关键约束：成功与失败互斥；失败时 error() 非空，成功时 value() 才可安全调用。

#include <optional>
#include <string>
#include <utility>

namespace l3d {

// 为什么不用异常：异常会越过模块边界，让调用方难以知道哪一层会抛、资源是否已清理；
// 用返回值把"失败"变成显式的、必须在调用点处理的普通值，模块之间的错误边界更清晰。
// 用法：if (auto r = load(); r) { use(r.value()); } else { log(r.error()); }
template <typename T>
class Result {
public:
    // 构造成功结果，持有值。
    static Result success(T value) {
        Result result;
        result.value_.emplace(std::move(value));
        return result;
    }

    // 构造失败结果，持有错误描述（约定为非空字符串）。
    static Result failure(std::string error) {
        Result result;
        result.error_ = std::move(error);
        return result;
    }

    // 判空约定：ok() 为真表示有值；也可直接 if (result) 使用。
    [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    // 只在 ok() 为真时调用；失败时调用会抛 bad_optional_access（这是编程错误，不是业务错误）。
    [[nodiscard]] const T& value() const { return value_.value(); }
    [[nodiscard]] T& value() { return value_.value(); }
    // 任何时候都可读取；成功结果返回空字符串。
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    std::optional<T> value_;
    std::string error_;
};

} // namespace l3d
