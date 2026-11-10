# 演示脚本与答辩材料

## 1. 演示目标

在 5–10 分钟内证明三件事：

| 目标 | 证据 |
|---|---|
| 引擎能完成完整游戏闭环 | 编辑、保存、播放、移动、碰撞、拾取、开门、停止恢复 |
| 架构边界真实存在 | 模块依赖图、RenderFrame、IPhysicsWorld、C ABI、异常隔离 |
| 质量可以被验证 | 16 个自动测试、隐藏窗口渲染测试、500 实体性能数据 |

## 2. 演示前检查

| 检查项 | 命令/动作 | 通过标准 |
|---|---|---|
| Debug 构建 | `cmake --build build --config Debug --parallel 6` | 无错误 |
| 自动测试 | `ctest --test-dir build -C Debug --output-on-failure` | 16/16 |
| C# 插件 | 检查 `build/bin/Debug/GameScripts.dll` 存在 | 文件存在 |
| 示例资产 | 确认 `assets/scenes/sample.scene.json` 存在 | 场景可加载 |
| 显示环境 | 确认 OpenGL 4.1 可用 | 窗口正常打开 |
| 备份 | 复制一份 `sample.scene.json` | 演示后可以恢复 |

建议提前运行一次：

```powershell
build\bin\Debug\engine_app.exe --scene assets\scenes\sample.scene.json
```

## 3. 时间分配

| 时间 | 内容 | 目的 |
|---:|---|---|
| 0:00–0:40 | 项目定位与架构摘要 | 先说明这是“以架构质量为重点的轻量引擎” |
| 0:40–1:40 | 编辑器与场景结构 | 展示 Hierarchy、Inspector、Assets、Viewport |
| 1:40–2:30 | 编辑与保存/加载 | 证明场景数据可持久化 |
| 2:30–4:30 | Play 闭环 | 移动、碰撞、拾取、触发开门 |
| 4:30–5:10 | Stop 恢复 | 证明运行时不污染作者场景 |
| 5:10–6:20 | C# ABI 与错误隔离 | 展示脚本目录、ABI 校验、异常测试 |
| 6:20–7:30 | 自动测试与性能 | 16/16、隐藏窗口渲染、500 实体数据 |
| 7:30–10:00 | 架构问答 | 讲解取舍、替代方案和后续演进 |

若只有 5 分钟，合并“编辑保存”和“测试详情”，保留 Play 闭环、Stop 恢复和 ABI/错误隔离。

## 4. 演示步骤

### 4.1 开场（40 秒）

讲解词要点：

| 要点 | 表述 |
|---|---|
| 定位 | “这是一个课程设计范围的轻量 3D 引擎，不追求商业引擎完整度” |
| 重点 | “重点是模块边界、单向依赖、错误隔离、可替换性和可验证测试” |
| 技术栈 | “C++20 + OpenGL 4.1 + C# NativeAOT，全部主线程，固定 60 Hz” |
| 示例 | “示例包含第三人称移动、碰撞、拾取和触发开门” |

可展示 `README.md` 的架构摘要表，不展开全部代码。

### 4.2 编辑器与场景结构（1 分钟）

操作：

1. 启动编辑器，展示默认 `sample.scene.json`。
2. 在 Hierarchy 中展开 `Gate Frame`、`Crate Stack`。
3. 点击 `Player`，指出 Transform、Mesh Renderer、Character Controller、Script、Tag。
4. 点击 `Main Camera`，指出 `ThirdPersonCamera` 脚本和属性 JSON。

讲解要点：

| 观察点 | 架构含义 |
|---|---|
| Hierarchy 显示父子关系 | 层级组件与变换传播 |
| Inspector 显示组件 | 数据驱动场景，不把编辑逻辑写入组件 |
| Script 下拉和属性 JSON | 运行时通过稳定名称绑定 C# 类型 |
| Assets 面板 | GUID 资产数据库与手动 Refresh |

### 4.3 编辑、保存与加载（50 秒）

操作：

1. 选中 `Player`，把位置 Z 改大约 1 个单位，或修改名字/标签。
2. 点击 `Save`。
3. 点击 `Load`，确认修改仍在。
4. 若修改了演示场景，演示前已备份，结束后恢复。

讲解要点：

| 机制 | 说明 |
|---|---|
| 场景格式 | `.scene.json`，版本化，实体 id 用于父子引用 |
| 失败语义 | 非法场景不会替换当前 World |
| 资产引用 | Mesh/Material 使用 GUID 或 `builtin:*` |

### 4.4 Play 闭环（2 分钟）

操作顺序建议：

1. 点击 `Play`，标题变为 `L3D Engine Editor | PLAY`。
2. 按 `D` 向右，再按 `W` 向前，展示玩家移动与相机平滑跟随。
3. 靠近右侧黄色能量球，触发 `Pickup`，观察拾取日志与实体消失。
4. 走向门传感器，观察棕色门体上升。
5. 按 `Space` 展示跳跃和重力。
6. 点击 `Stop`，观察玩家、门、拾取物恢复。

讲解要点：

| 现象 | 对应系统 |
|---|---|
| 角色移动 | `PlayerController` 通过 ABI 写 CharacterController 速度 |
| 相机跟随 | `ThirdPersonCamera` 查询 Player Tag 和 Transform |
| 拾取 | 球形触发器 + `Pickup` 脚本销毁实体 |
| 开门 | 触发器事件 + `DoorTrigger` 脚本移动门体 |
| Stop 恢复 | `EditorSession` 的 World 深拷贝快照 |

如果玩家不容易走到目标点，可重新开始 Play，按住 `D + W` 约 2 秒，再朝门方向调整；也可以先用编辑器相机观察触发器布局。

### 4.5 C# ABI 与错误隔离（1 分钟）

操作：

1. 展示 `GameScripts.dll` 位于可执行文件旁。
2. 展示日志中的 `Loaded C# script plugin with 7 script types` 和 `GameScripts initialized with EngineApiV1`。
3. 说明脚本目录包含 `PlayerController`、`ThirdPersonCamera`、`DoorTrigger`、`Pickup`、`ThrowingScript`、`CollisionProbe`、`AbiProbe`。
4. 如果时间允许，运行自动测试并指出 `NativeAOT gameplay ABI drives movement, triggers and exception isolation`。

讲解要点：

| 边界 | 说明 |
|---|---|
| 字符串 | UTF-8 指针 + 长度 |
| 句柄 | 64 位不透明实体/脚本句柄 |
| 函数表 | `EngineApiV1`，含 Transform、输入、实体、射线、日志 |
| 异常 | C# 导出函数捕获，C ABI 只返回状态码 |
| 版本 | ABI 版本和结构大小双重校验 |
| 失败策略 | 插件缺失/版本不匹配时禁用脚本，场景仍可加载 |

### 4.6 测试与性能（1 分钟）

操作：

1. 在终端执行 `ctest --test-dir build -C Debug --output-on-failure`。
2. 展示 16/16 通过。
3. 展示性能命令和结果表，不必现场跑 1000 帧；如现场运行，使用 Release 配置。

可讲数据：

| 指标 | 结果 |
|---|---|
| 自动测试 | Debug/Release 均 16/16 |
| 性能场景 | Release 约 1482.7 FPS；Debug 约 284.2 FPS |
| 场景规模 | 500 实体、101 个渲染项、1280×720 |
| 渲染冒烟 | 隐藏窗口读回非清屏颜色，深度顺序正确 |

### 4.7 收尾（30 秒）

讲解词要点：

| 主题 | 表述 |
|---|---|
| 架构收益 | “模块边界可以被编译期依赖和自动测试共同验证” |
| 取舍 | “没有做 PBR、复杂物理和热重载，是为了在课程周期内保证闭环和可验证性” |
| 演进 | “渲染、物理、资产、脚本宿主均有接口替换点；下一步可做 Shadow Map、撤销重做和组件 ABI” |

## 5. 答辩问答

| 问题 | 建议回答 |
|---|---|
| 为什么不用 EnTT/Bullet/Assimp？ | 课程重点是自研边界和可解释性；第三方库会掩盖关键设计，也会扩大集成面。自研范围被限制在示例闭环需要的子集。 |
| 为什么不用 Archetype ECS？ | 课程规模下稀疏集合的 O(1) 访问已足够，代码和快照恢复更易解释。Archetype 的查询缓存和批量系统留作演进。 |
| 为什么 C# 用 NativeAOT 而不是嵌入 CoreCLR？ | NativeAOT 产物是普通 DLL，加载简单，无托管运行时嵌入和程序集卸载复杂度；C ABI 更小、更稳定。 |
| 为什么 C ABI 不传 STL/glm？ | 跨编译器/语言 ABI 不稳定；POD 结构和指针+长度可以明确控制布局与所有权。 |
| 如何保证 C# 异常不破坏引擎？ | 每个 C# 导出函数都有 try/catch，失败时记录日志并禁用实例；C++ 侧只看到状态码/无返回。 |
| Play 模式为什么复制整个 World？ | 保证任意组件和实体修改都能完整恢复；代价是大场景内存/复制成本，当前规模可接受。 |
| 渲染如何与 ECS 解耦？ | 场景层提取 `RenderFrame`，渲染器只消费帧描述，不访问 ECS 或 OpenGL 之外的场景对象。 |
| 为什么固定步长 60 Hz？ | 物理和脚本行为确定、可测试；渲染帧率可变，避免物理随帧率变化。 |
| 500 实体性能为什么这么高？ | 场景渲染项只有 101 个，且是简单内置几何/单方向光；该数据用于验收退化，不是复杂场景基准。 |
| 目前最大的限制是什么？ | 没有阴影、复杂物理、热重载和完整编辑器命令栈；这些是明确的后续演进项，不影响核心闭环。 |
| 后续如何替换物理？ | 实现 `IPhysicsWorld`，在组合根替换 `SimplePhysicsWorld`；脚本射线和事件接口不变。 |
| 为什么没有多线程？ | 课程周期内优先确定性和可调试性；线程模型已明确，Job System 不是当前范围。 |

## 6. 演示失败预案

| 故障 | 处理 |
|---|---|
| 窗口无法创建 | 检查 GPU 驱动是否支持 OpenGL 4.1；使用 `--hidden` 运行测试确认引擎逻辑 |
| C# 插件未加载 | 检查 `GameScripts.dll` 是否与 `engine_app.exe` 同目录；重新构建 |
| 场景加载失败 | 使用仓库内 `assets/scenes/sample.scene.json` 绝对路径启动 |
| 玩家错过目标 | 重新点击 Play，从起点沿右前方 `D + W` 移动；门和拾取物都在起点前方 |
| 现场网络不可用 | 使用已配置的 `build/` 目录，不重新 FetchContent |
| 性能数据被质疑 | 说明是开发机 3 次运行取中位数，强调 60 FPS 验收门槛和场景规模，不宣称跨机器基准 |

## 7. 演示截图

| 视图 | 文件 |
|---|---|
| 编辑器 | `docs/images/sample-editor.png` |
| Runtime | `docs/images/sample-runtime.png` |
| 性能场景 | `docs/images/performance.png` |
