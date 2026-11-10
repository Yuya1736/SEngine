// 文件职责：实现 ScriptHost：动态加载 GameScripts.dll、校验 ABI、创建/驱动/销毁 C# 脚本实例。
// 所属模块：engine/scripting；位于 engine_app 与外部 DLL 之间，是引擎侧唯一接触动态库的代码。
// 分层位置：Application 每帧调用 update/dispatchEvents；World/物理/输入只在本文件内被适配成 ABI。
// 关键约束：DLL 必须先加载再 start，先 stop 再卸载；脚本只能通过 api_ 函数表访问引擎。
#include "l3d/scripting/script_host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <glm/gtc/quaternion.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "l3d/core/logging.h"
#include "l3d/core/file_utils.h"
#include "l3d/scene/components.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/world.h"

namespace l3d {

namespace {

// 动态库边界：LoadLibrary 只把 DLL 映射进进程，还要用 GetProcAddress 按“导出函数名”取地址。
// 这里把地址 reinterpret_cast 成 script_abi.h 中声明的函数指针类型，签名不一致会在调用时崩。
template <typename Function>
Function loadProcedure(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<Function>(
        GetProcAddress(static_cast<HMODULE>(library), name));
#else
    // 非 Windows 平台尚未实现动态加载，返回空指针，让 load() 走统一的失败路径。
    (void)library;
    (void)name;
    return nullptr;
#endif
}

// Windows API 失败时不抛异常，只设置线程局部错误码；把它转成可读字符串方便日志排查。
std::string lastLibraryError() {
#ifdef _WIN32
    const auto code = GetLastError();
    return "Windows error " + std::to_string(code);
#else
    // 其它平台没有 GetLastError，返回固定说明即可。
    return "Unsupported platform";
#endif
}

} // namespace

// 卸载顺序很重要：先 stop() 让脚本收到 destroy 回调（此时 World/API 仍可用），
// 再调用插件的 Shutdown 清理全局状态，最后才 FreeLibrary；反过来会在已卸载代码上继续执行。
ScriptHost::~ScriptHost() {
    stop();
    if (library_) {
        if (shutdown_) {
            shutdown_();
        }
#ifdef _WIN32
        // FreeLibrary 只减少引用计数；所有加载者都释放后，DLL 才真正从进程卸载。
        FreeLibrary(static_cast<HMODULE>(library_));
#endif
        library_ = nullptr;
    }
}

bool ScriptHost::load(const std::filesystem::path& dllPath) {
    // load 是幂等的：已经加载过就直接成功，避免重复映射同一个 DLL。
    if (library_) {
        return true;
    }
#ifdef _WIN32
    // 用 LoadLibraryW + wstring：宽字符路径能正确处理中文或空格目录。
    library_ = static_cast<void*>(LoadLibraryW(dllPath.wstring().c_str()));
    if (!library_) {
        L3D_WARN("C# script plugin not loaded: {} ({})", pathToUtf8(dllPath),
                 lastLibraryError());
        return false;
    }
#else
    (void)dllPath;
    L3D_WARN("C# script plugin loading is only implemented on Windows");
    return false;
#endif

    // 按 script_abi.h 中的导出名逐个解析地址；任何一个缺失都说明插件不完整，立即卸载。
    auto getVersion = loadProcedure<GetAbiVersionFn>(library_, "L3D_ScriptAbiVersion");
    initialize_ = loadProcedure<InitializeFn>(library_, "L3D_ScriptInitialize");
    catalogFn_ = loadProcedure<CatalogFn>(library_, "L3D_ScriptCatalog");
    create_ = loadProcedure<CreateFn>(library_, "L3D_ScriptCreate");
    update_ = loadProcedure<UpdateFn>(library_, "L3D_ScriptUpdate");
    collision_ = loadProcedure<CollisionFn>(library_, "L3D_ScriptCollision");
    trigger_ = loadProcedure<TriggerFn>(library_, "L3D_ScriptTrigger");
    destroy_ = loadProcedure<DestroyFn>(library_, "L3D_ScriptDestroy");
    shutdown_ = loadProcedure<ShutdownFn>(library_, "L3D_ScriptShutdown");
    if (!getVersion || !initialize_ || !catalogFn_ || !create_ || !update_ || !collision_ ||
        !trigger_ || !destroy_ || !shutdown_) {
        L3D_ERROR("C# script plugin is missing required ABI exports");
        FreeLibrary(static_cast<HMODULE>(library_));
        library_ = nullptr;
        return false;
    }

    // 先校验版本再调用其它函数：版本不一致时结构体布局未知，继续调用会按错误偏移读写内存。
    const auto version = getVersion();
    if (version != L3D_SCRIPT_ABI_VERSION) {
        // 版本不一致时先卸载，避免不兼容的 DLL 继续留在进程地址空间里。
        L3D_ERROR("C# script plugin ABI mismatch: expected {}, got {}", L3D_SCRIPT_ABI_VERSION,
                  version);
        FreeLibrary(static_cast<HMODULE>(library_));
        library_ = nullptr;
        return false;
    }

    // 函数表必须在 Initialize 之前填好：插件初始化期间就可能通过回调访问引擎。
    initializeApi();
    const auto initResult = initialize_(&api_);
    if (initResult != 0) {
        // 初始化失败也卸载 DLL，保证 load() 返回 false 时内部没有半初始化状态。
        L3D_ERROR("C# script plugin initialization failed with code {}", initResult);
        FreeLibrary(static_cast<HMODULE>(library_));
        library_ = nullptr;
        return false;
    }

    // 两段式查询：第一次 entries 为 null 只取脚本数量，第二次才真正写入缓冲区。
    // 数量上限 256 是对异常/恶意 DLL 的防御，避免一次分配过大内存。
    const auto count = (std::min)(catalogFn_(nullptr, 0), 256u);
    std::vector<L3D_ScriptInfo> entries(count);
    const auto written = (std::min)(catalogFn_(entries.data(), count), count);
    catalog_.clear();
    for (std::uint32_t index = 0; index < written; ++index) {
        const auto& entry = entries[index];
        // 定长 char 数组不保证以 '\0' 结尾，因此显式查找终止符；找不到就取满 64 字节。
        catalog_.emplace_back(entry.name, std::find(entry.name, entry.name + sizeof(entry.name), '\0'));
    }
    // catalog_ 保存插件声明的脚本类名：编辑器下拉框显示它，按名创建也用它校验。
    L3D_INFO("Loaded C# script plugin with {} script types", catalog_.size());
    return true;
}

bool ScriptHost::start(World& world, const IPhysicsWorld& physics, const InputSnapshot& input) {
    // 没有 DLL 就没有可创建的脚本类型；这是“无脚本模式”，不是错误。
    if (!library_) {
        return false;
    }
    // 若上一轮 Play 还在运行，先正常销毁旧实例，避免句柄泄漏。
    if (world_) {
        stop();
    }
    // 这里保存的是裸指针：调用方必须保证 world/physics/input 在整个播放期间存活；
    // physics_ 只在这里绑定一次，world_/input_ 每次 update 会重新绑定当帧的输入快照。
    physics_ = &physics;
    bind(&world, &input);
    instances_.clear();
    handles_.clear();
    // start 每个 Play 会话调用一次：这里创建全部实例，直到 stop() 才统一销毁。
    for (const auto entity : world.entities()) {
        const auto* script = world.tryGet<ScriptComponent>(entity);
        if (!script || script->scriptName.empty()) {
            continue;
        }
        // 实体以 uint64 编码跨越 ABI；字符串显式传长度，避免依赖 '\0' 和 C# 的字符串布局。
        // 每个 Create 对应插件里的一次 onCreate；插件返回的 handle 是后续 Update/Destroy 的凭据，
        // 0 表示创建失败。
        const auto handle = create_(script->scriptName.c_str(),
                                    static_cast<std::uint32_t>(script->scriptName.size()),
                                    encodeEntity(entity), script->propertiesJson.c_str(),
                                    static_cast<std::uint32_t>(script->propertiesJson.size()));
        if (handle == 0) {
            L3D_WARN("Could not create script '{}' for entity {}", script->scriptName,
                     entity.index);
            continue;
        }
        // 两份索引各司其职：instances_ 按顺序遍历，handles_ 在事件到来时按实体编码快速查句柄。
        instances_.push_back({entity, handle});
        handles_[encodeEntity(entity)] = handle;
    }
    // 记录实例总数，方便排查“Play 后脚本为什么没动”这类问题。
    L3D_INFO("Started {} C# script instances", instances_.size());
    return true;
}

void ScriptHost::update(World& world, const InputSnapshot& input, float delta) {
    // 未加载 DLL 或尚未 start 时是安全的空操作，调用方无需额外判断。
    if (!library_ || !world_) {
        return;
    }
    // 每次 update 重新绑定输入快照：脚本通过函数表读到的永远是“当帧”的输入。
    bind(&world, &input);
    // delta 是从上一帧到现在的秒数，插件用它做与帧率无关的计时和移动。
    // 用索引循环而非范围 for：循环中会 erase 元素，删除后必须保持 index 不变。
    for (std::size_t index = 0; index < instances_.size();) {
        const auto instance = instances_[index];
        // 实体可能已被脚本或编辑器销毁，先检查再回调，避免把失效实体交给插件。
        if (!world.alive(instance.entity) || !world.has<ScriptComponent>(instance.entity)) {
            destroy_(instance.handle);
            handles_.erase(encodeEntity(instance.entity));
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        // 实例按创建时的遍历顺序依次 update（目前等价于实体表顺序），没有依赖拓扑排序：
        // 脚本之间若有先后依赖，需要自己通过标签/事件协调。
        // 调用插件的 onUpdate。这里没有 try/catch：异常一旦抛出就会越过 C ABI 边界，
        // 插件（C# 侧）必须自行捕获，不能让托管异常逃逸到原生栈。
        update_(instance.handle, delta);
        // 脚本可能在 update 中销毁自己的实体，因此在回调之后还要再检查一次。
        if (!world.alive(instance.entity)) {
            destroy_(instance.handle);
            handles_.erase(encodeEntity(instance.entity));
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

void ScriptHost::dispatchEvents(World& world, const std::vector<CollisionEvent>& events,
                                const InputSnapshot& input) {
    if (!library_ || !world_) {
        return;
    }
    // 事件列表由物理系统在当帧生成，这里只读不改；世界和输入与 update 一样重新绑定。
    bind(&world, &input);
    for (const auto& event : events) {
        // 触发区走 trigger 回调，实体碰撞走 collision 回调，两者签名相同。
        const auto callback = event.trigger ? trigger_ : collision_;
        // 只通知挂了脚本的一侧；alive 检查兜底事件产生后实体又被销毁的情况。
        const auto first = handles_.find(encodeEntity(event.first));
        if (first != handles_.end() && world.alive(event.first)) {
            callback(first->second, encodeEntity(event.second),
                     event.phase == TriggerPhase::Enter ? 1 : 0);
        }
        // 事件对双方是对称的：第二个实体收到回调时，other 参数换成第一个实体。
        // 注意第一个回调可能已经销毁了第二个实体，所以这里仍要检查 alive。
        const auto second = handles_.find(encodeEntity(event.second));
        if (second != handles_.end() && world.alive(event.second)) {
            callback(second->second, encodeEntity(event.first),
                     event.phase == TriggerPhase::Enter ? 1 : 0);
        }
    }
}

void ScriptHost::stop() {
    // 先逐个销毁实例再清空容器：脚本会收到 onDestroy；此时 world_ 仍绑定，回调还能访问引擎。
    if (!instances_.empty() && destroy_) {
        for (const auto& instance : instances_) {
            destroy_(instance.handle);
        }
    }
    instances_.clear();
    handles_.clear();
    // 最后断开全部指针，running() 随之变为 false，后续 update/dispatchEvents 都是空操作。
    physics_ = nullptr;
    bind(nullptr, nullptr);
}

std::uint64_t ScriptHost::encodeEntity(Entity entity) {
    // 无效句柄统一用 ABI 哨兵；有效句柄把 generation 放高位、index 放低位，
    // generation 能让“索引被复用的新实体”与旧引用区分开。
    if (!entity.valid()) {
        return L3D_INVALID_ENTITY;
    }
    return (static_cast<std::uint64_t>(entity.generation) << 32) | entity.index;
}

Entity ScriptHost::decodeEntity(std::uint64_t value) {
    // 按 encodeEntity 的相反顺序拆位；哨兵还原成默认（无效）Entity。
    if (value == L3D_INVALID_ENTITY) {
        return {};
    }
    return Entity{static_cast<std::uint32_t>(value & 0xffffffffu),
                  static_cast<std::uint32_t>(value >> 32)};
}

// userData 就是 initializeApi() 里写入的 this；只可能由引擎传入，因此不需要运行时类型检查。
ScriptHost* ScriptHost::fromUserData(void* userData) {
    return static_cast<ScriptHost*>(userData);
}

// 记录当前会话可访问的指针；传 nullptr 表示结束会话（见 stop()）。
void ScriptHost::bind(World* world, const InputSnapshot* input) {
    world_ = world;
    input_ = input;
}

// 独立的实例清理逻辑：实例对应实体已不存在时调用 destroy 回调，并同步移除 handles_ 条目。
// 与 update() 内的清理代码相同，同样使用索引循环以便在遍历中删除元素。
void ScriptHost::removeDestroyedInstances() {
    if (!world_ || !destroy_) {
        return;
    }
    for (std::size_t index = 0; index < instances_.size();) {
        if (!world_->alive(instances_[index].entity)) {
            destroy_(instances_[index].handle);
            handles_.erase(encodeEntity(instances_[index].entity));
            instances_.erase(instances_.begin() + static_cast<std::ptrdiff_t>(index));
        } else {
            ++index;
        }
    }
}

void ScriptHost::initializeApi() {
    // 先整体清零，避免重新加载 DLL 时残留上一次的函数指针；再填头部元数据。
    api_ = {};
    api_.abiVersion = L3D_SCRIPT_ABI_VERSION;
    api_.structSize = static_cast<std::uint32_t>(sizeof(L3D_EngineApiV1));
    // 把 this 交给插件：之后每个回调的第一个参数 userData 都会原样传回，
    // 这就是“C 函数没有 this 指针”时把调用绑定到对象实例的常用手法。
    api_.userData = this;

    // 日志：message 由 C# 侧提供，长度显式给出，不保证以 '\0' 结尾，因此用 string_view 包住。
    api_.logMessage = [](void* userData, std::int32_t level, const char* message,
                         std::uint32_t length) {
        if (!message) {
            return;
        }
        const std::string_view text(message, length);
        // level 与 L3D_LogLevel 对应；超出范围的未知值按 INFO 处理，保证日志不会丢。
        if (level >= static_cast<std::int32_t>(L3D_LOG_ERROR)) {
            L3D_ERROR("[C#] {}", text);
        } else if (level == static_cast<std::int32_t>(L3D_LOG_WARNING)) {
            L3D_WARN("[C#] {}", text);
        } else {
            L3D_INFO("[C#] {}", text);
        }
        (void)userData;
    };

    // 以下访问器遵循同一模板：检查 host/world/输出指针 → tryGet 组件 → 写输出并返回 1，
    // 任何一个条件不满足就返回 0。这样插件侧只需判断返回值，绝不会收到野指针。
    api_.getPosition = [](void* userData, std::uint64_t entityValue,
                          L3D_Vec3* output) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !output) {
            return 0;
        }
        const auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue));
        if (!transform) {
            return 0;
        }
        // position 是 Transform 的本地坐标（相对父实体）；当前 ABI 不暴露世界变换，
        // 有层级的对象需要脚本自己叠加父链，或改用物理/射线查询。
        *output = {transform->position.x, transform->position.y, transform->position.z};
        return 1;
    };

    api_.setPosition = [](void* userData, std::uint64_t entityValue, L3D_Vec3 position) {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return;
        }
        // 写入的也是本地坐标；这里只改组件，世界矩阵会在下一次变换系统更新时刷新。
        if (auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue))) {
            transform->position = {position.x, position.y, position.z};
        }
    };

    api_.getRotationEuler = [](void* userData, std::uint64_t entityValue,
                               L3D_Vec3* output) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !output) {
            return 0;
        }
        const auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue));
        if (!transform) {
            return 0;
        }
        // Transform 内部存四元数；ABI 用角度制欧拉角，方便 C# 侧直接使用。
        // 与 position 一样，这是相对父实体的本地旋转，不是世界朝向。
        const auto euler = glm::degrees(glm::eulerAngles(transform->rotation));
        *output = {euler.x, euler.y, euler.z};
        return 1;
    };

    api_.setRotationEuler = [](void* userData, std::uint64_t entityValue, L3D_Vec3 euler) {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return;
        }
        // 约定的输入是角度制欧拉角，先转弧度再构造四元数，与 getRotationEuler 互为逆操作。
        if (auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue))) {
            transform->rotation = glm::quat(glm::radians(glm::vec3(euler.x, euler.y, euler.z)));
        }
    };

    // 角色的运行时状态（速度、是否着地）也通过 ABI 暴露，脚本据此决定跳跃或动画切换。
    api_.getVelocity = [](void* userData, std::uint64_t entityValue,
                          L3D_Vec3* output) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !output) {
            return 0;
        }
        const auto* controller =
            host->world_->tryGet<CharacterController>(decodeEntity(entityValue));
        if (!controller) {
            return 0;
        }
        *output = {controller->velocity.x, controller->velocity.y, controller->velocity.z};
        return 1;
    };

    // 写入速度后不会立刻移动实体；物理系统在下一帧积分时才真正推动角色。
    api_.setVelocity = [](void* userData, std::uint64_t entityValue, L3D_Vec3 velocity) {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return;
        }
        if (auto* controller = host->world_->tryGet<CharacterController>(
                decodeEntity(entityValue))) {
            controller->velocity = {velocity.x, velocity.y, velocity.z};
        }
    };

    // 没有 CharacterController 的实体返回 0（视为未着地），而不是报错。
    api_.isGrounded = [](void* userData, std::uint64_t entityValue) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return 0;
        }
        const auto* controller =
            host->world_->tryGet<CharacterController>(decodeEntity(entityValue));
        return controller && controller->grounded ? 1 : 0;
    };

    api_.isKeyDown = [](void* userData, std::int32_t key) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->input_ || key <= 0) {
            return 0;
        }
        // L3D_KeyCode 的数值与 l3d::Key 一一对应，可直接转型；key <= 0 已在上面排除。
        return host->input_->isKeyDown(static_cast<Key>(key)) ? 1 : 0;
    };

    api_.findEntityByTag = [](void* userData, const char* tag,
                              std::uint32_t length) -> std::uint64_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !tag) {
            return L3D_INVALID_ENTITY;
        }
        // tag 以“指针 + 长度”传递，不是 '\0' 结尾的 C 字符串，必须用 string_view 限定读取范围。
        const auto entity = findEntityByTag(*host->world_, std::string_view(tag, length));
        return encodeEntity(entity);
    };

    // 查询实体是否具备角色能力：脚本可先判断，再决定是否调用速度/着地相关接口。
    api_.hasCharacterController = [](void* userData,
                                     std::uint64_t entityValue) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return 0;
        }
        return host->world_->tryGet<CharacterController>(decodeEntity(entityValue)) ? 1 : 0;
    };

    api_.destroyEntity = [](void* userData, std::uint64_t entityValue) {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return;
        }
        // 与编辑器的删除行为一致：递归销毁整棵子树，避免子实体留下指向失效父节点的 Hierarchy。
        const auto entity = decodeEntity(entityValue);
        if (host->world_->alive(entity)) {
            destroyEntityRecursive(*host->world_, entity);
        }
    };

    api_.createEntity = [](void* userData, const char* tag,
                           std::uint32_t length) -> std::uint64_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return L3D_INVALID_ENTITY;
        }
        // 新实体总是带 Transform；有 tag 时同时写 Name 与 Tag，否则给一个默认显示名。
        const auto entity = host->world_->create();
        host->world_->add<Transform>(entity);
        if (tag && length > 0) {
            const std::string value(tag, length);
            host->world_->add<NameComponent>(entity, {value});
            host->world_->add<TagComponent>(entity, {value});
        } else {
            host->world_->add<NameComponent>(entity, {"Spawned Entity"});
        }
        // 返回 uint64 实体编码：ABI 两边都不存在 C++ 的 Entity 类型。
        return encodeEntity(entity);
    };

    // maxDistance 限制射线长度；ignoreEntity 常用于忽略发射者自己（例如从角色胸口发射）。
    api_.raycast = [](void* userData, L3D_Vec3 origin, L3D_Vec3 direction,
                      float maxDistance, std::uint64_t ignoreEntity,
                      std::uint64_t* outEntity, float* outDistance,
                      L3D_Vec3* outPoint) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !host->physics_ || !outEntity || !outDistance ||
            !outPoint) {
            return 0;
        }
        // physics_ 只在 start() 成功时绑定；没有物理世界就视为未命中，脚本可安全调用。
        const auto hit = host->physics_->raycast(
            *host->world_, {origin.x, origin.y, origin.z},
            {direction.x, direction.y, direction.z}, maxDistance,
            decodeEntity(ignoreEntity));
        if (!hit) {
            return 0;
        }
        // 命中结果通过输出参数返回：实体编码、距离、命中点，插件按顺序读取。
        *outEntity = encodeEntity(hit->entity);
        *outDistance = hit->distance;
        *outPoint = {hit->point.x, hit->point.y, hit->point.z};
        return 1;
    };

    api_.isMouseButtonDown = [](void* userData, std::int32_t button) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->input_ || button < 0 ||
            button >= static_cast<std::int32_t>(InputSnapshot::MouseCapacity)) {
            return 0;
        }
        // 先做容量检查：InputSnapshot 的鼠标数组长度固定，越界访问会破坏内存。
        return host->input_->isMouseDown(static_cast<MouseButton>(button)) ? 1 : 0;
    };

    api_.getMouseDelta = [](void* userData, L3D_Vec2* output) {
        auto* host = fromUserData(userData);
        if (!host || !host->input_ || !output) {
            return;
        }
        // 鼠标位移来自当帧 InputSnapshot，单位是窗口像素；灵敏度由脚本自行决定。
        *output = {host->input_->mouseDelta.x, host->input_->mouseDelta.y};
    };

    api_.getMousePosition = [](void* userData, L3D_Vec2* output) {
        auto* host = fromUserData(userData);
        if (!host || !host->input_ || !output) {
            return;
        }
        // 鼠标坐标使用窗口像素，原点在窗口左上角，与 ImGui 坐标一致。
        *output = {host->input_->mousePosition.x, host->input_->mousePosition.y};
    };

    // 滚轮增量是当帧累计值，没有输入时为 0；正负方向由平台约定，脚本按需取反。
    api_.getMouseScrollDelta = [](void* userData) -> float {
        auto* host = fromUserData(userData);
        return host && host->input_ ? host->input_->scrollDelta : 0.0f;
    };

    // 缩放同样是 Transform 字段，读写方式与位置/旋转完全一致。
    api_.getScale = [](void* userData, std::uint64_t entityValue,
                       L3D_Vec3* output) -> std::int32_t {
        auto* host = fromUserData(userData);
        if (!host || !host->world_ || !output) {
            return 0;
        }
        const auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue));
        if (!transform) {
            return 0;
        }
        // 这里返回的是本地缩放；父节点的缩放会通过世界矩阵继续作用于子节点。
        *output = {transform->scale.x, transform->scale.y, transform->scale.z};
        return 1;
    };

    api_.setScale = [](void* userData, std::uint64_t entityValue, L3D_Vec3 scale) {
        auto* host = fromUserData(userData);
        if (!host || !host->world_) {
            return;
        }
        // 直接写组件字段；实体若没有 Transform 则静默忽略，保持所有 setter 行为一致。
        if (auto* transform = host->world_->tryGet<Transform>(decodeEntity(entityValue))) {
            transform->scale = {scale.x, scale.y, scale.z};
        }
    };
    // 函数表到这里已经填满，作用相当于脚本侧的“虚函数表”：
    // 插件通过它调用引擎能力，而不需要（也无法）链接引擎的 C++ 符号。
}

} // namespace l3d
