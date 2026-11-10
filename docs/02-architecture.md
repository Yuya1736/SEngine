# 软件体系结构说明

## 1. 架构概览

L3D Engine 采用“分层模块化单体 + 稀疏集合 ECS + 同步事件 + C ABI 插件边界”。所有运行时子系统位于同一进程、同一主线程，通过显式接口和组合根装配，不使用全局单例或服务定位器。

| 架构属性 | 选择 | 目的 |
|---|---|---|
| 部署形态 | 单个桌面进程 + 一个可替换的 C# DLL | 保持课程项目可构建、可调试，同时展示插件边界 |
| 模块形态 | 9 个静态库模块 + 1 个应用可执行文件 | 用编译期依赖表达架构约束 |
| 数据组织 | 实体 + 类型级稀疏集合 | 兼顾简单性、O(1) 组件访问和可解释性 |
| 交互方式 | 直接接口调用 + 同步事件 | 不引入消息队列、多线程和分布式复杂度 |
| 脚本边界 | 固定 C ABI 函数表 | 隔离 C++/C# 运行时、标准库和异常 |
| 编辑器隔离 | 运行时模块不依赖编辑器 | 防止编辑器逻辑污染运行时 |

## 2. 上下文视图

```text
┌──────────────┐       编辑/播放/保存       ┌────────────────────────┐
│  场景设计者  │ ─────────────────────────▶ │  engine_app            │
└──────────────┘                            │  Editor / Runtime      │
                                            └───────┬────────┬───────┘
┌──────────────┐         键鼠输入                   │        │
│    玩家      │ ──────────────────────────────────┘        │
└──────────────┘                                            │
                                                            │ C ABI
┌──────────────┐       资产/场景/材质文件                 ▼
│  文件系统    │ ◀────────────────────────── ┌────────────────────────┐
└──────────────┘                             │ GameScripts.dll        │
                                             │ C# / NativeAOT 玩法    │
                                             └────────────────────────┘
                                                            │
                                                            │ OpenGL 4.1
                                                            ▼
                                             ┌────────────────────────┐
                                             │ GPU / 窗口系统         │
                                             └────────────────────────┘
```

## 3. 模块与依赖规则

### 3.1 模块职责

| 模块 | 主要职责 | 允许依赖 |
|---|---|---|
| `engine_core` | `Result<T>`、日志、事件总线、GUID、输入快照、固定时间步、文件工具 | 无引擎模块 |
| `engine_platform` | GLFW 窗口、OpenGL 上下文创建、键鼠状态、时间 | `engine_core` |
| `engine_assets` | GUID、`.meta.json`、扫描、OBJ/PNG、材质和缓存 | `engine_core` |
| `engine_scene` | World、Entity、组件池、层级、场景序列化、Play 快照 | `engine_core`、`engine_assets` |
| `engine_physics` | AABB/球体、角色移动、触发器、射线和碰撞事件 | `engine_core`、`engine_scene` |
| `engine_renderer` | 资产上传、GLSL、FBO、Blinn-Phong、RenderFrame 消费 | `engine_core`、`engine_platform`、`engine_scene`、`engine_assets` |
| `engine_scripting` | DLL 加载、ABI 校验、脚本实例、C ABI 函数表 | `engine_core`、`engine_scene`、`engine_physics` |
| `engine_editor` | 层级、Inspector、资产、控制台、视口、Play 控制 | 所有运行时模块 |
| `engine_app` | 组合根、主循环、Editor/Runtime 切换、事件装配 | 所有模块 |
| `GameScripts` | C# 玩法脚本 | 只通过 `EngineApiV1` 访问引擎 |

### 3.2 依赖图

```text
engine_app
├─▶ engine_editor
├─▶ engine_renderer ──▶ engine_platform
├─▶ engine_scripting ──▶ engine_physics
├─▶ engine_physics ──▶ engine_scene
├─▶ engine_scene ──▶ engine_assets
├─▶ engine_assets ──▶ engine_core
└─▶ engine_platform ──▶ engine_core

engine_editor ──▶ renderer, physics, scripting, scene, assets, platform
engine_renderer ──▶ platform, scene, assets, core
engine_scripting ──▶ physics, scene, core
engine_physics ──▶ scene, core
engine_scene ──▶ assets, core
engine_assets ──▶ core
```

实际 CMake 依赖以各模块 `CMakeLists.txt` 为准。关键规则如下：

| 规则 | 检查方式 |
|---|---|
| `engine_core` 不包含来自其他引擎模块的头文件 | 代码审查与目标依赖 |
| 场景层不包含 OpenGL/GLFW 头文件 | 代码审查 |
| 运行时模块不包含 `l3d/editor/*` | 代码审查 |
| 编辑器可以被应用依赖，但不能被运行时反向依赖 | CMake 目标依赖 |
| C# 项目没有 C++ 项目引用，只使用手写 ABI 镜像结构 | `GameScripts.csproj` |
| 具体类型只在 `Application` 中构造 | 组合根审查 |

## 4. 运行时视图

### 4.1 主循环

```text
窗口轮询
  → 更新 InputSnapshot
  → 计算可变帧间隔
  → FixedTimestep.advance(frameDelta)
      for step in 0..N-1:
          ScriptSystem: 读取输入、写 Transform/CharacterController
          PhysicsSystem: 重力、角色移动、碰撞/触发器检测
      帧边界：统一向脚本派发 CollisionEnter/Exit、TriggerEnter/Exit
  → updateWorldTransforms
  → 构建 RenderFrame/RenderItem
  → OpenGLRenderer.render
  → 显示 FBO 或 ImGui 视口
```

| 项目 | 语义 |
|---|---|
| 固定步长 | 60 Hz，`1/60 s` |
| 最大补步 | 5 步；超过后丢弃未消费累积时间并标记 clamped |
| 帧间隔上限 | 0.25 秒，防止调试断点后模拟爆炸 |
| 线程 | 输入、模拟、脚本、渲染、ImGui 全部主线程 |
| 事件派发 | 物理步内收集，整批固定步结束后统一派发 |
| 变换刷新 | 所有固定步和事件派发之后刷新世界矩阵 |

### 4.2 Runtime 模式

```text
load scene
  → load GameScripts.dll
  → scripts.start(world, physics, input)
  → 每帧 stepSimulation
  → 选择 primary Camera 构建 RenderFrame
  → 渲染到默认帧缓冲
```

### 4.3 Editor 模式

```text
ImGui NewFrame
  → 绘制 Hierarchy/Inspector/Assets/Console/Viewport/Toolbar
  → Application 处理编辑器请求
  → editing: 刷新变换并渲染编辑器相机
  → playing: 执行与 Runtime 相同的固定步模拟
  → 渲染到离屏 FBO，再由 ImGui 显示
```

## 5. ECS 与组件视图

### 5.1 实体

| 字段 | 宽度 | 作用 |
|---|---|---|
| `index` | 32 位 | 稀疏集合与槽位索引 |
| `generation` | 32 位 | 删除后使旧句柄失效 |

实体存储在 `World` 的槽位数组中。销毁实体时 generation 加一，索引进入自由列表；组件池删除时使用 swap-and-pop。`World::alive` 同时检查槽位存活和 generation 相等。

### 5.2 稀疏集合

```text
Entity index ──▶ sparse[index] ──▶ dense position
                                      │
                                      ├─▶ dense entity
                                      └─▶ dense component
```

| 操作 | 复杂度 | 说明 |
|---|---:|---|
| `has` | O(1) | 稀疏索引 + generation 校验 |
| `tryGet` | O(1) | 通过稠密索引访问组件 |
| `add` | 均摊 O(1) | 新元素追加；重复添加覆盖 |
| `remove` | O(1) | swap-and-pop，更新被移动实体的稀疏索引 |
| `clone` | O(n) | Play 快照和 World 复制 |

### 5.3 核心组件

| 组件 | 关键字段 |
|---|---|
| `Transform` | 局部 position/rotation/scale，缓存 worldMatrix |
| `Hierarchy` | 父实体 |
| `CameraComponent` | FOV、near、far、primary |
| `MeshRenderer` | mesh GUID、material GUID、visible |
| `DirectionalLight` | direction、color、intensity |
| `PointLight` | color、intensity、range |
| `BoxCollider` | halfExtents、trigger |
| `SphereCollider` | radius、trigger |
| `CharacterController` | halfExtents、velocity、gravity、grounded |
| `ScriptComponent` | 稳定脚本名称、参数 JSON |
| `TagComponent` | 玩法查询标签 |
| `NameComponent` | 编辑器显示名称 |

### 5.4 层级变换

`updateWorldTransforms` 从根节点向子节点传播：`world = parent.world * local`。循环父子关系由 `setParent` 拒绝。序列化时使用临时实体 id 映射父子关系，不持久化运行时指针。

## 6. 渲染架构

```text
World / ECS
   │  extractRenderFrame
   ▼
RenderFrame
   ├─ view / projection / cameraPosition
   ├─ directional light
   ├─ up to 8 point lights
   └─ RenderItem[]（meshGuid、materialGuid、worldMatrix）
   │
   ▼
IRenderer
   └─ OpenGLRenderer
       ├─ 资产 GUID → CPU MeshData/TextureData/MaterialAsset
       ├─ VAO/VBO/EBO、纹理、着色器缓存
       ├─ 离屏颜色 + 深度模板附件
       ├─ 深度测试开启、面部剔除关闭
       └─ 默认帧缓冲呈现
```

| 解耦点 | 效果 |
|---|---|
| `RenderFrame` 不含 OpenGL 类型 | 场景层不依赖 GL，渲染后端可替换 |
| 渲染器不访问 ECS | 渲染只消费不可变帧描述 |
| 内置几何体 GUID | 无资产场景也能显示基础内容 |
| GLSL 内置回退 | 着色器文件缺失时仍可渲染 |
| 资产 GUID 到 GPU 缓存 | 避免每帧重复解析/上传 |

## 7. 物理与事件架构

| 项目 | 实现 |
|---|---|
| 碰撞形状 | 世界轴 AABB、球体、角色 AABB |
| 角色移动 | X/Z/Y 分轴移动，遇阻时清零对应速度；Y 轴向下碰撞设置 grounded |
| 触发器 | 只对角色与其他触发器/碰撞体检测，进入/离开边沿去重 |
| 射线 | 最近命中，支持忽略指定实体（通常为自身） |
| 事件 | `CollisionEnter/Exit`、`TriggerEnter/Exit`、`EntityDestroyed`、`SceneLoaded`、`PlayModeChanged` |
| 重入控制 | Script Update 与事件回调分离；事件在固定步批次结束后统一派发 |
| 句柄稳定 | 事件和 ABI 使用 generation 高 32 位、index 低 32 位的 64 位句柄 |

## 8. 编辑器架构

| 面板/能力 | 数据来源 | 写入路径 |
|---|---|---|
| Hierarchy | `World::entities`、`NameComponent`、`Hierarchy` | 创建/删除实体，设置父子 |
| Inspector | 选中实体的组件池 | 直接编辑组件字段 |
| Assets | `IAssetDatabase::assets` | 手动 Refresh，重新扫描目录 |
| Console | 日志单例快照 | 只读展示和清空 |
| Viewport | 编辑器相机 + `extractRenderFrameWithCamera` | 鼠标选择、摄像机飞行 |
| Toolbar | 编辑器请求结构 | `Application::processEditorRequests` |
| Play/Stop | `EditorSession` 快照 | `Application::beginPlay/stopPlay` |

编辑器只发出请求，不直接执行场景加载、保存或模式切换；组合根统一处理副作用，保持编辑器与文件系统/脚本生命周期解耦。

## 9. 脚本与 C ABI 架构

```text
C++ ScriptHost
  ├─ LoadLibraryW(GameScripts.dll)
  ├─ GetProcAddress 必需导出
  ├─ ABI 版本校验
  ├─ 传入只读 EngineApiV1 函数表
  ├─ 枚举脚本目录
  ├─ 创建脚本实例：稳定名称 + 实体句柄 + 属性 JSON
  ├─ Update / Collision / Trigger 回调
  └─ 捕获 C# 边界内异常，禁用失败实例
```

| 边界规则 | 实现 |
|---|---|
| 字符串 | UTF-8 `const char*` + 显式长度 |
| 实体/脚本句柄 | 不透明 64 位整数 |
| 结构体 | 固定字段顺序，`structSize` 校验 |
| 异常 | C# 导出函数内捕获，C ABI 只返回状态码 |
| 生命周期 | Play 开始创建，Stop/场景切换销毁，插件不热重载 |
| 能力 | Transform、角色速度、按键、鼠标、标签查询、实体创建/销毁、射线、日志 |

## 10. 错误处理与恢复

| 边界 | 失败处理 |
|---|---|
| 文件读取 | `Result<T>` 返回错误字符串 |
| 场景解析 | 版本/结构/组件校验失败则返回错误，不替换当前 World |
| 资产导入 | 单资产失败记录警告并跳过，不终止扫描 |
| 渲染器初始化 | 返回 `false`，应用退出 |
| 插件缺失 | 记录警告，引擎继续运行但无玩法脚本 |
| 插件 ABI 不匹配 | 卸载 DLL 并禁用脚本，场景仍可加载 |
| C# 异常 | 捕获、记录、移除脚本实例，不让异常跨 ABI |
| Play 恢复 | 退出 Play 时用备份 World 替换运行时 World |

## 11. 可替换点与演进方向

| 扩展点 | 当前实现 | 替换/演进方式 |
|---|---|---|
| 渲染后端 | `OpenGLRenderer` | 实现 `IRenderer`，保持 `RenderFrame` 契约 |
| 物理 | `SimplePhysicsWorld` | 实现 `IPhysicsWorld`，注入 `Application` 和 `ScriptHost` |
| 资产数据库 | `AssetDatabase` | 实现 `IAssetDatabase`，可接入打包资产或远程缓存 |
| 脚本宿主 | `ScriptHost` + NativeAOT | 实现 `IScriptHost` 或新增其他 ABI 版本 |
| ECS 存储 | 类型级稀疏集合 | 保留 `World` 公共查询语义，替换内部为 Archetype |
| 编辑器 | ImGui 单窗口 | 拆分面板接口，增加命令栈和撤销重做 |
| 渲染特性 | Forward Blinn-Phong | 增加 Shadow Map、PBR、后处理，保持 RenderFrame 扩展字段 |

## 12. 目录结构

| 路径 | 内容 |
|---|---|
| `engine/core` | 基础类型、日志、事件、输入、时间步、UUID、文件工具 |
| `engine/platform` | GLFW/OpenGL 窗口 |
| `engine/assets` | 资产数据库和 OBJ/PNG 加载器 |
| `engine/scene` | World/ECS、组件、变换、序列化、Play 会话 |
| `engine/physics` | 简单物理实现 |
| `engine/renderer` | 渲染提取、OpenGL 渲染器 |
| `engine/scripting` | C ABI 定义和脚本宿主 |
| `engine/editor` | ImGui 编辑器 |
| `app` | 组合根和主循环 |
| `scripts/GameScripts` | C# NativeAOT 插件 |
| `assets` | 示例场景、模型、贴图、材质、着色器 |
| `tests` | Catch2 单元/集成/渲染冒烟测试 |
| `tools` | 性能场景生成器等开发工具 |
