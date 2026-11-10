// 文件职责：file_utils 的实现，用标准库完成读写，Windows 专属调用集中在 _WIN32 分支内。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：所有失败都通过返回值表达，函数内部不抛异常；读写一律使用二进制模式。

#include "l3d/core/file_utils.h"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace l3d {

std::optional<std::string> readTextFile(const std::filesystem::path& path) {
    // 必须用 binary 模式：文本模式在 Windows 上会把 \r\n 悄悄转成 \n，破坏原始内容。
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    // 用流缓冲迭代器一次性读完，省去手工计算长度。
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::optional<std::vector<std::uint8_t>> readBinaryFile(const std::filesystem::path& path) {
    // ate 让打开后指针位于文件末尾，从而用 tellg 直接得到文件大小。
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return std::nullopt;
    }
    const auto size = stream.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    // 回到开头；空文件跳过 read，避免对空缓冲做零字节读取的特殊处理。
    stream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty() && !stream.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

bool writeTextFile(const std::filesystem::path& path, std::string_view contents) {
    // 先确保父目录存在：首次写存档/配置时，目标目录往往还不存在。
    // 使用 error_code 版本，目录创建失败时不会抛异常，而是返回 false。
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return false;
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    // trunc 覆盖旧内容；写完后检查流状态，磁盘满或权限不足都会在这里体现。
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return stream.good();
}

bool writeTextFileAtomic(const std::filesystem::path& path, std::string_view contents) {
    // 思路：先把新内容完整写进同目录的 .tmp，最后一次性 rename 覆盖目标。
    // 类比：先把信写好装进信封，最后一步才替换旧信；中途失败时旧文件仍然完整。
    const auto temporary = path.string() + ".tmp";
    if (!writeTextFile(temporary, contents)) {
        return false;
    }
    // 临时文件与目标同目录，通常处于同一文件系统，此时 rename 才是原子替换。
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        // 某些平台/文件系统上目标已存在时 rename 会失败，因此先删除旧文件再重试一次（兜底）。
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    return !error;
}

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    // 为什么用宽字符 API：Windows 的路径本质是 UTF-16，窄字符版本会受 ANSI 代码页限制，
    // 遇到中文或特殊字符路径就会失败。32768 覆盖启用长路径后的最大长度。
    std::wstring buffer(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size > 0 && size < buffer.size()) {
        // 返回值不含结尾的 \0，resize 正好去掉缓冲区里多余的空字符。
        buffer.resize(size);
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

std::string pathToUtf8(const std::filesystem::path& path) {
    // Windows 下 path 内部是宽字符，直接窄化会按当前代码页丢字符；u8string() 固定按 UTF-8 编码。
    // u8string() 返回 char8_t 序列，reinterpret_cast 只是把字节原样看成 char，不做转换。
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

} // namespace l3d
