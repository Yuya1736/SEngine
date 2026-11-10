#pragma once

// 文件职责：生成与校验 UUID（GUID）。资产用 GUID 而非路径/文件名做稳定引用，
// 这样重命名或移动文件后，引用依然有效（类似身份证号与人名的区别）。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：生成的是随机 v4 UUID，只保证概率唯一，不做全局登记。

#include <string>

namespace l3d {

// 生成 RFC 4122 第 4 版（随机）UUID，格式为 36 字符小写：8-4-4-4-12。
[[nodiscard]] std::string generateUuid();
// 只校验格式（长度、连字符位置、十六进制字符），不检查版本位和大小写。
[[nodiscard]] bool isValidUuid(const std::string& value) noexcept;

} // namespace l3d
