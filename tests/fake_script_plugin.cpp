// 测试用的“假脚本插件”：只导出脚本 ABI 要求的符号，内部全部是空实现。
// 它的 ABI 版本故意声明为 2（引擎当前期望 1），用于验证 ScriptHost 在版本不匹配时
// 拒绝加载且不崩溃；由 CMake 编译成 fake_script_plugin.dll 供集成测试使用。
#include <cstdint>

// 各平台导出动态库符号的宏：Windows 用 __declspec(dllexport)，其他平台用默认可见性。
#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif

// 插件是独立二进制，这里故意不包含引擎头文件，只按 ABI 定义最小结构。
// 引擎会在版本检查处就拒绝本插件，所以这些结构的内容不会被真正使用。
struct L3D_EngineApiV1;
struct L3D_ScriptInfo {
    char name[64];
};

extern "C" {

// 故意返回 2：与引擎的 L3D_SCRIPT_ABI_VERSION（当前为 1）不一致，触发加载失败分支。
TEST_EXPORT std::uint32_t L3D_ScriptAbiVersion() {
    return 2;
}

TEST_EXPORT std::int32_t L3D_ScriptInitialize(const L3D_EngineApiV1*) {
    return 0;
}

// 目录与创建接口返回 0：0 条脚本、无效句柄。其他函数只是满足“符号齐全”的检查，
// 保证加载流程能走到版本比较这一步，从而精确测试版本不匹配行为。
TEST_EXPORT std::uint32_t L3D_ScriptCatalog(L3D_ScriptInfo*, std::uint32_t) {
    return 0;
}

TEST_EXPORT std::int64_t L3D_ScriptCreate(const char*, std::uint32_t, std::uint64_t,
                                          const char*, std::uint32_t) {
    return 0;
}

TEST_EXPORT void L3D_ScriptUpdate(std::int64_t, float) {}

TEST_EXPORT void L3D_ScriptCollision(std::int64_t, std::uint64_t, std::int32_t) {}

TEST_EXPORT void L3D_ScriptTrigger(std::int64_t, std::uint64_t, std::int32_t) {}

TEST_EXPORT void L3D_ScriptDestroy(std::int64_t) {}

TEST_EXPORT void L3D_ScriptShutdown() {}

} // extern "C"
