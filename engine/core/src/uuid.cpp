// 文件职责：生成与校验 UUID。资产靠 GUID 建立稳定引用，路径改名不影响引用关系。
// 所属模块：engine_core；分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core。
// 关键约束：使用随机数生成，不做全局唯一登记；格式为 36 字符小写十六进制加连字符。

#include "l3d/core/uuid.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <random>

namespace l3d {

std::string generateUuid() {
    // thread_local：每个线程各自持有一个随机引擎，避免共享引擎时的锁竞争；
    // 用 random_device 播种，保证不同线程/不同进程的序列不同。
    thread_local std::mt19937_64 engine(std::random_device{}());
    std::uniform_int_distribution<std::uint64_t> distribution;
    // 两次 64 位随机数拼成 16 个字节。
    const std::uint64_t high = distribution(engine);
    const std::uint64_t low = distribution(engine);

    std::array<unsigned char, 16> bytes{};
    // 按小端顺序把两个 64 位整数拆成独立的字节。
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<unsigned char>((high >> (index * 8)) & 0xff);
        bytes[8 + index] = static_cast<unsigned char>((low >> (index * 8)) & 0xff);
    }
    // RFC 4122 规定：第 6 字节高 4 位固定为 0100，表示"随机生成"的第 4 版；
    // 第 8 字节高 2 位固定为 10，表示使用标准变体。其余 122 位都是随机的。
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);

    // 36 个可见字符 + 结尾 \0，snprintf 比多次字符串拼接更直观也更省分配。
    char output[37]{};
    std::snprintf(output, sizeof(output),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                  bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return output;
}

bool isValidUuid(const std::string& value) noexcept {
    // 标准格式固定为 8-4-4-4-12，共 36 个字符。
    if (value.size() != 36) {
        return false;
    }
    // 连字符只允许出现在 8、13、18、23 四个位置，其余必须是十六进制数字。
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!std::isxdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

} // namespace l3d
