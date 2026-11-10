#pragma once

// 文件职责：定义引擎与 C# 脚本插件之间的固定 C ABI 契约（版本、POD 结构、函数表、导出入口）。
// 所属模块：engine/scripting，是 engine_core/scene/physics 与外部脚本 DLL 之间唯一的边界。
// 分层位置：engine_app 创建 ScriptHost，ScriptHost 按本文件约定加载/调用 GameScripts.dll。
// 关键约束：任何布局或语义变化都要递增 ABI 版本，否则新旧 DLL 会静默读写错位。
#include <stdint.h>

// DLL 导出宏：脚本插件用它把入口函数暴露给引擎；Windows 用 __declspec，其他平台用默认可见性。
#if defined(_WIN32)
#define L3D_SCRIPT_EXPORT __declspec(dllexport)
#else
#define L3D_SCRIPT_EXPORT __attribute__((visibility("default")))
#endif

// ABI 版本：引擎加载 DLL 后会与 L3D_ScriptAbiVersion() 的返回值比对，不一致则拒绝加载。
// 修改结构体字段、函数表顺序或字符串约定时必须递增这个数字。
#define L3D_SCRIPT_ABI_VERSION 1u
// 无效实体哨兵：uint64 全 1，与有效实体编码 index | generation << 32 不会冲突。
#define L3D_INVALID_ENTITY UINT64_MAX

// extern "C" 关闭 C++ 的名字改编（name mangling），让导出函数名在二进制层面可预测，
// C# 的 P/Invoke 或 NativeAOT 才能按名字找到它们。
#ifdef __cplusplus
extern "C" {
#endif

// 纯 POD 结构体，只有 float 成员，布局与 C# 的 blittable 结构一一对应；
// 不能加入构造函数、虚函数或 std:: 类型，否则跨语言布局不再一致。
typedef struct L3D_Vec3 {
    float x;
    float y;
    float z;
} L3D_Vec3;

typedef struct L3D_Vec2 {
    float x;
    float y;
} L3D_Vec2;

// 键码是 ABI 的一部分，数值与引擎内部 l3d::Key 完全对应，增删必须同步两侧。
typedef enum L3D_KeyCode {
    L3D_KEY_W = 1,
    L3D_KEY_A = 2,
    L3D_KEY_S = 3,
    L3D_KEY_D = 4,
    L3D_KEY_Q = 5,
    L3D_KEY_E = 6,
    L3D_KEY_SPACE = 7,
    L3D_KEY_LEFT_SHIFT = 8,
    L3D_KEY_ESCAPE = 9,
    L3D_KEY_DELETE = 10,
    L3D_KEY_ENTER = 11,
    L3D_KEY_TAB = 12,
    L3D_KEY_1 = 13,
    L3D_KEY_2 = 14,
    L3D_KEY_3 = 15,
    L3D_KEY_F = 16,
    L3D_KEY_R = 17
} L3D_KeyCode;

// 日志级别数值固定，脚本只能通过这三档把消息转给引擎的 Logger。
typedef enum L3D_LogLevel {
    L3D_LOG_INFO = 0,
    L3D_LOG_WARNING = 1,
    L3D_LOG_ERROR = 2
} L3D_LogLevel;

// 鼠标按键编号固定为 0/1/2，对应引擎内部的 MouseButton。
typedef enum L3D_MouseButton {
    L3D_MOUSE_LEFT = 0,
    L3D_MOUSE_RIGHT = 1,
    L3D_MOUSE_MIDDLE = 2
} L3D_MouseButton;

// 定长脚本名缓冲区：最多容纳 63 个字符加结尾 '\0'；跨语言传递时不保证一定有终止符，
// 接收方应结合数组长度或显式长度字段读取。
typedef struct L3D_ScriptInfo {
    char name[64];
} L3D_ScriptInfo;

// 引擎函数表：脚本不静态链接引擎，而是持有一组指向引擎函数的指针，通过它们访问 World/物理/输入。
// 为什么用函数指针：C#/C 无法理解 C++ 类布局，函数指针 + void* userData 两边都能稳定表达。
// 字段顺序与类型一旦发布就冻结；abiVersion/structSize 供以后做兼容性检查。
// 返回 int32 的接口约定 1 = 成功/真，0 = 失败/假；字符串统一用“指针 + 长度”传递。
typedef struct L3D_EngineApiV1 {
    // 头部元数据：abiVersion 必须等于引擎期望版本；structSize 是整张表大小，用于发现结构截断。
    // userData 由引擎填充（实际指向 ScriptHost），会原样传给每个回调，脚本只能透传不能解释。
    uint32_t abiVersion;
    uint32_t structSize;
    void* userData;

    // 以下都是 C 函数指针：没有 this 指针、不能抛 C++ 异常、参数只能出现本文件的 POD 类型。
    // 末尾的指针参数是输出参数，由调用方准备内存，引擎实现会检查空指针并返回失败。
    void (*logMessage)(void* userData, int32_t level, const char* message, uint32_t length);
    int32_t (*getPosition)(void* userData, uint64_t entity, L3D_Vec3* outPosition);
    void (*setPosition)(void* userData, uint64_t entity, L3D_Vec3 position);
    int32_t (*getRotationEuler)(void* userData, uint64_t entity, L3D_Vec3* outEuler);
    void (*setRotationEuler)(void* userData, uint64_t entity, L3D_Vec3 euler);
    int32_t (*getVelocity)(void* userData, uint64_t entity, L3D_Vec3* outVelocity);
    void (*setVelocity)(void* userData, uint64_t entity, L3D_Vec3 velocity);
    int32_t (*isGrounded)(void* userData, uint64_t entity);
    int32_t (*isKeyDown)(void* userData, int32_t key);
    uint64_t (*findEntityByTag)(void* userData, const char* tag, uint32_t length);
    int32_t (*hasCharacterController)(void* userData, uint64_t entity);
    void (*destroyEntity)(void* userData, uint64_t entity);
    uint64_t (*createEntity)(void* userData, const char* tag, uint32_t length);
    int32_t (*raycast)(void* userData, L3D_Vec3 origin, L3D_Vec3 direction,
                       float maxDistance, uint64_t ignoreEntity, uint64_t* outEntity,
                       float* outDistance, L3D_Vec3* outPoint);
    int32_t (*isMouseButtonDown)(void* userData, int32_t button);
    void (*getMouseDelta)(void* userData, L3D_Vec2* outDelta);
    void (*getMousePosition)(void* userData, L3D_Vec2* outPosition);
    float (*getMouseScrollDelta)(void* userData);
    int32_t (*getScale)(void* userData, uint64_t entity, L3D_Vec3* outScale);
    void (*setScale)(void* userData, uint64_t entity, L3D_Vec3 scale);
} L3D_EngineApiV1;

// 脚本 DLL 必须导出的入口，加载顺序为：AbiVersion → Initialize → Catalog，
// 之后引擎按需调用 Create/Update/Collision/Trigger/Destroy，卸载前调用 Shutdown。
// 这些函数由插件实现、引擎调用；C++ 异常绝对不能越过 DLL 边界，插件内部必须自行捕获。
L3D_SCRIPT_EXPORT uint32_t L3D_ScriptAbiVersion(void);
// 把函数表交给插件；插件保存 api 指针后即可回调引擎。返回 0 表示成功，非 0 表示失败。
L3D_SCRIPT_EXPORT int32_t L3D_ScriptInitialize(const L3D_EngineApiV1* api);
// 查询脚本类型：entries 为 null 时返回总数；否则最多写入 capacity 项并返回实际写入数。
L3D_SCRIPT_EXPORT uint32_t L3D_ScriptCatalog(L3D_ScriptInfo* entries, uint32_t capacity);
// 按脚本名创建实例并绑定到 entity；返回非 0 句柄，0 表示创建失败。properties 是不透明字符串。
L3D_SCRIPT_EXPORT int64_t L3D_ScriptCreate(const char* name, uint32_t nameLength,
                                           uint64_t entity, const char* properties,
                                           uint32_t propertiesLength);
// 每帧回调一次，delta 为距上一帧的秒数；句柄在 Destroy 之前保持有效。
L3D_SCRIPT_EXPORT void L3D_ScriptUpdate(int64_t handle, float delta);
// 碰撞事件：other 是另一方实体的 ABI 编码，enter 为 1 表示进入、0 表示离开。
L3D_SCRIPT_EXPORT void L3D_ScriptCollision(int64_t handle, uint64_t other, int32_t enter);
// 触发区事件，参数含义与 Collision 相同。
L3D_SCRIPT_EXPORT void L3D_ScriptTrigger(int64_t handle, uint64_t other, int32_t enter);
// 销毁单个实例并释放其状态；调用后该句柄不可再使用。
L3D_SCRIPT_EXPORT void L3D_ScriptDestroy(int64_t handle);
// DLL 卸载前调用一次，用于清理插件的全局状态（不是销毁实例，实例应先 Destroy）。
L3D_SCRIPT_EXPORT void L3D_ScriptShutdown(void);

#ifdef __cplusplus
}
#endif
