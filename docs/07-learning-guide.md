# L3D Engine 源码阅读与架构入门

> 面向第一次阅读游戏引擎源码的读者。默认你掌握基本 C++ 语法、类、模板和 `std::vector`，但不要求有 OpenGL、ECS 或商业引擎经验。
>
> 本文以 2026-10-21 的源码快照为准。文中的行号用于快速定位，源码变化后应优先按符号名搜索。

## 0. 学习目标

读完后，你应该能够回答以下问题：

| 编号 | 问题 |
|---|---|
| G-01 | 程序从哪里启动，哪些对象由谁创建和销毁？ |
| G-02 | 一帧中脚本、物理、变换和渲染的先后顺序是什么？ |
| G-03 | `Entity`、`World`、组件和系统之间是什么关系？ |
| G-04 | 为什么实体需要 `generation`，悬空实体句柄如何被拒绝？ |
| G-05 | 玩家输入如何从 C# 脚本传到物理系统？ |
| G-06 | 场景为什么先提取成 `RenderFrame`，再交给渲染器？ |
| G-07 | C++ 与 C# 之间如何在不传递 C++ 对象的情况下通信？ |
| G-08 | Play 模式为什么能完整恢复运行前的场景？ |
| G-09 | 哪些边界使用 `Result<T>`，哪些边界使用状态码？ |
| G-10 | 如何用测试和调试器验证自己的理解？ |

配套阅读：

| 文档 | 用途 |
|---|---|
| `docs/01-requirements.md` | 需求和质量属性 |
| `docs/02-architecture.md` | 完整架构视图 |
| `docs/03-adr.md` | 关键设计决策及理由 |
| `docs/04-interfaces.md` | 公共接口和数据格式 |
| `docs/05-test-report.md` | 可执行行为清单和验收结果 |
| `docs/06-demo.md` | 演示流程和答辩说明 |

## 1. 先建立正确的心智模型

### 1.1 引擎不是“直接画模型”

更准确的理解是：引擎维护一个场景数据库，每帧先更新它，再从中提取一份“绘制清单”。

```text
窗口与输入
    ↓
固定步长模拟
    ├─ C# 脚本写组件
    ├─ 物理系统改组件
    ├─ 碰撞事件派发
    └─ 层级变换更新
    ↓
World（实体和组件）
    ↓ 提取
RenderFrame（相机、灯光、渲染项）
    ↓
OpenGLRenderer
    ↓
离屏纹理 / 默认帧缓冲
```

一句话记忆：

> **World 保存状态，System 修改状态，RenderFrame 描述画面，Renderer 只负责执行绘制。**

### 1.2 六个核心类比

| 概念 | 类比 | 本项目中的实现 |
|---|---|---|
| `World` | 场景数据库 | 实体表、空闲索引和组件池 |
| `Entity` | 数据行 ID | 32 位索引 + 32 位 generation |
| Component | 数据列 | `Transform`、`MeshRenderer` 等纯数据结构 |
| System | 处理数据的函数 | 脚本、物理、变换和渲染提取 |
| `RenderFrame` | 交给画家的绘制清单 | 相机矩阵、灯光、变换和资源 GUID |
| `Application` | 总导演与装配工 | 组合根、主循环和编辑器/运行时切换 |

### 1.3 三类代码要分开看

| 类型 | 特征 | 例子 |
|---|---|---|
| 数据 | 尽量只有字段，没有复杂行为 | `components.h`、`render_types.h` |
| 行为 | 接收 World 或数据并更新它 | `physics_world.cpp`、`transform_system.cpp` |
| 装配 | 创建对象、连接接口、安排顺序 | `app/src/application.cpp` |

阅读时先判断一个文件属于哪一类，比逐行阅读更有效。

## 2. 仓库地图

### 2.1 模块职责

| 模块 | 一句话职责 | 首先打开的文件 |
|---|---|---|
| `engine_core` | 错误、日志、事件、输入、时间步、GUID | `engine/core/include/l3d/core/result.h` |
| `engine_platform` | 窗口、OpenGL 上下文、键鼠状态 | `engine/platform/src/window.cpp` |
| `engine_assets` | 扫描资产、GUID、OBJ/PNG/材质加载 | `engine/assets/include/l3d/assets/asset_database.h:23` |
| `engine_scene` | ECS、组件、层级、场景序列化、Play 快照 | `engine/scene/include/l3d/scene/world.h:88` |
| `engine_physics` | 角色移动、碰撞、触发器、射线 | `engine/physics/include/l3d/physics/physics_world.h:30` |
| `engine_renderer` | 渲染提取、GPU 资源、OpenGL 绘制 | `engine/renderer/src/renderer.cpp:509` |
| `engine_scripting` | 加载 DLL、校验 ABI、管理脚本实例 | `engine/scripting/src/script_host.cpp:61` |
| `engine_editor` | Hierarchy、Inspector、Assets、Console、Viewport | `engine/editor/src/editor.cpp:160` |
| `engine_app` | 组合根、主循环、模式切换、保存/加载 | `app/src/application.cpp:369` |
| `GameScripts` | 玩法逻辑和 C ABI 客户端 | `scripts/GameScripts/NativeAbi.cs` |

### 2.2 依赖方向

```text
engine_app
├─▶ engine_editor
├─▶ engine_renderer ──▶ engine_platform
├─▶ engine_scripting ──▶ engine_physics
├─▶ engine_physics ──▶ engine_scene
├─▶ engine_scene ──▶ engine_assets
├─▶ engine_assets ──▶ engine_core
└─▶ engine_platform ──▶ engine_core
```

需要记住两条规则：

1. 运行时模块不能反向依赖 `engine_editor`。
2. 只有 `engine_app` 知道所有具体实现，并负责把它们装配起来。

这就是“模块化单体 + 组合根”。它不是微服务，也不是把所有代码塞进一个大类，而是用编译期依赖表达边界。

### 2.3 推荐阅读顺序

| 顺序 | 文件 | 原因 |
|---:|---|---|
| 1 | `app/src/main.cpp` | 找到程序入口 |
| 2 | `app/src/application.cpp` | 看对象如何被创建和驱动 |
| 3 | `engine/scene/include/l3d/scene/entity.h` | 理解实体只是句柄 |
| 4 | `engine/scene/include/l3d/scene/world.h` | 理解 ECS 存储 |
| 5 | `engine/scene/include/l3d/scene/components.h` | 认识数据模型 |
| 6 | `engine/scene/src/transform_system.cpp` | 理解系统如何批量更新 |
| 7 | `engine/physics/src/physics_world.cpp` | 理解角色、碰撞和事件 |
| 8 | `engine/renderer/src/render_extraction.cpp` | 理解渲染解耦 |
| 9 | `engine/scripting/src/script_host.cpp` | 理解插件边界 |
| 10 | `engine/editor/src/editor.cpp` | 理解编辑器如何发出请求 |

不建议一开始从 `renderer.cpp` 的 OpenGL 初始化部分读起。它细节多，但对理解整体数据流帮助较小。

## 3. 第一课：程序如何启动

### 3.1 `main` 只做参数解析和对象启动

程序入口在 `app/src/main.cpp:17`。它主要做两件事：

1. 把 `--runtime`、`--hidden`、`--frames`、`--scene` 等参数解析到 `ApplicationConfig`。
2. 构造 `Application`，调用 `run()`。

`main` 不负责创建窗口、OpenGL 或 World，这符合组合根原则：入口应保持简单，具体装配交给应用层。

### 3.2 `Ap plication::initialize`

初始化入口是 `app/src/application.cpp:87`，顺序如下：

| 顺序 | 动作 | 位置 |
|---:|---|---|
| 1 | 找到资产根目录 | `app/src/application.cpp:88` |
| 2 | 创建 GLFW 窗口和 OpenGL 上下文 | `app/src/application.cpp:99` |
| 3 | 扫描资产并建立 GUID 索引 | `app/src/application.cpp:103` |
| 4 | 初始化 OpenGL 渲染器 | `app/src/application.cpp:108` |
| 5 | 加载 `GameScripts.dll` | `app/src/application.cpp:113` |
| 6 | 加载场景，失败则创建默认场景 | `app/src/application.cpp:121` |
| 7 | 编辑器模式创建 ImGui 编辑器 | `app/src/application.cpp:127` |
| 8 | Runtime 模式立即启动脚本 | `app/src/application.cpp:133` |

关键设计是**脚本加载失败不阻塞场景加载**。因此即使 `GameScripts.dll` 缺失或 ABI 不匹配，引擎仍能显示世界，只是没有玩法行为。

### 3.3 `Application::run`

主循环从 `app/src/application.cpp:369` 开始：

| 步骤 | 代码行为 |
|---|---|
| 检查窗口是否关闭 | `while (!window_.shouldClose())` |
| 收集输入和窗口事件 | `window_.pollEvents()` |
| 计算帧间隔 | `frameDelta = clamp(now - previousTime_, 0.0, 0.1)` |
| Runtime 模式 | 模拟、提取、渲染、呈现 |
| Editor 模式 | ImGui 界面、编辑器请求、可选模拟、视口渲染 |
| 可选截图 | 从默认帧缓冲读回像素 |
| 交换缓冲 | `window_.swapBuffers()` |

这一层是“驱动层”，负责顺序，不负责具体物理或绘制细节。

### 第一课检查点

你应该能回答：

1. `GameScripts.dll` 加载失败时，为什么场景仍能显示？
2. 窗口、资产数据库、渲染器和脚本宿主由谁创建？
3. `main.cpp` 为什么不直接创建 `World`？

## 4. 第二课：一帧的完整调用链

### 4.1 两种运行模式

| 模式 | 入口 | 每帧行为 |
|---|---|---|
| Runtime | `--runtime` | 始终推进模拟，直接渲染到默认帧缓冲 |
| Editor | 默认启动 | 先绘制编辑界面，只有 Play 状态才推进模拟 |

Runtime 分支在 `app/src/application.cpp:385`，Editor 分支在 `app/src/application.cpp:391`。

### 4.2 固定时间步

模拟时间步长为 `1/60` 秒。`FixedTimestep::advance` 的核心逻辑在 `engine/core/src/fixed_timestep.cpp:10`：

```text
accumulator += min(frameDelta, maxFrameDelta)
steps = min(floor(accumulator / fixedDelta), maxSteps)
accumulator -= steps * fixedDelta
```

默认最多执行 5 个补步。这样即使某一帧卡顿，也最多补 5 次模拟，避免“卡顿后实体瞬移很远”。

当前 `StepResult.alpha` 用于表示两个固定步之间的插值比例，但渲染尚未使用插值，画面显示的是最新模拟状态。这是一个明确的演进点。

### 4.3 系统顺序

模拟入口是 `Application::stepSimulation`，位于 `app/src/application.cpp:332`：

```text
对每个固定步：
    ScriptSystem
    PhysicsSystem

所有固定步结束后：
    批量派发碰撞/触发器事件
    TransformSystem
```

对应代码：

| 行为 | 位置 | 说明 |
|---|---|---|
| 脚本更新 | `app/src/application.cpp:339` | 读取输入，写速度等组件字段 |
| 物理更新 | `app/src/application.cpp:340` | 消费速度，执行重力、碰撞和触发器检测 |
| 事件派发 | `app/src/application.cpp:342` | 在所有补步结束后统一派发给 C# |
| 世界变换 | `app/src/application.cpp:345` | 计算父子层级的最终矩阵 |

为什么事件不在物理 `step` 内立即回调脚本？因为那会让“脚本更新中又进入脚本”，增加重入和状态不一致风险。把事件放到固定步批次结束后，时序更清楚。

### 4.4 Runtime 的渲染链

Runtime 每帧执行：

```text
stepSimulation()
    ↓
找到 primary CameraComponent
    ↓
extractRenderFrame()
    ↓
renderer_.render(frame, ...)
    ↓
renderer_.presentToDefaultFramebuffer(...)
```

入口分别在 `app/src/application.cpp:386`、`app/src/application.cpp:348` 和 `app/src/application.cpp:388`。

### 4.5 Editor 的渲染链

Editor 每帧执行：

| 顺序 | 行为 | 位置 |
|---:|---|---|
| 1 | 绘制 ImGui 界面 | `app/src/application.cpp:401` |
| 2 | 处理 Save/Load/Play 等请求 | `app/src/application.cpp:402` |
| 3 | Play 时推进模拟，否则只更新变换 | `app/src/application.cpp:403` |
| 4 | 从编辑器相机提取 RenderFrame | `app/src/application.cpp:409` |
| 5 | 渲染到离屏视口 | `app/src/application.cpp:410` |
| 6 | ImGui 把视口纹理画进面板 | `app/src/application.cpp:415` |

### 第二课检查点

1. 脚本更新和物理更新的先后顺序是什么？
2. 一帧内如果执行了 3 个固定步，碰撞事件会派发几次？
3. 为什么编辑器模式下停止状态仍然需要更新层级变换？

## 5. 第三课：ECS 与场景数据

### 5.1 `Entity` 不是对象指针

实体定义在 `engine/scene/include/l3d/scene/entity.h:9`：

```cpp
struct Entity {
    std::uint32_t index;
    std::uint32_t generation;
};
```

它不是 `GameObject*`，也不包含组件。实体只是一个可复制的句柄，组件实际存放在 `World` 的组件池中。

### 5.2 generation 解决悬空句柄

假设 Player 的索引是 7，generation 是 0。销毁 Player 时：

1. 索引 7 进入空闲列表。
2. generation 从 0 增加到 1。
3. 以后创建的新实体可以复用索引 7，但 generation 会是 1。
4. 旧句柄 `{7, 0}` 会被 `World::alive()` 拒绝。

创建、销毁和存活检查分别在：

- `engine/scene/src/world.cpp:30`
- `engine/scene/src/world.cpp:45`
- `engine/scene/src/world.cpp:74`

这避免了 C++ 中常见的 use-after-free 式逻辑错误，尤其适合 C# 持有实体句柄的场景。

### 5.3 稀疏集合

`SparseSet<Component>` 位于 `engine/scene/include/l3d/scene/world.h:24`，由三块数据组成：

```text
sparse_[entity.index] = denseIndex

entities_[denseIndex]   = Entity
components_[denseIndex] = Component
```

比喻：

| 数组 | 作用 |
|---|---|
| `sparse_` | 从实体索引快速找到“第几行” |
| `entities_` | 紧凑保存当前拥有该组件的实体 |
| `components_` | 与实体一一对应的组件数据 |

查询时先通过 `sparse_` 得到 dense 下标，再直接访问组件，因此增删查都是 O(1)。删除时把最后一个元素搬到空位，再弹出尾部，保持数组紧凑。

### 5.4 World 的职责

`World` 在 `engine/scene/include/l3d/scene/world.h:88`，负责：

- 创建和销毁实体。
- 维护存活实体列表和空闲索引。
- 按组件类型延迟创建组件池。
- 检查句柄是否仍然有效。
- 在销毁实体时发布 `EntityDestroyedEvent`。
- 提供复制能力，用于 Play 快照。

公共操作非常小：

```text
world.create()
world.destroy(entity)
world.add<Transform>(entity, transform)
world.tryGet<Transform>(entity)
world.has<MeshRenderer>(entity)
world.remove<BoxCollider>(entity)
```

添加和查询的泛型入口在 `engine/scene/include/l3d/scene/world.h:104`，组件池查找在 `engine/scene/include/l3d/scene/world.h:140`。

### 5.5 组件是数据，不是基类

组件定义在 `engine/scene/include/l3d/scene/components.h`：

| 组件 | 关键字段 | 用途 |
|---|---|---|
| `Transform` | 位置、四元数旋转、缩放、世界矩阵 | 所有可见对象和层级节点 |
| `Hierarchy` | 父实体 | 建立父子关系 |
| `CameraComponent` | FOV、近远裁剪、primary | Runtime 主相机 |
| `MeshRenderer` | 网格 GUID、材质 GUID、visible | 渲染提取 |
| `DirectionalLight` | 方向、颜色、强度 | 全局方向光 |
| `PointLight` | 颜色、强度、范围 | 局部点光 |
| `BoxCollider` | 半尺寸、trigger | AABB 碰撞或触发器 |
| `SphereCollider` | 半径、trigger | 球形碰撞或触发器 |
| `CharacterController` | 半尺寸、速度、重力、grounded | 运动学角色 |
| `ScriptComponent` | 脚本名称、参数 JSON | C# 实例创建依据 |
| `TagComponent` | 字符串标签 | 按标签查找实体 |

这种设计避免“所有组件继承同一个 Component 基类”，组件可以保持简单、可复制和可序列化。

### 5.6 场景 JSON 如何恢复 World

示例场景在 `assets/scenes/sample.scene.json`。加载和保存入口分别是：

- `engine/scene/src/scene_serializer.cpp:101`
- `engine/scene/src/scene_serializer.cpp:223`

加载流程：

```text
读取 JSON
    ↓
检查 version
    ↓
解析实体 id、name、components
    ↓
创建 Entity
    ↓
添加 Transform、MeshRenderer 等组件
    ↓
解析 Hierarchy 父子关系
    ↓
返回新的 World
```

加载失败时返回 `Result`，`Application` 不会用半成品覆盖当前 World。这是错误隔离在场景层的体现。

### 第三课检查点

1. 为什么删除实体后不能只把 `alive` 设为 false？
2. `sparse_`、`entities_` 和 `components_` 分别解决什么问题？
3. 场景 JSON 中为什么保存资源 GUID，而不是保存 C++ 指针？

## 6. 第四课：Transform 与物理

### 6.1 局部变换到世界变换

`Transform` 包含本地位置、旋转和缩放。`updateWorldTransforms` 在 `engine/scene/src/transform_system.cpp:22` 计算：

```text
local = T(position) * R(rotation) * S(scale)
world = parentWorld * local
```

局部矩阵构造在 `engine/scene/src/transform_system.cpp:15`。

函数使用三种状态做深度优先解析：

| 状态 | 含义 |
|---:|---|
| 0 | 未访问 |
| 1 | 正在访问，若再次遇到说明存在环 |
| 2 | 已完成，直接返回缓存的世界矩阵 |

发现父子循环时只记录一次警告并拒绝继续递归，不会无限循环。

### 6.2 物理只实现需要的子集

物理接口 `IPhysicsWorld` 在 `engine/physics/include/l3d/physics/physics_world.h:30`，提供：

```cpp
step(world, delta, events)
raycast(world, origin, direction, maxDistance, ignore)
overlap(world, first, second)
reset()
```

当前实现是 `SimplePhysicsWorld`，只处理课程演示需要的能力，不追求完整刚体动力学。

### 6.3 角色移动为什么“脚本先、物理后”

`PlayerController.Update` 在 `scripts/GameScripts/NativeAbi.cs:416` 中做的是：

1. 读取 WASD。
2. 计算水平速度。
3. 读取 `CharacterController.velocity`。
4. 写回新的速度。

它不直接修改 Player 的 Transform。物理系统随后在 `engine/physics/src/physics_world.cpp:176`：

1. 给 Y 方向速度施加重力。
2. 用速度乘 delta 得到位移。
3. 分别尝试 X、Z、Y 轴移动。
4. 被挡住时清空对应速度。
5. 向下碰撞时设置 `grounded = true`。

这种分工让玩法脚本只表达“我想怎么移动”，物理层统一负责碰撞和落地。

### 6.4 触发器与事件边沿

物理系统会分别收集：

- 角色与触发器之间的重叠。
- 实体与实体之间的实体碰撞。

然后与上一帧的接触集合比较，只在“进入”或“离开”时生成事件，见 `engine/physics/src/physics_world.cpp:258` 和 `engine/physics/src/physics_world.cpp:270`。

因此：

| 状态变化 | 事件 |
|---|---|
| 上一帧没接触，当前帧接触 | `Enter` |
| 上一帧接触，当前帧仍接触 | 不重复发事件 |
| 上一帧接触，当前帧离开 | `Exit` |

事件最终由 `ScriptHost::dispatchEvents` 转成 C# 回调，见 `engine/scripting/src/script_host.cpp:181`。

### 6.5 射线的用途

`raycast` 在 `engine/physics/src/physics_world.cpp:291`，返回最近命中的实体、距离和命中点。C# 脚本可以传一个 `ignore` 实体，通常用于排除自己。

### 第四课检查点

1. 为什么 PlayerController 不直接设置位置？
2. `grounded` 在哪个分支被设置？
3. 触发器持续重叠时，为什么不会每帧重复触发 `Enter`？
4. 父子循环为什么不能靠简单递归解决？

## 7. 第五课：渲染解耦

### 7.1 `RenderFrame` 是纯数据

`RenderFrame` 定义在 `engine/renderer/include/l3d/renderer/render_types.h:25`，包含：

| 字段 | 用途 |
|---|---|
| `view`、`projection` | 相机矩阵 |
| `cameraPosition` | 高光计算需要的位置 |
| `directionalDirection/Color/Intensity` | 方向光 |
| `pointLights` | 最多 8 个点光 |
| `items` | 网格 GUID、材质 GUID 和世界矩阵 |

`RenderItem` 定义在 `engine/renderer/include/l3d/renderer/render_types.h:19`。它不包含 `Entity`、组件指针或 OpenGL 对象。

### 7.2 提取器只读 World

`extractRenderFrame` 的公共入口在 `engine/renderer/include/l3d/renderer/render_extraction.h:11`，收集逻辑在 `engine/renderer/src/render_extraction.cpp:13`：

```text
更新世界变换
    ↓
遍历所有实体
    ├─ 找到第一个方向光
    ├─ 收集最多 8 个点光
    └─ 收集可见 MeshRenderer
    ↓
返回 RenderFrame
```

这一层的意义是把 ECS 细节转换成渲染器能理解的稳定数据。未来替换成 Vulkan、D3D 或其他渲染器时，场景层不需要知道图形 API 的存在。

### 7.3 渲染器如何消费帧

`OpenGLRenderer::render` 在 `engine/renderer/src/renderer.cpp:509`。它大致执行：

1. 绑定渲染目标和设置视口尺寸。
2. 清空颜色和深度。
3. 使用相机矩阵和灯光设置着色器 uniform。
4. 按 `RenderItem` 的 GUID 从资产库解析网格和材质。
5. 第一次使用时上传到 GPU，之后复用缓存。
6. 调用 `glDrawElements` 绘制三角形。

Runtime 模式最后调用 `presentToDefaultFramebuffer`；Editor 模式使用离屏纹理作为 ImGui 视口背景。

### 7.4 为什么渲染器不能直接遍历 ECS

如果渲染器直接访问 `World` 和所有组件类型，会产生以下耦合：

- 渲染器必须理解 ECS 存储布局。
- 新增组件可能迫使渲染器重新编译。
- 更换渲染后端时，场景代码也要改。
- 测试渲染器需要构造完整的 World。

当前设计把耦合压缩在一个提取函数里，这是本引擎最重要的架构边界之一。

### 第五课检查点

1. `RenderFrame` 中为什么不保存 `Entity`？
2. 没有 `MeshRenderer` 但有 `Transform` 的实体会不会出现在渲染项中？
3. 为什么点光数组限制为 8 个？

## 8. 第六课：C# 脚本与 C ABI

### 8.1 为什么需要 ABI

C++ 与 C# 的编译器、标准库和异常模型不同。直接传递 `std::string`、`std::vector`、`glm::vec3` 或 C++ 对象会产生布局和所有权问题。因此本项目使用固定 C 结构体、函数指针和基础 POD 数据。

ABI 版本和函数表定义在 `engine/scripting/include/l3d/scripting/script_abi.h:65`。

### 8.2 加载握手

`ScriptHost::load` 位于 `engine/scripting/src/script_host.cpp:61`，大致流程是：

```text
LoadLibraryW(GameScripts.dll)
    ↓
GetProcAddress 查找导出函数
    ↓
调用 L3D_ScriptAbiVersion
    ↓
检查版本是否等于 1
    ↓
构造 EngineApiV1 函数表
    ↓
调用 L3D_ScriptInitialize
    ↓
枚举脚本目录
```

版本不匹配、DLL 缺失或必需导出缺失时，脚本功能被禁用，但引擎继续加载和渲染场景。

### 8.3 函数表是“受控能力清单”

`EngineApiV1` 中包含：

- 日志。
- Transform 位置、旋转、缩放的读取和写入。
- CharacterController 速度、grounded。
- 键盘和鼠标。
- 按标签查找实体。
- 创建和销毁实体。
- 射线检测。

使用 `UserData` 指针把函数表绑定到具体的 `ScriptHost`，初始化过程在 `engine/scripting/src/script_host.cpp:253`。

C# 侧镜像定义在 `scripts/GameScripts/NativeAbi.cs:73`，通过 `delegate* unmanaged[Cdecl]` 调用 C++。

### 8.4 实体句柄如何跨语言

实体在 C++ 中是两个 32 位字段。跨 ABI 时编码成一个 64 位整数：

```text
高 32 位：generation
低 32 位：index
```

编码和解码分别在 `engine/scripting/src/script_host.cpp:214` 和 `engine/scripting/src/script_host.cpp:221`。

这样 C# 只持有一个不透明整数，不需要知道 C++ 的 `Entity` 布局。

### 8.5 脚本实例生命周期

| 阶段 | 行为 | 位置 |
|---|---|---|
| 加载 DLL | 只加载插件并校验 ABI | `engine/scripting/src/script_host.cpp:61` |
| 开始 Play | 遍历 World，按 `ScriptComponent` 创建实例 | `engine/scripting/src/script_host.cpp:125` |
| 固定步更新 | 调用每个实例的 `update` | `engine/scripting/src/script_host.cpp:157` |
| 事件派发 | 根据触发器/实体碰撞调用不同回调 | `engine/scripting/src/script_host.cpp:181` |
| 停止 Play | 销毁所有实例并解除绑定 | `engine/scripting/src/script_host.cpp:202` |

当前不支持热重载。修改 C# 后必须重新构建 DLL 并重启引擎。

### 8.6 C# 异常如何隔离

所有 C# 导出函数都有 `try/catch`，例如：

- `L3D_ScriptCreate` 在 `scripts/GameScripts/NativeAbi.cs:933`
- `L3D_ScriptUpdate` 在 `scripts/GameScripts/NativeAbi.cs:967`
- `L3D_ScriptTrigger` 在 `scripts/GameScripts/NativeAbi.cs:995`

异常在 C# 边界内被记录，实例可能被移除，但不会跨 ABI 抛回 C++。

### 8.7 用 PlayerController 理解脚本职责

`PlayerController` 位于 `scripts/GameScripts/NativeAbi.cs:405`，它只做以下事情：

1. 读取输入。
2. 计算目标水平速度。
3. 检查是否可以跳跃。
4. 写回速度。
5. 根据移动方向写入 Y 轴旋转。

它不负责碰撞解算，也不负责最终渲染。组件数据、物理执行和绘制分别由不同模块完成。

### 8.8 DoorTrigger 与 Pickup

| 脚本 | 行为 | 位置 |
|---|---|---|
| `DoorTrigger` | 进入触发器时设置开门状态，更新中平滑抬高门 | `scripts/GameScripts/NativeAbi.cs:532` |
| `Pickup` | 玩家进入球形触发器后销毁拾取物 | `scripts/GameScripts/NativeAbi.cs:594` |

这是“物理只产生事件，玩法脚本决定后果”的例子。物理系统不知道“门”和“能量球”是什么，它只知道实体发生重叠。

### 第六课检查点

1. 为什么 ABI 不直接传 `std::string`？
2. 为什么实体句柄需要 generation？
3. C# 异常为什么不能让 C++ 直接捕获？
4. 物理系统是否知道门会升降？

## 9. 第七课：编辑器、Play 快照与组合根

### 9.1 编辑器是 World 的观察者

编辑器入口是 `Editor::draw`，位于 `engine/editor/src/editor.cpp:160`，它绘制：

- Hierarchy：实体树和父子关系。
- Inspector：选中实体的组件字段。
- Assets：资产数据库内容。
- Console：日志。
- Viewport：渲染结果、相机移动和实体选择。

编辑器并不直接保存文件或启动脚本。它只设置 `EditorRequests`，由 `Application::processEditorRequests` 在 `app/src/application.cpp:289` 统一处理。

### 9.2 为什么要请求结构

如果按钮回调直接执行 `SceneSerializer::save`、启动脚本或销毁 World，编辑器会依赖文件系统、脚本宿主和模式切换细节，测试也会困难。

请求结构把“用户意图”和“副作用执行”分开：

```text
ImGui 按钮点击
    ↓
EditorRequests.saveScene = true
    ↓
Application 读取请求
    ↓
SceneSerializer::save(world, path)
```

### 9.3 Play 模式快照

`EditorSession` 位于 `engine/scene/include/l3d/scene/editor_session.h:9`。进入 Play 时：

1. 复制整个 World。
2. 启动脚本实例。
3. 重置物理状态和固定时间步累加器。

退出 Play 时：

1. 停止并销毁脚本实例。
2. 用备份 World 替换运行时 World。
3. 重新计算层级变换并重新绑定事件总线。

实现入口在 `app/src/application.cpp:256` 和 `app/src/application.cpp:271`。

这保证了 Play 中的拾取、开门、实体销毁不会污染作者编辑的场景。

### 9.4 组合根

`Application` 是唯一知道所有具体实现的地方。它创建：

- `Window`
- `AssetDatabase`
- `OpenGLRenderer`
- `ScriptHost`
- `SimplePhysicsWorld`
- `EditorSession`
- `Editor`

其他模块只看到接口，例如 `IRenderer`、`IAssetDatabase`、`IPhysicsWorld`、`IScriptHost`。这为替换实现和测试隔离提供了位置。

### 第七课检查点

1. 为什么编辑器不直接调用 `SceneSerializer::save`？
2. Play 模式复制的是 World 还是场景 JSON？
3. 哪些模块可以看到编辑器，哪些模块不能反向依赖编辑器？

## 10. 七课学习计划

| 课次 | 阅读内容 | 学习目标 | 练习 | 验收标准 |
|---:|---|---|---|---|
| 1 | `main.cpp`、`application.cpp:87`、`application.cpp:369` | 看懂启动和主循环 | 运行 Debug 程序，找出资产、场景和脚本日志 | 能说清初始化顺序 |
| 2 | `application.cpp:332`、`fixed_timestep.cpp` | 看懂固定时间步和系统顺序 | 在 `stepSimulation` 打断点，记录 3 帧的步数 | 能解释为什么最多补 5 步 |
| 3 | `entity.h`、`world.h`、`world.cpp` | 看懂 ECS 和 generation | 运行 `engine_tests.exe` 的实体测试 | 能解释悬空句柄为何失效 |
| 4 | `transform_system.cpp`、`physics_world.cpp` | 看懂层级、角色移动和事件 | 把 Player 的 `moveSpeed` 改为 3.0 | 能描述键盘到速度再到位移的链路 |
| 5 | `render_types.h`、`render_extraction.cpp`、`renderer.cpp` | 看懂渲染解耦 | 隐藏 Marker Ring 或修改点光强度 | 能画出 World 到 RenderFrame 的流程 |
| 6 | `script_host.cpp`、`NativeAbi.cs` | 看懂 C ABI 和玩法脚本 | 修改门速度或开门高度 | 能解释 C# 如何取得速度并写回 |
| 7 | `editor.cpp`、`editor_session.h`、测试 | 看懂编辑器请求和 Play 快照 | 完成一次选择、修改、保存、Play、Stop | 能解释为什么 Stop 能恢复场景 |

建议每课控制在 1–2 小时，先读代码，再做练习，最后用自己的话写下结论。不要只收藏结论而不跑程序。

## 11. 调试与验证方法

### 11.1 构建和运行

```powershell
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
build\bin\Debug\engine_app.exe --scene assets\scenes\sample.scene.json
```

无界面冒烟：

```powershell
build\bin\Debug\engine_app.exe --runtime --hidden --no-vsync --frames 300
```

### 11.2 推荐断点

| 断点位置 | 观察变量 | 目的 |
|---|---|---|
| `Application::run` | `frameDelta`、`frameCount` | 观察帧循环 |
| `Application::stepSimulation` | `step.steps`、`step.accumulator` | 观察固定步补偿 |
| `ScriptHost::update` | `instance.entity`、`delta` | 观察脚本实例 |
| `SimplePhysicsWorld::step` | `controller->velocity`、`grounded` | 观察角色移动 |
| `ScriptHost::dispatchEvents` | `event.trigger`、`event.phase` | 观察触发事件 |
| `extractRenderFrame` | `frame.items.size()` | 观察渲染项数量 |
| `OpenGLRenderer::render` | `width`、`height` | 观察绘制尺寸 |
| `Application::beginPlay` / `stopPlay` | World 地址、实体数 | 观察快照恢复 |

### 11.3 用搜索代替盲读

```powershell
rg -n "PlayerController" .
rg -n "trigger" engine/physics scripts/GameScripts
rg -n "RenderFrame" app engine
rg -n "tryGet<CharacterController>" engine
```

阅读一个功能时，沿着“数据从哪来、谁修改、谁消费”搜索，通常比从头读文件更高效。

### 11.4 把测试当行为说明书

测试位于 `tests/engine_tests.cpp` 和 `tests/script_integration_tests.cpp`。例如：

| 测试 | 说明 |
|---|---|
| `Entity generations invalidate stale handles and component storage is reusable` | 验证 generation 和组件复用 |
| `Scene JSON round trip restores components, tags and hierarchy` | 验证场景往返 |
| `Character controller settles on a collider and trigger enter/exit fires once` | 验证落地和触发器边沿 |
| `NativeAOT gameplay ABI drives movement, triggers and exception isolation` | 验证真实 C# 插件端到端 |

如果代码和文档不一致，先看测试；测试通常更接近可执行行为。

## 12. 自测题

| 编号 | 问题 | 答题提示 |
|---:|---|---|
| Q-01 | `Entity` 为什么不是组件容器？ | 关注句柄与数据分离 |
| Q-02 | generation 何时增加？ | 看 `World::destroy` |
| Q-03 | 稀疏集合删除元素为何是 O(1)？ | 看末尾元素搬移 |
| Q-04 | 为什么脚本先于物理更新？ | 脚本写速度，物理消费速度 |
| Q-05 | 为什么碰撞事件要批量派发？ | 避免回调重入和时序不稳定 |
| Q-06 | 为什么 `RenderFrame` 不包含 OpenGL 类型？ | 保持渲染后端可替换 |
| Q-07 | C# 如何引用一个实体？ | 高 generation、低 index 的 64 位句柄 |
| Q-08 | 插件版本不匹配时发生什么？ | 禁用脚本，但场景仍能工作 |
| Q-09 | Play 停止后为什么场景能恢复？ | `EditorSession` 备份整个 World |
| Q-10 | 哪一层负责创建所有具体子系统？ | 组合根 `Application` |

## 13. 推荐的第一组动手练习

### 练习 A：改变玩家速度

修改 `assets/scenes/sample.scene.json` 中 Player 的 `moveSpeed`，从 `6.0` 改为 `3.0`。重新运行并 Play，确认移动速度变化。

这条练习验证：场景 JSON → 脚本参数 → C# 实例 → 速度 → 物理移动。

### 练习 B：改变开门高度或速度

修改 Door Sensor 的 `openHeight` 或 `speed`，观察门的行为。位置在 `assets/scenes/sample.scene.json` 的 DoorTrigger 脚本参数中。

这条练习验证：触发器事件 → DoorTrigger → 门 Transform → 渲染更新。

### 练习 C：关闭一个渲染项

把 Marker Ring 的 `meshRenderer.visible` 改为 `false`，确认它从画面消失，但实体仍然存在。

这条练习验证：`MeshRenderer` 数据 → 渲染提取过滤 → `RenderFrame.items` 数量变化。

### 练习 D：用断点观察一帧

在 `Application::stepSimulation`、`SimplePhysicsWorld::step` 和 `extractRenderFrame` 打断点，运行 Play，记录一次固定步中：

- `step.steps`
- Player 的速度
- 产生的碰撞事件数量
- `RenderFrame.items.size()`

如果能解释这些数字之间的关系，说明你已经掌握了本引擎的主数据流。

## 14. 常见误区

| 误区 | 正确理解 |
|---|---|
| “Entity 就是 GameObject” | 这里 Entity 只是索引和 generation，组件在 World 中 |
| “脚本直接移动玩家” | 脚本通常只设置速度，物理系统执行移动 |
| “渲染器遍历 ECS” | 渲染器只消费提取后的 `RenderFrame` |
| “事件应立即在物理内部回调” | 本项目在固定步批次结束后统一派发 |
| “C# 可以直接使用 C++ 类” | 只能通过固定 C ABI 和函数表 |
| “修改 C# 会自动生效” | 需要重新构建 DLL 并重启，当前无热重载 |
| “Play 修改的就是原场景” | Play 操作的是 World 副本，Stop 后恢复 |

## 15. 术语表

| 术语 | 解释 |
|---|---|
| ECS | Entity-Component-System，实体、组件、系统 |
| Sparse Set | 稀疏数组加速索引、紧凑数组保存数据的容器 |
| Generation | 实体槽位复用时的版本号，用于拒绝旧句柄 |
| Fixed Timestep | 使用固定 delta 推进模拟，避免物理依赖帧率 |
| Accumulator | 固定时间步中累计尚未消费的真实时间 |
| Render Extraction | 从场景数据生成渲染帧的过程 |
| ABI | 二进制接口，规定函数调用、结构布局和数据所有权 |
| NativeAOT | .NET 提前编译为原生 DLL，不嵌入托管运行时 |
| Composition Root | 集中创建并连接所有具体实现的入口 |
| Play Snapshot | 进入 Play 前复制 World，退出时恢复 |

## 16. 下一步

完成前两课后，建议按以下顺序继续深入：

1. 先读 `world.h` 和 `world.cpp`，彻底理解 ECS。
2. 再读 `transform_system.cpp` 和 `physics_world.cpp`，理解“数据如何被系统修改”。
3. 然后读 `render_extraction.cpp`，理解模块边界。
4. 最后读 `script_host.cpp` 和 `NativeAbi.cs`，理解跨语言边界。

如果只记一句话：

> **先跟数据走，再跟函数走；先看边界，再看实现。**
