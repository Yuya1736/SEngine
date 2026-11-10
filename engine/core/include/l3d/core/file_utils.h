#pragma once

// 文件职责：跨平台的文本/二进制文件读写与路径工具，失败通过返回值表达而不抛异常。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：Windows 下 path 内部是宽字符，输出到日志/UI 前需经 pathToUtf8 转为 UTF-8。

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace l3d {

// 读取整个文本文件；失败返回 nullopt。以二进制方式读，不做换行或 BOM 转换。
[[nodiscard]] std::optional<std::string> readTextFile(const std::filesystem::path& path);
// 读取整个二进制文件；失败返回 nullopt。会先按文件大小分配缓冲，再一次性读入。
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readBinaryFile(const std::filesystem::path& path);
// 写入文本；自动创建父目录并覆盖已有内容，成功返回 true。
[[nodiscard]] bool writeTextFile(const std::filesystem::path& path, std::string_view contents);
// 原子写入：先写同目录的 .tmp 文件，再 rename 覆盖目标，避免中途失败留下残缺文件。
[[nodiscard]] bool writeTextFileAtomic(const std::filesystem::path& path, std::string_view contents);
// 返回可执行文件所在目录；Windows 取模块路径，其他平台回退到当前工作目录。
[[nodiscard]] std::filesystem::path executableDirectory();
// 把 path 转换为 UTF-8 字符串，用于日志、界面显示或跨平台序列化。
[[nodiscard]] std::string pathToUtf8(const std::filesystem::path& path);

} // namespace l3d
