// GameScripts：引擎的 C# 脚本插件。C++ 负责渲染、物理与实体管理，这里只写玩法逻辑。
// 双方通过固定的 C ABI 通信：引擎递来一张函数指针表（EngineApiV1）供我们调用引擎能力，
// 我们则导出若干 [UnmanagedCallersOnly] 函数（NativeExports）供引擎回调。
// 本程序集用 NativeAOT 编译成 GameScripts.dll：没有 JIT、不依赖完整 .NET 运行时，未用到的
// 代码会被裁剪，所以 JSON 必须用源生成器；重新编译后需要重启引擎才会加载新 DLL。
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace GameScripts;

// 按键枚举。跨 ABI 只传整数，所以这些数值必须与 C++ 侧完全一致，改名无所谓、改值会错位。
// 新增按键时两边要同步加值，否则同一个数字会代表不同的键。
internal enum KeyCode
{
    W = 1,
    A = 2,
    S = 3,
    D = 4,
    Q = 5,
    E = 6,
    Space = 7,
    LeftShift = 8,
    Escape = 9,
    Delete = 10,
    Enter = 11,
    Tab = 12,
    Num1 = 13,
    Num2 = 14,
    Num3 = 15,
    F = 16,
    R = 17,
}

// 日志级别，编号与引擎侧一一对应；传错级别只会让日志落到别的分类里。
// 日志通过函数表交给引擎输出，脚本不直接写控制台或文件。
internal enum LogLevel
{
    Info = 0,
    Warning = 1,
    Error = 2,
}

// 鼠标键编号：0 左、1 右、2 中，具体含义同样由 C++ 端解释。
internal enum MouseButton
{
    Left = 0,
    Right = 1,
    Middle = 2,
}

// 与 C++ Vec3 逐字段对齐的 POD 结构。LayoutKind.Sequential 保证字段按声明顺序排列，
// 双方看到的内存布局一致，才能安全地按值传递，或让引擎通过指针写入。
// 浮点统一用 4 字节 float；换成 double 会改变布局，导致双方读数错位。
[StructLayout(LayoutKind.Sequential)]
internal struct Vec3
{
    public float X;
    public float Y;
    public float Z;

    public Vec3(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }
}

// 二维向量，用途与约束同 Vec3：布局必须与 C++ Vec2 一致。
[StructLayout(LayoutKind.Sequential)]
internal struct Vec2
{
    public float X;
    public float Y;

    public Vec2(float x, float y)
    {
        X = x;
        Y = y;
    }
}

// 引擎能力表（版本 1）。可以把它想象成 C++ 递过来的一本“电话簿”：每项都是一个函数指针，
// 双方按约定好的顺序解读同一块内存，字段的数量、顺序和类型都不能随意改动。
// 表内只能出现 POD 参数（整数、浮点、结构体指针、void*），不能出现 C# 对象或 GC 引用；
// 所有函数都使用 Cdecl 调用约定，与 C++ 侧 extern "C" 的导出方式匹配。
// 每个函数都以 UserData 作为第一个参数，脚本只需原样转发，不要自己另存一份。
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct EngineApiV1
{
    // 双方约定的版本号；对不上说明引擎与脚本 DLL 不是配套构建。
    public uint AbiVersion;
    // 本结构的字节大小，用于发现“旧 DLL 配新引擎”这类布局差异。
    public uint StructSize;
    // 引擎自己的上下文指针；每次调用都原样带回，脚本不要解读其内容。
    public void* UserData;
    // 返回值约定：Get*/Is* 用 0 表示失败或“否”，非 0 表示成功或“是”。
    // 输出结果由调用方提供指针、引擎写回，绝不返回托管对象。
    public delegate* unmanaged[Cdecl]<void*, int, byte*, uint, void> LogMessage;
    // 实体状态读写：entity 是 64 位实体 id，数值按值或指针传递。
    // 只传 id 不传裸指针：对象生命周期由引擎掌握，C# 持有指针随时可能悬空。
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3*, int> GetPosition;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3, void> SetPosition;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3*, int> GetRotationEuler;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3, void> SetRotationEuler;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3*, int> GetVelocity;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3, void> SetVelocity;
    // 物理查询：返回非 0 表示角色当前踩在地面上。
    public delegate* unmanaged[Cdecl]<void*, ulong, int> IsGrounded;
    // 输入快照：只回答“这一帧按没按”，不提供按下/抬起事件。
    public delegate* unmanaged[Cdecl]<void*, int, int> IsKeyDown;
    // 实体管理：标签是引擎中实体的字符串标识，找不到时返回 InvalidEntity。
    public delegate* unmanaged[Cdecl]<void*, byte*, uint, ulong> FindEntityByTag;
    public delegate* unmanaged[Cdecl]<void*, ulong, int> HasCharacterController;
    // 销毁后该 id 失效，脚本不应再拿它读写。
    public delegate* unmanaged[Cdecl]<void*, ulong, void> DestroyEntity;
    // 返回新实体的 id，标签用于之后按名字查找；失败时同样返回 InvalidEntity。
    public delegate* unmanaged[Cdecl]<void*, byte*, uint, ulong> CreateEntity;
    // 射线检测：命中时返回 1，并通过后三个指针写回命中实体、距离和命中点。
    // direction 通常应先归一化，这样得到的 distance 才等于实际距离。
    public delegate* unmanaged[Cdecl]<void*, Vec3, Vec3, float, ulong, ulong*, float*,
        Vec3*, int> Raycast;
    // 以下四个是鼠标状态的“本帧快照”，不是会排队的输入事件。
    // 坐标和增量都由引擎填进 Vec2，单位与坐标原点由引擎约定。
    public delegate* unmanaged[Cdecl]<void*, int, int> IsMouseButtonDown;
    public delegate* unmanaged[Cdecl]<void*, Vec2*, void> GetMouseDelta;
    public delegate* unmanaged[Cdecl]<void*, Vec2*, void> GetMousePosition;
    public delegate* unmanaged[Cdecl]<void*, float> GetMouseScrollDelta;
    // 缩放属于 Transform，读写方式与位置、旋转相同。
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3*, int> GetScale;
    public delegate* unmanaged[Cdecl]<void*, ulong, Vec3, void> SetScale;
}

// 对函数表的薄封装：把“指针调用 + 错误码”包装成 C# 风格的返回值，并统一做就绪检查。
// 用函数表而不是逐函数 P/Invoke，好处是 DLL 不必静态导入每个引擎符号，双方只约定一张表。
// 未就绪时：读取类返回 null/false/0，写入类变成空操作，绝不抛异常。
// 这些方法都很薄，真正的校验在 C++ 侧；C# 侧只负责安全检查和类型转换。
internal static unsafe class EngineApi
{
    public const uint AbiVersion = 1;
    // 引擎用 ulong.MaxValue 表示“实体不存在”；C# 侧只把它当句柄比较，绝不当作指针。
    public const ulong InvalidEntity = ulong.MaxValue;

    // 保存表的副本；脚本运行期间引擎保证表中函数与 UserData 一直有效。
    private static EngineApiV1 s_api;
    private static bool s_ready;

    public static bool Ready => s_ready;

    // 由引擎在加载插件后调用一次：先校验版本、表大小和每个函数指针，
    // 避免运行期因空指针或版本错位而崩溃。全部通过后才置 Ready。
    public static bool Initialize(EngineApiV1* api)
    {
        if (api == null || api->AbiVersion != AbiVersion ||
            api->StructSize < (uint)sizeof(EngineApiV1) || api->UserData == null)
        {
            return false;
        }

        if (api->LogMessage == null || api->GetPosition == null || api->SetPosition == null ||
            api->GetRotationEuler == null || api->SetRotationEuler == null ||
            api->GetVelocity == null || api->SetVelocity == null || api->IsGrounded == null ||
            api->IsKeyDown == null || api->FindEntityByTag == null ||
            api->HasCharacterController == null || api->DestroyEntity == null ||
            api->CreateEntity == null || api->Raycast == null ||
            api->IsMouseButtonDown == null || api->GetMouseDelta == null ||
            api->GetMousePosition == null || api->GetMouseScrollDelta == null ||
            api->GetScale == null || api->SetScale == null)
        {
            return false;
        }

        // 复制整张表到自己的静态字段，之后调用都走副本，不保存引擎传入的指针。
        s_api = *api;
        s_ready = true;
        return true;
    }

    // 关闭后所有包装方法都变成空操作，避免卸载阶段继续回调可能已失效的引擎对象。
    public static void Shutdown()
    {
        s_ready = false;
        s_api = default;
    }

    // 日志统一走引擎，脚本不要直接写控制台，否则 NativeAOT 下可能根本看不到输出。
    public static void Log(LogLevel level, string message)
    {
        if (!s_ready)
        {
            return;
        }

        // 引擎只认识字节指针：先编码成 UTF-8，再用 fixed 钉住托管数组，防止 GC 搬走它。
        // 长度按字节数传，而不是字符数。
        var bytes = Encoding.UTF8.GetBytes(message);
        fixed (byte* pointer = bytes)
        {
            s_api.LogMessage(s_api.UserData, (int)level, pointer, (uint)bytes.Length);
        }
    }

    public static Vec3? GetPosition(ulong entity)
    {
        if (!s_ready)
        {
            return null;
        }

        // 结果由引擎写进栈上的 value；返回非 0 才代表实体有效，否则以 null 表示失败。
        Vec3 value;
        return s_api.GetPosition(s_api.UserData, entity, &value) != 0 ? value : null;
    }

    // 写入只是把值交给引擎，具体何时生效（例如物理步进之后）由引擎决定。
    public static void SetPosition(ulong entity, Vec3 value)
    {
        if (s_ready)
        {
            s_api.SetPosition(s_api.UserData, entity, value);
        }
    }

    // 旋转以欧拉角（度）表示，而不是四元数；顺序按引擎约定。
    public static Vec3? GetRotationEuler(ulong entity)
    {
        if (!s_ready)
        {
            return null;
        }

        Vec3 value;
        return s_api.GetRotationEuler(s_api.UserData, entity, &value) != 0 ? value : null;
    }

    // 写入时同样以度为单位，轴顺序与读取保持一致。
    public static void SetRotationEuler(ulong entity, Vec3 value)
    {
        if (s_ready)
        {
            s_api.SetRotationEuler(s_api.UserData, entity, value);
        }
    }

    // 速度单位是“每秒”，乘以 delta 可估算这一帧的位移。
    public static Vec3? GetVelocity(ulong entity)
    {
        if (!s_ready)
        {
            return null;
        }

        Vec3 value;
        return s_api.GetVelocity(s_api.UserData, entity, &value) != 0 ? value : null;
    }

    // 速度交给角色控制器/物理积分，脚本不应同时直接改位置，否则两者会打架。
    public static void SetVelocity(ulong entity, Vec3 value)
    {
        if (s_ready)
        {
            s_api.SetVelocity(s_api.UserData, entity, value);
        }
    }

    // 跳跃前查询，避免角色在空中起跳；判定由引擎的物理查询给出。
    public static bool IsGrounded(ulong entity)
    {
        return s_ready && s_api.IsGrounded(s_api.UserData, entity) != 0;
    }

    // 只读取快照，不消耗事件；同一帧内多次查询结果一致。
    public static bool IsKeyDown(KeyCode key)
    {
        return s_ready && s_api.IsKeyDown(s_api.UserData, (int)key) != 0;
    }

    // 标签重复时返回哪一个由引擎决定，场景里应保证标签唯一。
    public static ulong FindEntityByTag(string tag)
    {
        if (!s_ready)
        {
            return InvalidEntity;
        }

        // 字符串同样以 UTF-8 字节 + 长度传入；找不到时引擎返回 InvalidEntity。
        var bytes = Encoding.UTF8.GetBytes(tag);
        fixed (byte* pointer = bytes)
        {
            return s_api.FindEntityByTag(s_api.UserData, pointer, (uint)bytes.Length);
        }
    }

    // 带角色控制器的实体才能当玩家；移动、跳跃和开门都靠它区分真假“人”。
    public static bool HasCharacterController(ulong entity)
    {
        return s_ready && s_api.HasCharacterController(s_api.UserData, entity) != 0;
    }

    // 实体 id 由引擎发放，脚本只保存和回传，不能把它解引用成 C++ 指针。
    public static void DestroyEntity(ulong entity)
    {
        if (s_ready)
        {
            s_api.DestroyEntity(s_api.UserData, entity);
        }
    }

    public static ulong CreateEntity(string tag)
    {
        if (!s_ready)
        {
            return InvalidEntity;
        }

        var bytes = Encoding.UTF8.GetBytes(tag);
        fixed (byte* pointer = bytes)
        {
            return s_api.CreateEntity(s_api.UserData, pointer, (uint)bytes.Length);
        }
    }

    // 命中点可用于放置特效或判断弹着位置；未命中时 out 值保持默认。
    public static bool Raycast(Vec3 origin, Vec3 direction, float maxDistance,
        ulong ignoreEntity, out ulong hitEntity, out float distance, out Vec3 point)
    {
        // 先把 out 参数置为确定值，保证引擎返回失败时调用方看到的状态也一致。
        hitEntity = InvalidEntity;
        distance = 0.0f;
        point = default;
        if (!s_ready)
        {
            return false;
        }

        ulong entityValue;
        float distanceValue;
        Vec3 pointValue;
        // ignoreEntity 通常传自身，避免射线一出发就命中发射者。
        var hit = s_api.Raycast(s_api.UserData, origin, direction, maxDistance, ignoreEntity,
            &entityValue, &distanceValue, &pointValue) != 0;
        if (!hit)
        {
            return false;
        }

        hitEntity = entityValue;
        distance = distanceValue;
        point = pointValue;
        return true;
    }

    public static bool IsMouseButtonDown(MouseButton button)
    {
        return s_ready && s_api.IsMouseButtonDown(s_api.UserData, (int)button) != 0;
    }

    // 位移是相对上一帧的增量，适合做视角旋转；滚动增量一般用于缩放。
    public static Vec2? GetMouseDelta()
    {
        if (!s_ready)
        {
            return null;
        }

        Vec2 value;
        s_api.GetMouseDelta(s_api.UserData, &value);
        return value;
    }

    // 鼠标坐标与增量使用同一套单位，可直接用于 UI 定位或视角旋转。
    public static Vec2 GetMousePosition()
    {
        if (!s_ready)
        {
            return default;
        }

        Vec2 value;
        s_api.GetMousePosition(s_api.UserData, &value);
        return value;
    }

    // 滚轮增量是这一帧的累计值，没有滚动时为 0。
    public static float GetMouseScrollDelta()
    {
        return s_ready ? s_api.GetMouseScrollDelta(s_api.UserData) : 0.0f;
    }

    // 缩放也属于 Transform，和位置、旋转一样由引擎统一管理。
    public static Vec3? GetScale(ulong entity)
    {
        if (!s_ready)
        {
            return null;
        }

        Vec3 value;
        return s_api.GetScale(s_api.UserData, entity, &value) != 0 ? value : null;
    }

    public static void SetScale(ulong entity, Vec3 value)
    {
        if (s_ready)
        {
            s_api.SetScale(s_api.UserData, entity, value);
        }
    }
}

// 目录项：固定 64 字节的 UTF-8 名称缓冲区。定长让 C++ 可以一次性分配数组，
// 无需管理字符串生命周期；名字以 0 结尾，实际最多存放 63 字节加终止符。
// 引擎会先把名字复制到自己的缓冲区再返回，所以这里不需要脚本管理内存。
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ScriptInfo
{
    public fixed byte Name[64];
}

// NativeAOT 下反射被裁剪，JsonSerializer 无法在运行时扫描类型；源生成器会在编译期
// 为下面每个类型生成序列化代码，camelCase 命名和忽略大小写也不必依赖反射。
// 每个可配置属性类型都要在这里登记，漏登记会在 NativeAOT 运行时报错。
[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
    PropertyNameCaseInsensitive = true)]
[JsonSerializable(typeof(PlayerControllerProperties))]
[JsonSerializable(typeof(ThirdPersonCameraProperties))]
[JsonSerializable(typeof(DoorTriggerProperties))]
[JsonSerializable(typeof(PickupProperties))]
[JsonSerializable(typeof(AbiProbeProperties))]
internal partial class ScriptJsonContext : JsonSerializerContext
{
}

// 属性类只影响玩法参数；引擎负责序列化并在创建脚本时传入，脚本不负责保存配置。
// 属性名按 camelCase 匹配，字段初始值即缺省配置。
internal sealed class PlayerControllerProperties
{
    public float MoveSpeed { get; set; } = 6.0f;
    public float JumpSpeed { get; set; } = 7.5f;
}

// 相机相对目标的偏移（依次为 X 左右、Y 高度、Z 后方）；LookHeight 是注视点高度，
// Smoothing 是跟随速度，越大越“贴身”。
internal sealed class ThirdPersonCameraProperties
{
    public string TargetTag { get; set; } = "Player";
    public float[] Offset { get; set; } = [0.0f, 3.6f, 7.5f];
    public float LookHeight { get; set; } = 1.2f;
    public float Smoothing { get; set; } = 8.0f;
}

// 门的目标标签、开启时上升的高度和升降速度。
internal sealed class DoorTriggerProperties
{
    public string DoorTag { get; set; } = "Door";
    public float OpenHeight { get; set; } = 3.5f;
    public float Speed { get; set; } = 2.4f;
}

// 拾取物名称，仅用于日志显示。
internal sealed class PickupProperties
{
    public string Label { get; set; } = "Pickup";
}

// ABI 自检开关：分别控制创建实体、射线检测、鼠标状态三条验证路径。
internal sealed class AbiProbeProperties
{
    public bool CreateEntity { get; set; } = true;
    public string SpawnTag { get; set; } = "SpawnedByAbi";
    public bool Raycast { get; set; } = true;
    public float RayDistance { get; set; } = 10.0f;
    public bool ReportMouse { get; set; } = true;
}

// 每个脚本实例都实现这三个回调。引擎不持有 C# 对象引用，只凭句柄找到实例再转发事件。
// Update 每帧调用一次，delta 单位为秒；Trigger/Collision 在进入和离开时各调用一次。
// 与引擎生命周期的对应关系：
// Create -> 执行构造函数（类似 onCreate），此时属性已解析，实体 id 已传入；
// Update -> 每帧一次（onUpdate）；Destroy -> 从实例表移除（onDestroy），之后不再有回调。
internal interface IScript
{
    void Update(float delta);
    // other 是事件另一方的实体 id；entered 为 true 表示进入，false 表示离开。
    void OnTrigger(ulong other, bool entered);
    // 触发事件来自触发器体积重叠，碰撞事件来自实体实体碰撞，两者参数含义一致。
    void OnCollision(ulong other, bool entered);
}

// 玩家移动脚本：把 WASD 折算成水平速度，落地时响应空格跳跃，并让角色面朝移动方向。
// 它不直接改位置，而是把速度交给引擎的角色控制器/物理系统积分，碰撞因此仍由引擎负责。
internal sealed class PlayerController : IScript
{
    // 只保存实体 id 和配置，不缓存任何 C++ 对象；实体销毁后 id 自然失效。
    private readonly ulong _entity;
    private readonly PlayerControllerProperties _properties;

    public PlayerController(ulong entity, PlayerControllerProperties properties)
    {
        _entity = entity;
        _properties = properties;
    }

    public void Update(float delta)
    {
        // 以速度驱动，不依赖本帧时长，因此 delta 在这里用不到。
        _ = delta;
        // 取上一帧速度作为基础，保留引擎施加的重力等 Y 分量，只覆盖水平方向。
        var velocity = EngineApi.GetVelocity(_entity) ?? new Vec3();

        // 把按键折算成 -1/0/1 的轴向输入；W 让 Z 为负、S 为正，须与引擎坐标约定一致。
        var horizontal = 0.0f;
        var vertical = 0.0f;
        if (EngineApi.IsKeyDown(KeyCode.A)) horizontal -= 1.0f;
        if (EngineApi.IsKeyDown(KeyCode.D)) horizontal += 1.0f;
        if (EngineApi.IsKeyDown(KeyCode.W)) vertical -= 1.0f;
        if (EngineApi.IsKeyDown(KeyCode.S)) vertical += 1.0f;

        // 斜向同时按两个方向时长度为根号 2，归一化可避免斜着走比直着走更快。
        // 归一化只改长度、不改方向符号，所以“左右前后”仍然正确。
        var lengthSquared = horizontal * horizontal + vertical * vertical;
        if (lengthSquared > 1.0f)
        {
            var inverseLength = 1.0f / MathF.Sqrt(lengthSquared);
            horizontal *= inverseLength;
            vertical *= inverseLength;
        }

        // MoveSpeed 的单位是每秒，引擎会按 delta 把它积分成实际位移。
        velocity.X = horizontal * _properties.MoveSpeed;
        velocity.Z = vertical * _properties.MoveSpeed;
        // 仅在踩地时给向上速度，空中再按空格不会二段跳；水平速度则保持不变。
        if (EngineApi.IsKeyDown(KeyCode.Space) && EngineApi.IsGrounded(_entity))
        {
            // JumpSpeed 是起跳瞬间的向上速度，之后由引擎的重力逐帧拉回地面。
            velocity.Y = _properties.JumpSpeed;
        }

        // 移动是持续的：只要按键保持按下，每帧都会重新写入同样的水平速度。
        EngineApi.SetVelocity(_entity, velocity);
        // 用 atan2 把输入方向转成偏航角（度），让角色面朝移动方向；静止时不更新朝向。
        if (lengthSquared > 0.0001f)
        {
            var yaw = MathF.Atan2(horizontal, -vertical) * (180.0f / MathF.PI);
            EngineApi.SetRotationEuler(_entity, new Vec3(0.0f, yaw, 0.0f));
        }
    }

    // 玩家对触发和碰撞事件没有兴趣，空实现只为满足 IScript 约定。
    public void OnTrigger(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 第三人称相机：按标签找到目标，平滑移动到“目标位置 + 偏移”，并始终注视目标。
// 这里是纯跟随，不做相机与墙体的遮挡检测。
internal sealed class ThirdPersonCamera : IScript
{
    private readonly ulong _entity;
    private readonly ThirdPersonCameraProperties _properties;
    // 目标延迟解析：场景加载顺序不保证玩家先于相机创建，先记无效值、之后每帧重试。
    private ulong _target = EngineApi.InvalidEntity;

    public ThirdPersonCamera(ulong entity, ThirdPersonCameraProperties properties)
    {
        _entity = entity;
        _properties = properties;
    }

    public void Update(float delta)
    {
        if (_target == EngineApi.InvalidEntity)
        {
            // 只在目标丢失时按标签查找，找到后缓存，避免每帧做字符串查询。
            _target = EngineApi.FindEntityByTag(_properties.TargetTag);
        }
        if (_target == EngineApi.InvalidEntity)
        {
            // 玩家还没生成或已经被销毁，这一帧先不跟随。
            return;
        }

        var targetPosition = EngineApi.GetPosition(_target);
        if (targetPosition == null)
        {
            // 目标已销毁：清空缓存，下一帧重新按标签查找。
            _target = EngineApi.InvalidEntity;
            return;
        }

        // Offset 是相对目标的位置差，不是世界坐标；长度不足时用默认值补齐。
        var offset = _properties.Offset;
        var desired = new Vec3(
            targetPosition.Value.X + (offset.Length > 0 ? offset[0] : 0.0f),
            targetPosition.Value.Y + (offset.Length > 1 ? offset[1] : 3.5f),
            targetPosition.Value.Z + (offset.Length > 2 ? offset[2] : 7.5f));

        // 首次运行若拿不到相机自身位置，就从目标位置起步，避免从原点突然飞过去。
        var current = EngineApi.GetPosition(_entity) ?? desired;
        // blend = 1 - e^(-k*dt)：与帧率无关的指数平滑，Smoothing 越大跟随越紧，
        // 且 dt 再大也不会过冲。
        var blend = 1.0f - MathF.Exp(-MathF.Max(_properties.Smoothing, 0.0f) * delta);
        var smoothed = new Vec3(
            current.X + (desired.X - current.X) * blend,
            current.Y + (desired.Y - current.Y) * blend,
            current.Z + (desired.Z - current.Z) * blend);
        EngineApi.SetPosition(_entity, smoothed);

        // 把“相机指向注视点”的方向拆成 yaw 与 pitch；水平距离为 0 时 atan2(0,0) 返回 0，
        // 不会产生 NaN。结果转成角度后写入欧拉角。
        // LookHeight 抬高注视点，让镜头看向角色上半身而不是脚底。
        var lookAt = new Vec3(targetPosition.Value.X,
            targetPosition.Value.Y + _properties.LookHeight, targetPosition.Value.Z);
        var dx = lookAt.X - smoothed.X;
        var dy = lookAt.Y - smoothed.Y;
        var dz = lookAt.Z - smoothed.Z;
        var horizontalLength = MathF.Sqrt(dx * dx + dz * dz);
        var yaw = MathF.Atan2(-dx, -dz) * (180.0f / MathF.PI);
        var pitch = MathF.Atan2(dy, horizontalLength) * (180.0f / MathF.PI);
        // 相机朝向用 pitch/yaw 表达，roll 固定为 0，避免画面绕视线倾斜。
        EngineApi.SetRotationEuler(_entity, new Vec3(pitch, yaw, 0.0f));
    }

    public void OnTrigger(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 门脚本：挂在触发器上；带角色控制器的实体进入时把门抬高，离开时放回原位。
// 触发器通常是覆盖门口的体积，门本体按标签查找，脚本只改门的 Y 坐标。
internal sealed class DoorTrigger : IScript
{
    private readonly DoorTriggerProperties _properties;
    // 门实体延迟按标签查找；_closedHeight 记住初始高度，作为“关”状态的端点。
    private ulong _door = EngineApi.InvalidEntity;
    private float _closedHeight;
    private bool _open;

    public DoorTrigger(ulong entity, DoorTriggerProperties properties)
    {
        // 触发器实体自身不参与计算，只使用它携带的配置。
        _ = entity;
        _properties = properties;
    }

    public void Update(float delta)
    {
        if (_door == EngineApi.InvalidEntity)
        {
            _door = EngineApi.FindEntityByTag(_properties.DoorTag);
            if (_door == EngineApi.InvalidEntity)
            {
                // 门还没加载出来，这一帧先不动作。
                return;
            }
            // 第一次找到门时记下关闭高度；之后门在动也不重新取，基准保持不变。
            _closedHeight = EngineApi.GetPosition(_door)?.Y ?? 0.0f;
        }

        var position = EngineApi.GetPosition(_door);
        if (position == null)
        {
            // 门被销毁：清空缓存，等下次重新查找。
            _door = EngineApi.InvalidEntity;
            return;
        }

        // 目标高度只有“关”和“开”两档；每帧最多移动 speed*delta，
        // 接近目标时直接吸附，避免在终点附近来回抖动。
        // OpenHeight 是相对关闭高度的偏移，不是世界坐标里的绝对高度。
        var targetHeight = _closedHeight + (_open ? _properties.OpenHeight : 0.0f);
        var y = position.Value.Y;
        // Speed 是每秒上升高度，乘 delta 后才是这一帧的步长。
        var step = MathF.Max(_properties.Speed, 0.0f) * delta;
        if (MathF.Abs(targetHeight - y) <= step)
        {
            y = targetHeight;
        }
        else
        {
            // MathF.Sign 决定往开还是往关方向移动，两者共用同一段插值代码。
            y += MathF.Sign(targetHeight - y) * step;
        }

        // 只改 Y，保留原来的 X/Z，所以门是垂直升降而不是平移。
        EngineApi.SetPosition(_door, new Vec3(position.Value.X, y, position.Value.Z));
    }

    public void OnTrigger(ulong other, bool entered)
    {
        // 只有带角色控制器的实体算玩家；_open 表示门当前应当开着。
        // 离开时引擎会再调用一次并传 entered=false，所以门能自动关上。
        if (EngineApi.HasCharacterController(other))
        {
            _open = entered;
        }
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 拾取物：被角色碰到时记一条日志并销毁自身；_collected 防止重复触发。
// 通常挂在带触发器的拾取物实体上，被玩家碰到后消失。
internal sealed class Pickup : IScript
{
    private readonly ulong _entity;
    private readonly PickupProperties _properties;
    // 同一拾取物只结算一次，防止连续触发重复删除。
    private bool _collected;

    public Pickup(ulong entity, PickupProperties properties)
    {
        _entity = entity;
        _properties = properties;
    }

    // 拾取物不需要逐帧逻辑，保留空 Update 只是满足接口。
    public void Update(float delta)
    {
        _ = delta;
    }

    public void OnTrigger(ulong other, bool entered)
    {
        // 只处理“进入”事件；已收集或碰到非角色实体都直接忽略。
        if (!entered || _collected || !EngineApi.HasCharacterController(other))
        {
            return;
        }

        // 先置位再销毁，即使销毁流程再次触发事件，也不会重复日志和删除。
        _collected = true;
        // 日志交给引擎输出，可在引擎控制台按级别过滤。
        EngineApi.Log(LogLevel.Info, $"Collected {_properties.Label}");
        // 销毁由引擎执行；托管实例会等到 Destroy 或 Shutdown 时才从字典移除。
        EngineApi.DestroyEntity(_entity);
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 异常隔离的测试脚本：第一次 Update 抛异常后，导出层会禁用该实例并记录错误，
// 用来验证托管异常不会穿过 C ABI 传播到 C++。
internal sealed class ThrowingScript : IScript
{
    public void Update(float delta)
    {
        _ = delta;
        // 故意每次 Update 都抛；导出层捕获后会移除实例，因此只会看到一次错误日志。
        throw new InvalidOperationException("Intentional managed exception for ABI containment test");
    }

    public void OnTrigger(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 碰撞探针：把 enter/exit 事件写进日志，方便确认引擎的碰撞回调确实送达了脚本。
internal sealed class CollisionProbe : IScript
{
    public void Update(float delta)
    {
        _ = delta;
    }

    public void OnTrigger(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }

    public void OnCollision(ulong other, bool entered)
    {
        // 日志区分 enter/exit 并带上对方 id，便于核对事件顺序。
        EngineApi.Log(LogLevel.Info,
            $"CollisionProbe: {(entered ? "enter" : "exit")} other={other}");
    }
}

// ABI 探针：启动后只执行一次，依次验证建实体、缩放、射线、鼠标这几条接口通路。
// 各步骤由 JSON 属性开关控制，便于在场景里单独排查某个能力。
internal sealed class AbiProbe : IScript
{
    private readonly ulong _entity;
    private readonly AbiProbeProperties _properties;
    // 自检只适合做一次；置位后后续帧直接空转。
    private bool _ran;

    public AbiProbe(ulong entity, AbiProbeProperties properties)
    {
        _entity = entity;
        _properties = properties;
    }

    public void Update(float delta)
    {
        _ = delta;
        // 只在第一帧执行；否则每帧都会刷屏并不断创建实体。
        if (_ran)
        {
            return;
        }
        _ran = true;

        // 拿不到自身位置就从原点开始，后续步骤仍会执行，便于观察失败原因。
        var own = EngineApi.GetPosition(_entity) ?? new Vec3();
        if (_properties.CreateEntity)
        {
            var spawned = EngineApi.CreateEntity(_properties.SpawnTag);
            if (spawned != EngineApi.InvalidEntity)
            {
                // 新实体只设位置和缩放，脚本不接管其生命周期，便于观察 ABI 是否生效。
                EngineApi.SetPosition(spawned,
                    new Vec3(own.X + 2.0f, own.Y + 0.5f, own.Z));
                // 取不到缩放时按 1,1,1 兜底，避免拿 default 的 0 去参与计算。
                var scale = EngineApi.GetScale(spawned) ?? new Vec3(1.0f, 1.0f, 1.0f);
                EngineApi.SetScale(spawned,
                    new Vec3(scale.X + 0.5f, scale.Y + 0.5f, scale.Z + 0.5f));
                EngineApi.Log(LogLevel.Info, $"AbiProbe created entity={spawned}");
            }
            else
            {
                EngineApi.Log(LogLevel.Error, "AbiProbe entity creation failed");
            }
        }

        if (_properties.Raycast)
        {
            // 从自身头顶向下打一条射线，忽略自己，检查地面命中情况和距离。
            var origin = new Vec3(own.X, own.Y + 1.0f, own.Z);
            // 射线结果只用于验证 ABI，不参与玩法。
            if (EngineApi.Raycast(origin, new Vec3(0.0f, -1.0f, 0.0f),
                    _properties.RayDistance, _entity, out var hitEntity,
                    out var distance, out _))
            {
                // 只关心是否命中与距离，命中点用弃元忽略。
                EngineApi.Log(LogLevel.Info,
                    $"AbiProbe ray hit entity={hitEntity} distance={distance:F2}");
            }
            else
            {
                EngineApi.Log(LogLevel.Warning, "AbiProbe ray miss");
            }
        }

        // 鼠标状态是“本帧快照”：delta 是相对上一帧的位移，position 是窗口坐标。
        if (_properties.ReportMouse)
        {
            var mouseDelta = EngineApi.GetMouseDelta() ?? new Vec2();
            var position = EngineApi.GetMousePosition();
            var left = EngineApi.IsMouseButtonDown(MouseButton.Left) ? 1 : 0;
            // 三类鼠标数据拼成一条日志，避免每帧多行刷屏。
            EngineApi.Log(LogLevel.Info,
                $"AbiProbe mouse left={left} delta={mouseDelta.X:F2},{mouseDelta.Y:F2} " +
                $"position={position.X:F2},{position.Y:F2} " +
                $"scroll={EngineApi.GetMouseScrollDelta():F2}");
        }
    }

    public void OnTrigger(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }

    public void OnCollision(ulong other, bool entered)
    {
        _ = other;
        _ = entered;
    }
}

// 脚本工厂：维护脚本类型目录，并按 JSON 属性创建实例。目录里的名字是与 C++ 的约定，
// 必须和引擎侧保存的脚本类型字符串完全一致，否则引擎传来的名字会匹配失败。
internal static class ScriptFactory
{
    // 目录顺序即导出给引擎的顺序；这些名字会出现在 ScriptInfo 里供编辑器选择。
    private static readonly string[] s_catalog =
    [
        "PlayerController",
        "ThirdPersonCamera",
        "DoorTrigger",
        "Pickup",
        "ThrowingScript",
        "CollisionProbe",
        "AbiProbe",
    ];

    // 新增脚本要同时改目录和 Create 的 switch，否则引擎能看到名字却创建失败。
    public static int Count => s_catalog.Length;
    public static string NameAt(int index) => s_catalog[index];

    // 未知类型返回 null，由导出层记录警告；属性 JSON 解析失败时各 Parse* 会退回默认配置。
    public static IScript? Create(string name, ulong entity, string json)
    {
        // 名字到类型的唯一映射；区分大小写，必须与目录中的字符串完全一致。
        return name switch
        {
            "PlayerController" => new PlayerController(entity, ParsePlayer(json)),
            "ThirdPersonCamera" => new ThirdPersonCamera(entity, ParseCamera(json)),
            "DoorTrigger" => new DoorTrigger(entity, ParseDoor(json)),
            "Pickup" => new Pickup(entity, ParsePickup(json)),
            "ThrowingScript" => new ThrowingScript(),
            "CollisionProbe" => new CollisionProbe(),
            "AbiProbe" => new AbiProbe(entity, ParseAbiProbe(json)),
            // 未知名字返回 null，由导出层统一记录警告。
            _ => null,
        };
    }

    // 所有 Parse* 都遵循“失败即用默认值”的策略：一份坏的属性配置不应让整个脚本加载失败。
    private static PlayerControllerProperties ParsePlayer(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json, ScriptJsonContext.Default.PlayerControllerProperties)
                   ?? new PlayerControllerProperties();
        }
        catch (JsonException exception)
        {
            // 配置坏了就退回默认值，游戏仍能启动，只会少掉自定义参数。
            EngineApi.Log(LogLevel.Warning, $"PlayerController properties: {exception.Message}");
            return new PlayerControllerProperties();
        }
    }

    private static ThirdPersonCameraProperties ParseCamera(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json,
                       ScriptJsonContext.Default.ThirdPersonCameraProperties)
                   ?? new ThirdPersonCameraProperties();
        }
        catch (JsonException exception)
        {
            EngineApi.Log(LogLevel.Warning, $"ThirdPersonCamera properties: {exception.Message}");
            return new ThirdPersonCameraProperties();
        }
    }

    private static DoorTriggerProperties ParseDoor(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json, ScriptJsonContext.Default.DoorTriggerProperties)
                   ?? new DoorTriggerProperties();
        }
        catch (JsonException exception)
        {
            EngineApi.Log(LogLevel.Warning, $"DoorTrigger properties: {exception.Message}");
            return new DoorTriggerProperties();
        }
    }

    private static PickupProperties ParsePickup(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json, ScriptJsonContext.Default.PickupProperties)
                   ?? new PickupProperties();
        }
        catch (JsonException exception)
        {
            EngineApi.Log(LogLevel.Warning, $"Pickup properties: {exception.Message}");
            return new PickupProperties();
        }
    }

    private static AbiProbeProperties ParseAbiProbe(string json)
    {
        try
        {
            return JsonSerializer.Deserialize(json, ScriptJsonContext.Default.AbiProbeProperties)
                   ?? new AbiProbeProperties();
        }
        catch (JsonException exception)
        {
            EngineApi.Log(LogLevel.Warning, $"AbiProbe properties: {exception.Message}");
            return new AbiProbeProperties();
        }
    }
}

// 导出给 C++ 的唯一入口集合。每个方法都标了 [UnmanagedCallersOnly]：NativeAOT 会为它生成
// 真正的原生函数入口，名字由 EntryPoint 指定，C++ 用 GetProcAddress/dlsym 按名字找到并调用。
// 这类方法只能使用 blittable 参数（整数、指针、POD 结构体），不能收/发托管对象。
// 托管异常绝不能越过这条边界（那会直接终止进程），所以每个导出函数都整段包在 try/catch 里。
// C++ 只通过这些 EntryPoint 认识脚本 DLL；函数名或签名一变，引擎侧必须同步修改。
internal static unsafe class NativeExports
{
    private const string IntentionalError = "Intentional managed exception from exported ABI";
    // handle 是脚本实例的编号，不是实体 id：C++ 只保存整数，C# 靠它查字典找到托管对象。
    // 用字典而不是返回对象指针，是因为 GC 会移动托管对象，裸指针随时可能失效。
    // 这里假设引擎只在单一线程调用导出函数，所以没有给字典加锁。
    private static readonly Dictionary<long, IScript> s_instances = [];
    private static long s_nextHandle = 1;

    // 导出函数统一用返回值表达结果：0 表示成功，非 0 表示失败；
    // 具体错误码由脚本侧定义，引擎通常只判断是否为 0。
    // 引擎先询问版本号，确认配套后才调用 Initialize；此时还不能使用任何引擎能力。
    // 这里也不检查 EngineApi.Ready，因为初始化之前同样必须能回答版本问题。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptAbiVersion",
        CallConvs = [typeof(CallConvCdecl)])]
    public static uint AbiVersion()
    {
        try
        {
            return EngineApi.AbiVersion;
        }
        catch
        {
            return 0;
        }
    }

    // 初始化失败返回非 0，引擎会卸载插件；即使这里出异常，也只会被吞掉并转成错误码。
    // 参数指针只在本次调用期间有效，EngineApi.Initialize 会把表内容复制走。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptInitialize",
        CallConvs = [typeof(CallConvCdecl)])]
    public static int Initialize(EngineApiV1* api)
    {
        try
        {
            if (!EngineApi.Initialize(api))
            {
                return 1;
            }

            EngineApi.Log(LogLevel.Info, "GameScripts initialized with EngineApiV1");
            return 0;
        }
        catch
        {
            return 2;
        }
    }

    // 目录查询采用“两次调用”协议：entries 为 null 时只返回脚本总数，
    // 引擎据此分配数组后再调一次，capacity 是它给出的数组长度。
    // 两次调用之间目录不会变化，所以第二次可以按索引直接写入。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptCatalog",
        CallConvs = [typeof(CallConvCdecl)])]
    public static uint Catalog(ScriptInfo* entries, uint capacity)
    {
        try
        {
            var count = (uint)ScriptFactory.Count;
            if (entries == null)
            {
                // entries 为 null：只报告数量，不写任何内存。
                // 返回值始终是总数而不是 capacity，方便引擎按需分配。
                return count;
            }

            // capacity 可能大于脚本数，只写前 count 项，剩余空间保持调用方的原值。
            var written = Math.Min(count, capacity);
            for (var index = 0u; index < written; ++index)
            {
                // 手动清零并逐字节写入，保证 64 字节内以 0 结尾，也不依赖字符串封送。
                var name = entries[index].Name;
                for (var byteIndex = 0; byteIndex < 64; ++byteIndex)
                {
                    name[byteIndex] = 0;
                }

                var encoded = Encoding.UTF8.GetBytes(ScriptFactory.NameAt((int)index));
                var length = Math.Min(encoded.Length, 63);
                for (var byteIndex = 0; byteIndex < length; ++byteIndex)
                {
                    name[byteIndex] = encoded[byteIndex];
                }
            }

            return count;
        }
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptCatalog failed: {exception.Message}");
            return 0;
        }
    }

    // name 和 properties 都是“UTF-8 字节指针 + 长度”，不是 C# string；
    // entity 是引擎实体 id，脚本之后靠它读写该实体的 Transform。
    // 名字不要求以 0 结尾，一律按长度解码，避免越界读取。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptCreate",
        CallConvs = [typeof(CallConvCdecl)])]
    public static long Create(byte* name, uint nameLength, ulong entity, byte* properties,
        uint propertiesLength)
    {
        try
        {
            if (!EngineApi.Ready || name == null || nameLength == 0)
            {
                return 0;
            }

            var scriptName = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(name, (int)nameLength));
            // 属性为空时当作空 JSON，让各 Parse* 使用默认配置。
            var json = properties == null || propertiesLength == 0
                ? "{}"
                : Encoding.UTF8.GetString(new ReadOnlySpan<byte>(properties, (int)propertiesLength));
            var script = ScriptFactory.Create(scriptName, entity, json);
            if (script == null)
            {
                // 名字拼错或 DLL 版本旧时走到这里；返回 0 表示创建失败，引擎会跳过该脚本。
                EngineApi.Log(LogLevel.Warning, $"Unknown script type '{scriptName}'");
                return 0;
            }

            // 句柄从 1 开始递增；0 留作失败标记，C++ 侧只需判断非 0。
            // 句柄用 long，对应 C++ 的 int64，不要截断成 int 再传回来。
            // 字典把整数句柄映射到托管实例，避免把对象指针直接交给 C++。
            var handle = s_nextHandle++;
            s_instances[handle] = script;
            return handle;
        }
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptCreate failed: {exception.Message}");
            return 0;
        }
    }

    // 每帧由引擎调用，delta 单位为秒。脚本抛异常时只禁用该实例，而不让异常穿出边界。
    // delta 由引擎提供，脚本不要自己用计时器测量帧间隔，否则和引擎不同步。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptUpdate",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Update(long handle, float delta)
    {
        try
        {
            // 句柄找不到通常表示实例已销毁或被禁用，静默跳过。
            if (!s_instances.TryGetValue(handle, out var script))
            {
                return;
            }

            // 两层 try 分工：内层只包脚本逻辑，外层兜住边界代码自身的异常。
            try
            {
                script.Update(delta);
            }
            catch (Exception exception)
            {
                // 移除实例等于禁用它，避免坏脚本每帧重复抛异常、刷爆日志。
                EngineApi.Log(LogLevel.Error,
                    $"Script update failed and instance was disabled: {exception.Message}");
                s_instances.Remove(handle);
            }
        }
        // 外层 catch 兜住边界代码自身的异常（例如字典操作），保证一定返回。
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptUpdate boundary failure: {exception.Message}");
        }
    }

    // 引擎在触发事件的“进入”和“离开”时各调用一次；布尔值以 int 传递，非 0 即 true。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptTrigger",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Trigger(long handle, ulong other, int entered)
    {
        try
        {
            // 事件可能晚于 Destroy 到达，句柄失效时直接忽略。
            if (!s_instances.TryGetValue(handle, out var script))
            {
                return;
            }

            try
            {
                script.OnTrigger(other, entered != 0);
            }
            catch (Exception exception)
            {
                EngineApi.Log(LogLevel.Error,
                    $"Script trigger failed and instance was disabled: {exception.Message}");
                s_instances.Remove(handle);
            }
        }
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptTrigger boundary failure: {exception.Message}");
        }
    }

    // 碰撞事件走独立入口，参数含义与 Trigger 相同。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptCollision",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Collision(long handle, ulong other, int entered)
    {
        try
        {
            if (!s_instances.TryGetValue(handle, out var script))
            {
                return;
            }

            try
            {
                script.OnCollision(other, entered != 0);
            }
            catch (Exception exception)
            {
                EngineApi.Log(LogLevel.Error,
                    $"Script collision failed and instance was disabled: {exception.Message}");
                s_instances.Remove(handle);
            }
        }
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptCollision boundary failure: {exception.Message}");
        }
    }

    // 引擎销毁实体或卸载脚本时调用；Remove 是幂等的，重复调用不会出错。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptDestroy",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Destroy(long handle)
    {
        try
        {
            // 只断开引擎到脚本的引用，托管对象何时释放由 GC 决定。
            s_instances.Remove(handle);
        }
        catch (Exception exception)
        {
            EngineApi.Log(LogLevel.Error, $"ScriptDestroy failed: {exception.Message}");
        }
    }

    // 引擎退出前的最后调用：先清空实例，再让包装层失效，之后任何调用都变成空操作。
    [UnmanagedCallersOnly(EntryPoint = "L3D_ScriptShutdown",
        CallConvs = [typeof(CallConvCdecl)])]
    public static void Shutdown()
    {
        try
        {
            // 清空后所有句柄失效，引擎不应再调用其他导出函数。
            s_instances.Clear();
            EngineApi.Shutdown();
        }
        catch
        {
            // Exceptions must never cross the C ABI.
        }
    }
}
