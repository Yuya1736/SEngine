# 接口与数据格式说明

## 1. 模块公共接口

| 接口/类型 | 头文件 | 作用 |
|---|---|---|
| `World` / `Entity` | `engine/scene/include/l3d/scene/world.h` | ECS 实体、组件和标签查询 |
| `IRenderer` | `engine/renderer/include/l3d/renderer/renderer.h` | 渲染后端边界 |
| `IPhysicsWorld` | `engine/physics/include/l3d/physics/physics_world.h` | 物理步进、射线和重叠查询 |
| `IAssetDatabase` | `engine/assets/include/l3d/assets/asset_database.h` | 资产扫描、GUID 和 CPU 侧加载 |
| `IScriptHost` | `engine/scripting/include/l3d/scripting/script_host.h` | C# 插件生命周期和脚本回调 |
| `EditorSession` | `engine/scene/include/l3d/scene/editor_session.h` | Play 前快照和 Stop 后恢复 |
| `RenderFrame` | `engine/renderer/include/l3d/renderer/render_types.h` | 场景到渲染器的只读帧描述 |
| `L3D_EngineApiV1` | `engine/scripting/include/l3d/scripting/script_abi.h` | C++ 暴露给 C# 的固定 C ABI 函数表 |

## 2. World / Entity

### 2.1 Entity 契约

```cpp
struct Entity {
    std::uint32_t index;
    std::uint32_t generation;
    bool valid() const noexcept;
};
```

| 契约 | 行为 |
|---|---|
| 稳定性 | `World::alive(entity)` 只有在槽位存活且 generation 匹配时为真 |
| 复用 | 销毁后 index 可复用，但 generation 递增 |
| 跨 ABI 表示 | `(uint64_t(generation) << 32)` 与 `index` 按位或 |
| 无效句柄 | `L3D_INVALID_ENTITY = UINT64_MAX` |

### 2.2 World API

```cpp
Entity create();
void destroy(Entity entity);
void clear();
bool alive(Entity entity) const;
const std::vector<Entity>& entities() const noexcept;

template <typename Component> Component& add(Entity, Component = {});
template <typename Component> bool remove(Entity);
template <typename Component> bool has(Entity) const;
template <typename Component> Component* tryGet(Entity);
template <typename Component> const Component* tryGet(Entity) const;
```

| 约束 | 说明 |
|---|---|
| 失效实体 | `tryGet` 对失效实体返回 `nullptr` |
| 组件添加 | 对同一实体重复添加同类型组件时覆盖原值 |
| 删除时通知 | `World::destroy` 若绑定了事件总线，同步发布 `EntityDestroyedEvent` |
| 复制 | 复制 World 会深拷贝所有组件池，但不复制事件总线指针 |
| 遍历 | `entities()` 返回存活实体列表，顺序不保证持久稳定 |

## 3. IRenderer

```cpp
class IRenderer {
public:
    virtual bool initialize(IAssetDatabase& assets) = 0;
    virtual void shutdown() = 0;
    virtual void render(const RenderFrame& frame, int width, int height) = 0;
    virtual void presentToDefaultFramebuffer(int width, int height) = 0;
    virtual std::uint32_t viewportTexture() const = 0;
    virtual std::optional<MeshBounds> meshBounds(const std::string& meshGuid) = 0;
};
```

| 契约 | 说明 |
|---|---|
| 输入 | `RenderFrame` 不包含 OpenGL 类型、STL 容器以外的 GPU 对象 |
| 输出 | `render` 写入离屏目标；`presentToDefaultFramebuffer` 负责默认帧缓冲呈现 |
| 资产 | 渲染器通过 `IAssetDatabase` 按 GUID 加载 CPU 数据并自行上传 GPU |
| 视口纹理 | 编辑器使用 `viewportTexture()` 把离屏结果显示到 ImGui |
| 选择信息 | `meshBounds` 用于视口拾取，不暴露 GL 资源 |

## 4. IPhysicsWorld

```cpp
class IPhysicsWorld {
public:
    virtual void step(World&, float delta, std::vector<CollisionEvent>& events) = 0;
    virtual std::optional<RayHit> raycast(const World&, const glm::vec3& origin,
                                          const glm::vec3& direction, float maxDistance,
                                          Entity ignore = {}) const = 0;
    virtual bool overlap(const World&, Entity first, Entity second) const = 0;
    virtual void reset() = 0;
};
```

| 类型 | 字段/语义 |
|---|---|
| `RayHit` | `entity`、`distance`、`point` |
| `CollisionEvent` | `first`、`second`、`phase`（Enter/Exit）、`trigger` |
| `resize/reset` | `reset` 清空上一帧碰撞/触发器边沿状态 |
| 射线忽略 | `ignore` 用于跳过角色自身等实体 |
| 确定性 | 纯主线程、无随机性、固定步进 |

## 5. IAssetDatabase

```cpp
class IAssetDatabase {
public:
    virtual Result<int> scan(const std::filesystem::path& assetRoot) = 0;
    virtual void refresh() = 0;
    virtual const std::filesystem::path& root() const noexcept = 0;
    virtual const std::vector<AssetRecord>& assets() const noexcept = 0;
    virtual const AssetRecord* findByGuid(std::string_view guid) const = 0;
    virtual const AssetRecord* findByPath(const std::filesystem::path&) const = 0;
    virtual std::vector<const AssetRecord*> byType(AssetType type) const = 0;
    virtual Result<std::shared_ptr<MeshData>> loadMesh(std::string_view guid) = 0;
    virtual Result<std::shared_ptr<TextureData>> loadTexture(std::string_view guid) = 0;
    virtual Result<MaterialAsset> loadMaterial(std::string_view guid) = 0;
};
```

| 类型 | 规则 |
|---|---|
| `AssetType` | Unknown、Mesh、Texture、Material、Scene、Shader |
| GUID | 文本 UUID，存储于附属 `.meta.json` |
| 缓存 | Mesh/Texture/Material 以 GUID 为键缓存 |
| 扫描失败 | 单个无元数据/非法资产记录警告并跳过；根目录不存在返回错误 |
| OpenGL | 资产模块不创建任何 GPU 资源 |

## 6. IScriptHost

```cpp
class IScriptHost {
public:
    virtual bool load(const std::filesystem::path& dllPath) = 0;
    virtual bool start(World& world, const IPhysicsWorld& physics,
                       const InputSnapshot& input) = 0;
    virtual void update(World&, const InputSnapshot&, float delta) = 0;
    virtual void dispatchEvents(World&, const std::vector<CollisionEvent>&,
                                const InputSnapshot&) = 0;
    virtual void stop() = 0;
    virtual bool loaded() const noexcept = 0;
    virtual bool running() const noexcept = 0;
    virtual std::size_t instanceCount() const noexcept = 0;
    virtual const std::vector<std::string>& catalog() const noexcept = 0;
};
```

| 生命周期 | 行为 |
|---|---|
| `load` | 只加载 DLL、校验导出和 ABI；可独立于场景开始调用 |
| `start` | 绑定 World、物理和输入，按 `ScriptComponent` 创建实例 |
| `update` | 按实例列表调用 C# `L3D_ScriptUpdate`，自动清理已销毁实体实例 |
| `dispatchEvents` | 按实体句柄查找脚本实例，分发碰撞/触发器事件 |
| `stop` | 销毁所有 C# 实例并解除 World/Physics/Input 绑定 |
| 错误 | DLL 缺失或版本不匹配返回 `false`，引擎继续运行 |

## 7. EditorSession

```cpp
class EditorSession {
public:
    bool beginPlay(World& scene);
    std::unique_ptr<World> endPlay();
    bool playing() const noexcept;
};
```

| 操作 | 语义 |
|---|---|
| `beginPlay` | 深拷贝当前 World；已在播放时返回 `false` |
| `endPlay` | 返回备份 World 并清空会话；无播放时返回空指针 |
| 所有权 | 返回的 `unique_ptr<World>` 由 `Application` 接管 |
| 事件总线 | 恢复后由应用重新绑定，不把旧指针复制进备份 |

## 8. RenderFrame

```cpp
struct RenderItem {
    std::string meshGuid;
    std::string materialGuid;
    glm::mat4 transform;
};

struct RenderFrame {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 cameraPosition;
    glm::vec3 directionalDirection;
    glm::vec3 directionalColor;
    float directionalIntensity;
    std::array<RenderPointerLight, 8> pointLights;
    int pointLightCount;
    std::vector<RenderItem> items;
};
```

| 约束 | 说明 |
|---|---|
| 方向光 | 取第一个 `DirectionalLight`，方向为世界空间方向 |
| 点光 | 最多收集 8 个，超出部分忽略 |
| 主相机 | 构建帧时选择第一个 `primary == true` 的相机 |
| 矩阵 | 使用 GLM 列主序矩阵；世界为 Y-up、右手坐标系 |
| 剔除 | 当前关闭面部剔除，允许双面观察内置几何体 |

## 9. C ABI：宿主导出给 C#

### 9.1 ABI 结构

```cpp
struct L3D_EngineApiV1 {
    uint32_t abiVersion;
    uint32_t structSize;
    void* userData;
    // 20 个函数指针，见下表
};
```

| 规则 | 实现 |
|---|---|
| 调用约定 | `extern "C"`，C# 侧 `CallConvCdecl` |
| 字符串 | UTF-8 `const char*` + `uint32_t` 长度；不要求 NUL 终止 |
| 向量 | `L3D_Vec2` / `L3D_Vec3` 为 2/3 个连续 `float` |
| 句柄 | 实体和脚本均为不透明 64 位整数 |
| 内存所有权 | 所有指针仅在调用期间有效；C# 不得保存宿主指针 |
| 异常 | 宿主函数不抛异常；插件导出函数必须捕获 |
| 版本 | `L3D_SCRIPT_ABI_VERSION == 1`；`structSize` 用于拒绝截断布局 |

### 9.2 EngineApiV1 函数

| 字段 | C++ 签名 | 说明 |
|---|---|---|
| `logMessage` | `void(*)(void*, int32_t, const char*, uint32_t)` | 写引擎日志 |
| `getPosition` | `int32_t(*)(void*, uint64_t, L3D_Vec3*)` | 读取 Transform 局部位置 |
| `setPosition` | `void(*)(void*, uint64_t, L3D_Vec3)` | 写 Transform 局部位置 |
| `getRotationEuler` | `int32_t(*)(void*, uint64_t, L3D_Vec3*)` | 读取欧拉角（度） |
| `setRotationEuler` | `void(*)(void*, uint64_t, L3D_Vec3)` | 写旋转四元数（输入为度） |
| `getVelocity` | `int32_t(*)(void*, uint64_t, L3D_Vec3*)` | 读取角色速度 |
| `setVelocity` | `void(*)(void*, uint64_t, L3D_Vec3)` | 写角色速度 |
| `isGrounded` | `int32_t(*)(void*, uint64_t)` | 角色是否着地 |
| `isKeyDown` | `int32_t(*)(void*, int32_t)` | 键状态 |
| `findEntityByTag` | `uint64_t(*)(void*, const char*, uint32_t)` | 按标签查找首个实体 |
| `hasCharacterController` | `int32_t(*)(void*, uint64_t)` | 组件存在性 |
| `destroyEntity` | `void(*)(void*, uint64_t)` | 递归销毁实体及其子实体 |
| `createEntity` | `uint64_t(*)(void*, const char*, uint32_t)` | 创建带 Transform/可选 Tag/Name 的实体 |
| `raycast` | `int32_t(*)(void*, L3D_Vec3, L3D_Vec3, float, uint64_t, uint64_t*, float*, L3D_Vec3*)` | 最近射线命中，支持忽略实体 |
| `isMouseButtonDown` | `int32_t(*)(void*, int32_t)` | 鼠标按钮状态 |
| `getMouseDelta` | `void(*)(void*, L3D_Vec2*)` | 本帧鼠标位移 |
| `getMousePosition` | `void(*)(void*, L3D_Vec2*)` | 鼠标位置 |
| `getMouseScrollDelta` | `float(*)(void*)` | 滚轮增量 |
| `getScale` | `int32_t(*)(void*, uint64_t, L3D_Vec3*)` | 读取 Transform 局部缩放 |
| `setScale` | `void(*)(void*, uint64_t, L3D_Vec3)` | 写 Transform 局部缩放 |

### 9.3 插件导出函数

| 导出 | 签名 | 作用 |
|---|---|---|
| `L3D_ScriptAbiVersion` | `uint32_t()` | 返回插件 ABI 版本 |
| `L3D_ScriptInitialize` | `int32_t(const L3D_EngineApiV1*)` | 校验并保存函数表，0 表示成功 |
| `L3D_ScriptCatalog` | `uint32_t(L3D_ScriptInfo*, uint32_t)` | 返回脚本数量/写入目录 |
| `L3D_ScriptCreate` | `int64_t(const char*, uint32_t, uint64_t, const char*, uint32_t)` | 实体句柄 + 脚本名 + 属性 JSON |
| `L3D_ScriptUpdate` | `void(int64_t, float)` | 固定步更新 |
| `L3D_ScriptCollision` | `void(int64_t, uint64_t, int32_t)` | 实体碰撞 Enter/Exit |
| `L3D_ScriptTrigger` | `void(int64_t, uint64_t, int32_t)` | 触发器 Enter/Exit |
| `L3D_ScriptDestroy` | `void(int64_t)` | 销毁脚本实例 |
| `L3D_ScriptShutdown` | `void()` | 清空插件状态 |

### 9.4 C# 参数解析

| 项目 | 实现 |
|---|---|
| JSON | `System.Text.Json` + `JsonSerializerContext` 源生成 |
| 属性类 | `PlayerControllerProperties`、`ThirdPersonCameraProperties`、`DoorTriggerProperties`、`PickupProperties`、`AbiProbeProperties` |
| 解析失败 | 记录警告并使用默认属性 |
| 未知脚本 | 记录警告，`L3D_ScriptCreate` 返回 0 |
| 异常隔离 | `L3D_ScriptUpdate/Collision/Trigger` 捕获异常并移除实例 |

## 10. 场景文件格式

文件扩展名：`.scene.json`；当前 `version == 1`。

```json
{
  "version": 1,
  "entities": [
    {
      "id": 0,
      "name": "Player",
      "components": {
        "transform": {
          "position": [0.0, 0.95, 2.0],
          "rotationEuler": [0.0, 0.0, 0.0],
          "scale": [0.8, 1.8, 0.8]
        },
        "meshRenderer": {
          "mesh": "builtin:cube",
          "material": "0b6c8a22-61f4-4f4e-9e5b-100000000011",
          "visible": true
        },
        "characterController": {
          "halfExtents": [0.4, 0.9, 0.4],
          "gravity": -26.0
        },
        "script": {
          "name": "PlayerController",
          "properties": "{\"moveSpeed\":6.0,\"jumpSpeed\":7.5}"
        },
        "tag": "Player"
      }
    }
  ]
}
```

| 字段 | 规则 |
|---|---|
| `id` | 场景内临时整数 id，用于层级父子引用，不持久化为运行时 Entity |
| `hierarchy.parent` | 指向父实体的场景 id |
| `mesh` / `material` | 资产 GUID 或 `builtin:*` |
| `script.properties` | 字符串化 JSON，由 C# 自行解析 |
| 版本不匹配 | 返回错误，不创建/替换 World |
| 缺少实体数组 | 返回错误 |
| 旋转 | 序列化为欧拉角（度） |

## 11. 资产元数据格式

每个可导入资产拥有同名 `.meta.json`：

```json
{
  "guid": "0b6c8a22-61f4-4f4e-9e5b-100000000002",
  "type": "Mesh",
  "importerVersion": 1
}
```

| 字段 | 规则 |
|---|---|
| `guid` | 全局唯一文本 ID；场景/材质通过它引用资产 |
| `type` | Mesh、Texture、Material、Scene、Shader |
| `importerVersion` | 导入器版本，当前为 1 |
| 缺失 `.meta.json` | 扫描时生成并写回（见资产实现） |
| 重复 GUID | 记录警告，后出现的资产跳过 |

## 12. 材质格式

文件扩展名：`.mat.json`。

```json
{
  "name": "Wood Crate",
  "baseColor": [0.90, 0.78, 0.62, 1.0],
  "specularColor": [0.22, 0.18, 0.12],
  "shininess": 12.0,
  "albedoTexture": "0b6c8a22-61f4-4f4e-9e5b-100000000001"
}
```

| 字段 | 语义 |
|---|---|
| `baseColor` | RGBA 基础色 |
| `specularColor` | 高光颜色 |
| `shininess` | Blinn-Phong 光泽度 |
| `albedoTexture` | 贴图 GUID；空字符串表示无贴图 |
| Shader | 当前统一使用内置 `lit.vert/lit.frag`；材质文件暂不保存独立 shader GUID |

## 13. OBJ 与 PNG 约束

| 类型 | 支持 | 不支持/忽略 |
|---|---|---|
| OBJ | `v`、`vn`、`vt`、`f`、负索引、凸多边形三角化、无索引顶点生成 | MTL、材质组、骨骼、动画、非凸多边形完整细分 |
| PNG | 通过 stb_image 解码，sRGB 纹理上传 | 压缩纹理、KTX、DDS、异步解码 |
| 着色器 | GLSL 4.10 Core，文件缺失回退内置源码 | 热重载、程序化节点、材质变体编译 |

## 14. 坐标与单位约定

| 项目 | 约定 |
|---|---|
| 上方向 | Y-up |
| 手性 | 右手坐标系 |
| 旋转存储 | `glm::quat`；序列化/脚本使用欧拉角（度） |
| 时间 | 秒；固定步长 1/60 秒 |
| 距离 | 场景单位，无物理单位换算 |
| 相机 | 透视投影；默认 FOV 60 度 |
