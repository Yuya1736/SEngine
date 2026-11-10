// 引擎核心单元测试：覆盖实体/组件生命周期、场景序列化、固定步长、物理与渲染管线。
// 这些用例把引擎的规格写成可执行断言，供初学者理解各子系统的约定行为与边界条件。
// 通过 CTest（catch_discover_tests）自动注册运行；资源与 OpenGL 用例需要 assets/ 可用。
// 注意：渲染用例会创建隐藏窗口并直接读回像素，要求当前环境具备可用的 OpenGL 上下文。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glad/gl.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "l3d/assets/asset_database.h"
#include "l3d/core/event_bus.h"
#include "l3d/core/file_utils.h"
#include "l3d/core/fixed_timestep.h"
#include "l3d/core/logging.h"
#include "l3d/core/uuid.h"
#include "l3d/physics/physics_world.h"
#include "l3d/platform/window.h"
#include "l3d/renderer/render_extraction.h"
#include "l3d/renderer/renderer.h"
#include "l3d/scene/components.h"
#include "l3d/scene/editor_session.h"
#include "l3d/scene/events.h"
#include "l3d/scene/scene_serializer.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/transform_system.h"
#include "l3d/scene/world.h"

namespace {

// 返回编译期注入的资产根目录（由 CMake 定义 L3D_TEST_ASSET_ROOT），用于定位测试资产。
std::filesystem::path assetRoot() {
    return std::filesystem::path(L3D_TEST_ASSET_ROOT);
}

// 在系统临时目录生成唯一的场景文件路径：用 UUID 命名，避免并行或用例间互相覆盖。
std::filesystem::path temporaryScenePath() {
    return std::filesystem::temp_directory_path() /
           ("l3d_" + l3d::generateUuid() + ".scene.json");
}

// 从默认帧缓冲读回屏幕中心的一个像素（RGBA 四字节），用于校验渲染结果的颜色。
std::array<unsigned char, 4> readCenterPixel(int width, int height) {
    // 绑定窗口默认帧缓冲并显式设置按字节对齐，避免 1 像素读取时的行对齐问题。
    std::array<unsigned char, 4> pixel{};
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
}

// 快捷构造一个可渲染立方体实体（变换 + 内置立方体网格 + 指定材质），供渲染用例复用。
l3d::Entity createCube(l3d::World& world, const glm::vec3& position, const glm::vec3& scale,
                       const std::string& material) {
    const auto entity = world.create();
    auto& transform = world.add<l3d::Transform>(entity);
    transform.position = position;
    transform.scale = scale;
    world.add<l3d::MeshRenderer>(entity, {l3d::BuiltinCubeGuid, material, true});
    return entity;
}

} // namespace

// 验证实体句柄的“代际”机制：实体销毁后旧句柄立即失效，槽位复用不会让旧句柄“复活”。
// 这是 ECS 避免悬空引用的核心约定，index 复用 + generation 递增共同保证句柄唯一性。
TEST_CASE("Entity generations invalidate stale handles and component storage is reusable") {
    l3d::World world;
    const auto first = world.create();
    world.add<l3d::TagComponent>(first, {"Original"});
    REQUIRE(world.alive(first));
    REQUIRE(world.has<l3d::TagComponent>(first));

    world.destroy(first);
    CHECK_FALSE(world.alive(first));
    CHECK_FALSE(world.has<l3d::TagComponent>(first));

    const auto replacement = world.create();
    // 新实体复用了同一 index，但 generation 不同；旧组件不会残留到新实体上。
    CHECK(replacement.index == first.index);
    CHECK(replacement.generation != first.generation);
    CHECK(world.alive(replacement));
    CHECK_FALSE(world.has<l3d::TagComponent>(replacement));

    world.add<l3d::TagComponent>(replacement, {"Replacement"});
    CHECK(world.get<l3d::TagComponent>(replacement).value == "Replacement");
}

// 验证 World 的拷贝是“深拷贝”：副本拥有独立的实体与组件存储，改副本不影响原世界。
// 编辑器快照/播放态恢复依赖这一值语义。
TEST_CASE("World copy keeps entities and components independent") {
    l3d::World source;
    const auto entity = source.create();
    source.add<l3d::Transform>(entity).position = {1.0f, 2.0f, 3.0f};

    l3d::World copy = source;
    copy.get<l3d::Transform>(entity).position = {7.0f, 8.0f, 9.0f};
    CHECK(source.get<l3d::Transform>(entity).position.x == 1.0f);
    CHECK(copy.get<l3d::Transform>(entity).position.x == 7.0f);
    CHECK(copy.alive(entity));
}

// 验证层级系统：合法挂接成功，自环、成环以及不存在的父实体都必须被拒绝；
// 更新世界变换后，子实体的世界矩阵应叠加父级变换，这是场景图正确渲染的前提。
TEST_CASE("Hierarchy transforms propagate and cycles are rejected") {
    l3d::World world;
    const auto parent = world.create();
    auto& parentTransform = world.add<l3d::Transform>(parent);
    parentTransform.position = {10.0f, 0.0f, 0.0f};

    const auto child = world.create();
    auto& childTransform = world.add<l3d::Transform>(child);
    childTransform.position = {1.0f, 2.0f, 3.0f};

    REQUIRE(l3d::setParent(world, child, parent));
    CHECK_FALSE(l3d::setParent(world, parent, child));
    CHECK_FALSE(l3d::setParent(world, child, child));
    CHECK_FALSE(l3d::setParent(world, child, l3d::Entity{999, 0}));

    l3d::updateWorldTransforms(world);
    // 子本地位置 (1,2,3) 叠加父位置 (10,0,0)，世界矩阵平移列应为 (11,2,3)。
    const auto& matrix = world.get<l3d::Transform>(child).worldMatrix;
    CHECK(matrix[3].x == Catch::Approx(11.0f));
    CHECK(matrix[3].y == Catch::Approx(2.0f));
    CHECK(matrix[3].z == Catch::Approx(3.0f));
}

// 验证销毁实体会通过同步事件总线广播 EntityDestroyedEvent，且只广播一次。
// 脚本、编辑器等订阅方据此清理外部资源，因此通知的内容和时机属于公开契约。
TEST_CASE("World publishes entity destruction through the synchronous event bus") {
    l3d::EventBus events;
    int destroyed = 0;
    l3d::Entity observed{};
    events.subscribe<l3d::EntityDestroyedEvent>([&](const l3d::EntityDestroyedEvent& event) {
        ++destroyed;
        observed = event.entity;
    });

    l3d::World world;
    world.setEventBus(&events);
    const auto entity = world.create();
    world.destroy(entity);
    CHECK(destroyed == 1);
    CHECK(observed == entity);
    CHECK_FALSE(world.alive(entity));
}

// 验证场景 JSON 的存/读往返能完整还原组件数据、标签、父子层级和脚本属性。
// 这是编辑器“保存/打开场景”的基础，任一字段丢失都会造成用户数据损坏。
TEST_CASE("Scene JSON round trip restores components, tags and hierarchy") {
    l3d::World world;
    const auto parent = world.create();
    world.add<l3d::NameComponent>(parent, {"Root"});
    world.add<l3d::TagComponent>(parent, {"RootTag"});
    world.add<l3d::Transform>(parent).position = {4.0f, 0.0f, 1.0f};

    const auto child = world.create();
    world.add<l3d::NameComponent>(child, {"Child"});
    world.add<l3d::TagComponent>(child, {"ChildTag"});
    auto& childTransform = world.add<l3d::Transform>(child);
    childTransform.position = {-1.0f, 2.5f, 0.25f};
    childTransform.scale = {2.0f, 3.0f, 4.0f};
    childTransform.rotation = glm::quat(glm::radians(glm::vec3(10.0f, 20.0f, 30.0f)));
    world.add<l3d::MeshRenderer>(child, {l3d::BuiltinCubeGuid, "material-guid", true});
    world.add<l3d::BoxCollider>(child, {{1.0f, 2.0f, 3.0f}, true});
    world.add<l3d::ScriptComponent>(child, {"PlayerController", "{\"moveSpeed\":7.0}"});
    REQUIRE(l3d::setParent(world, child, parent));

    const auto path = temporaryScenePath();
    const auto saved = l3d::SceneSerializer::save(world, path);
    REQUIRE(saved.ok());
    auto loaded = l3d::SceneSerializer::load(path);
    REQUIRE(loaded.ok());
    CHECK(loaded.value()->entities().size() == 2);

    // 通过 Tag 找回子实体，避免依赖读档后实体的排列顺序。
    const auto loadedChild = l3d::findEntityByTag(*loaded.value(), "ChildTag");
    REQUIRE(loadedChild.valid());
    const auto* loadedTransform = loaded.value()->tryGet<l3d::Transform>(loadedChild);
    REQUIRE(loadedTransform != nullptr);
    CHECK(loadedTransform->position.x == Catch::Approx(-1.0f));
    CHECK(loadedTransform->position.y == Catch::Approx(2.5f));
    CHECK(loadedTransform->scale.z == Catch::Approx(4.0f));
    // 脚本属性以原始 JSON 字符串保存，引擎不解析内容，只需原样保留。
    const auto* loadedScript = loaded.value()->tryGet<l3d::ScriptComponent>(loadedChild);
    REQUIRE(loadedScript != nullptr);
    CHECK(loadedScript->scriptName == "PlayerController");
    CHECK(loadedScript->propertiesJson.find("7.0") != std::string::npos);

    // 父实体通过显示名（NameComponent）核对，说明父子链接在反序列化后仍然有效。
    const auto* hierarchy = loaded.value()->tryGet<l3d::Hierarchy>(loadedChild);
    REQUIRE(hierarchy != nullptr);
    CHECK(l3d::displayName(*loaded.value(), hierarchy->parent) == "Root");

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK_FALSE(error);
}

// 验证读档失败时返回错误且不破坏调用方的现有世界：加载必须是“全有或全无”。
// 否则一次损坏的文件就可能让编辑器丢失当前未保存的工作。
TEST_CASE("Invalid scene data returns an error without replacing the current world") {
    auto current = std::make_unique<l3d::World>();
    const auto entity = current->create();
    current->add<l3d::TagComponent>(entity, {"StillAlive"});

    const auto path = temporaryScenePath();
    // version 999 模拟未来/未知格式，解析器应明确拒绝而不是猜测字段。
    REQUIRE(l3d::writeTextFile(path, "{\"version\":999,\"entities\":[]}"));
    auto loaded = l3d::SceneSerializer::load(path);
    CHECK_FALSE(loaded.ok());
    CHECK(current->alive(entity));
    CHECK(l3d::findEntityByTag(*current, "StillAlive") == entity);

    std::error_code error;
    std::filesystem::remove(path, error);
}

// 验证固定步长累加器：按 60Hz 累积得到正确步数，单帧过长时被钳制（防止“死亡螺旋”），
// 负数与 NaN 增量被安全忽略，reset 能清空累加器。
TEST_CASE("Fixed timestep accumulates, clamps and rejects invalid deltas") {
    l3d::FixedTimestep timestep(1.0 / 60.0, 5, 0.25);
    auto result = timestep.advance(1.0 / 60.0 * 3.0 + 1.0 / 600.0);
    CHECK(result.steps == 3);
    CHECK_FALSE(result.clamped);

    // 一次 1 秒的卡顿只允许跑满上限 5 步，多余时间被丢弃，避免追帧越追越慢。
    result = timestep.advance(1.0);
    CHECK(result.steps == 5);
    CHECK(result.clamped);
    CHECK(timestep.accumulator() == Catch::Approx(0.0).margin(1e-12));

    result = timestep.advance(-1.0);
    CHECK(result.steps == 0);
    result = timestep.advance(std::numeric_limits<double>::quiet_NaN());
    CHECK(result.steps == 0);
    timestep.reset();
    CHECK(timestep.accumulator() == 0.0);
}

// 验证角色控制器在重力下落到地面并置 grounded，同时触发器进入/离开各只报告一次。
// 事件去重很关键：脚本若每帧都收到 Enter，就会重复开门或重复拾取。
TEST_CASE("Character controller settles on a collider and trigger enter/exit fires once") {
    l3d::World world;
    const auto ground = world.create();
    world.add<l3d::Transform>(ground).position = {0.0f, -0.05f, 0.0f};
    world.add<l3d::BoxCollider>(ground, {{10.0f, 0.05f, 10.0f}, false});

    const auto player = world.create();
    world.add<l3d::Transform>(player).position = {0.0f, 2.0f, 0.0f};
    world.add<l3d::CharacterController>(player);

    const auto trigger = world.create();
    auto& triggerTransform = world.add<l3d::Transform>(trigger);
    triggerTransform.position = {0.0f, 0.95f, 0.8f};
    world.add<l3d::BoxCollider>(trigger, {{0.5f, 0.9f, 0.5f}, true});

    l3d::SimplePhysicsWorld physics;
    std::vector<l3d::CollisionEvent> events;
    // 连续模拟 120 帧（约 2 秒），让角色完成下落并稳定站在地面上。
    for (int step = 0; step < 120; ++step) {
        events.clear();
        physics.step(world, 1.0f / 60.0f, events);
    }
    const auto* controller = world.tryGet<l3d::CharacterController>(player);
    REQUIRE(controller != nullptr);
    CHECK(controller->grounded);
    CHECK(world.get<l3d::Transform>(player).position.y ==
          Catch::Approx(0.9f).margin(0.03f));

    // 角色仍站在触发区内，重复检测不应再产生 Enter 事件。
    events.clear();
    physics.step(world, 1.0f / 60.0f, events);
    CHECK(std::ranges::count_if(events, [](const l3d::CollisionEvent& event) {
              return event.phase == l3d::TriggerPhase::Enter;
          }) == 0);

    // 把触发器移远，本帧应恰好产生一次 Exit。
    triggerTransform.position.z = 10.0f;
    events.clear();
    physics.step(world, 1.0f / 60.0f, events);
    CHECK(std::ranges::count_if(events, [](const l3d::CollisionEvent& event) {
              return event.phase == l3d::TriggerPhase::Exit;
          }) == 1);
}

// 验证射线检测返回最近的命中实体与正确距离，未命中时返回空值。
// 射线拾取、地面检测、射击判定等玩法都建立在这一能力上。
TEST_CASE("Raycasts find the nearest box and sphere hit") {
    l3d::World world;
    const auto box = world.create();
    world.add<l3d::Transform>(box).position = {0.0f, 0.0f, -5.0f};
    world.add<l3d::BoxCollider>(box, {{0.5f, 0.5f, 0.5f}, false});

    const auto sphere = world.create();
    world.add<l3d::Transform>(sphere).position = {3.0f, 0.0f, 0.0f};
    world.add<l3d::SphereCollider>(sphere, {1.0f, false});

    l3d::SimplePhysicsWorld physics;
    // 射线从原点沿 -Z 射出，命中 z=-5 处半边长 0.5 的盒子前表面，距离 4.5。
    const auto boxHit = physics.raycast(world, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, 100.0f);
    REQUIRE(boxHit.has_value());
    CHECK(boxHit->entity == box);
    CHECK(boxHit->distance == Catch::Approx(4.5f));

    // 球心 (3,0,0)、半径 1，沿 +X 命中球面，距离 2.0。
    const auto sphereHit = physics.raycast(world, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 100.0f);
    REQUIRE(sphereHit.has_value());
    CHECK(sphereHit->entity == sphere);
    CHECK(sphereHit->distance == Catch::Approx(2.0f));
    CHECK_FALSE(physics.raycast(world, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 100.0f)
                    .has_value());
}

// 验证实体碰撞（非触发器）的进入/离开事件同样“每次状态切换只发一次”，
// 避免接触中的对象每帧刷出重复的 Enter，让碰撞音效/伤害等逻辑可安全订阅。
TEST_CASE("Solid collider enter and exit events are emitted once per transition") {
    l3d::World world;
    const auto first = world.create();
    world.add<l3d::Transform>(first).position = {0.0f, 0.0f, 0.0f};
    world.add<l3d::BoxCollider>(first, {{1.0f, 1.0f, 1.0f}, false});

    const auto second = world.create();
    auto& secondTransform = world.add<l3d::Transform>(second);
    secondTransform.position = {1.5f, 0.0f, 0.0f};
    world.add<l3d::BoxCollider>(second, {{1.0f, 1.0f, 1.0f}, false});

    l3d::SimplePhysicsWorld physics;
    std::vector<l3d::CollisionEvent> events;
    // 两盒子中心距 1.5，小于两者半长之和 2.0，首帧即重叠，应报告恰好一次 Enter。
    physics.step(world, 1.0f / 60.0f, events);
    CHECK(std::ranges::count_if(events, [](const l3d::CollisionEvent& event) {
              return !event.trigger && event.phase == l3d::TriggerPhase::Enter;
          }) == 1);

    events.clear();
    physics.step(world, 1.0f / 60.0f, events);
    CHECK(std::ranges::count_if(events, [](const l3d::CollisionEvent& event) {
              return !event.trigger && event.phase == l3d::TriggerPhase::Enter;
          }) == 0);

    secondTransform.position.x = 10.0f;
    events.clear();
    physics.step(world, 1.0f / 60.0f, events);
    CHECK(std::ranges::count_if(events, [](const l3d::CollisionEvent& event) {
              return !event.trigger && event.phase == l3d::TriggerPhase::Exit;
          }) == 1);
}

// 验证编辑器的“播放/停止”语义：进入播放时对原场景做快照，停止后回到编辑态。
// 播放期间的位置修改和新增组件都属于临时状态，必须被丢弃，否则会污染用户存档。
TEST_CASE("Play session snapshot restores the authored scene") {
    l3d::World live;
    const auto entity = live.create();
    live.add<l3d::NameComponent>(entity, {"Authored"});
    live.add<l3d::Transform>(entity).position = {1.0f, 2.0f, 3.0f};

    l3d::EditorSession session;
    REQUIRE(session.beginPlay(live));
    CHECK(session.playing());
    live.get<l3d::Transform>(entity).position = {99.0f, 99.0f, 99.0f};
    live.add<l3d::TagComponent>(entity, {"Temporary"});

    auto restored = session.endPlay();
    REQUIRE(restored != nullptr);
    CHECK_FALSE(session.playing());
    REQUIRE(restored->alive(entity));
    CHECK(restored->get<l3d::Transform>(entity).position.x == 1.0f);
    CHECK_FALSE(restored->has<l3d::TagComponent>(entity));
}

// 验证资产库能扫描资产目录，并按 GUID 加载 OBJ 网格、PNG 贴图和材质元数据；
// 未知 GUID 必须返回错误而不是空指针或崩溃，这是资源热重载与缺失资源处理的基础。
TEST_CASE("Asset database imports OBJ, PNG and material metadata") {
    l3d::AssetDatabase assets;
    const auto scanned = assets.scan(assetRoot());
    REQUIRE(scanned.ok());
    CHECK(scanned.value() >= 12);

    const auto mesh = assets.loadMesh("0b6c8a22-61f4-4f4e-9e5b-100000000002");
    // 立方体按面拆分为 4 顶点 × 6 面 = 24 顶点、2 三角形 × 6 面 = 36 索引（便于独立法线）。
    REQUIRE(mesh.ok());
    CHECK(mesh.value()->vertices.size() == 24);
    CHECK(mesh.value()->indices.size() == 36);
    CHECK(mesh.value()->boundsMin.x == Catch::Approx(-0.5f));
    CHECK(mesh.value()->boundsMax.y == Catch::Approx(0.5f));

    const auto texture = assets.loadTexture("0b6c8a22-61f4-4f4e-9e5b-100000000001");
    REQUIRE(texture.ok());
    CHECK(texture.value()->width == 128);
    CHECK(texture.value()->height == 128);
    CHECK(texture.value()->pixels.size() == 128u * 128u * 4u);

    const auto material = assets.loadMaterial("0b6c8a22-61f4-4f4e-9e5b-100000000014");
    REQUIRE(material.ok());
    CHECK(material.value().name == "Wood Crate");
    CHECK(material.value().albedoTexGuid == "0b6c8a22-61f4-4f4e-9e5b-100000000001");
    CHECK(material.value().baseColor.a == Catch::Approx(1.0f));

    CHECK_FALSE(assets.loadMesh("missing-guid").ok());
    CHECK(assets.findByGuid("missing-guid") == nullptr);
}

// 验证内置性能场景可被正常加载，实体数、渲染组件数符合预期，作为性能与回归基线。
TEST_CASE("Performance acceptance scene loads the expected entity and render counts") {
    const auto path = assetRoot() / "scenes" / "performance.scene.json";
    auto scene = l3d::SceneSerializer::load(path);
    REQUIRE(scene.ok());
    CHECK(scene.value()->entities().size() == 500);
    CHECK(scene.value()->componentPool<l3d::MeshRenderer>().size() == 101);
    l3d::updateWorldTransforms(*scene.value());
    CHECK(l3d::findEntityByTag(*scene.value(), "MainCamera").valid());
}

// 隐藏窗口的 OpenGL 冒烟测试：真实走一遍提取-渲染-呈现流程，并读回屏幕中心像素，
// 既验证材质颜色输出，也验证深度测试让较近的立方体遮挡较远的立方体。
// 窗口设为不可见，便于在 CI/无头环境运行；读回前调用 glFinish 确保命令已执行完毕。
TEST_CASE("Hidden-window OpenGL smoke test verifies color output and depth ordering") {
    l3d::Logger::instance().setConsoleEnabled(false);
    l3d::AssetDatabase assets;
    REQUIRE(assets.scan(assetRoot()).ok());

    l3d::Window window;
    l3d::WindowConfig windowConfig;
    windowConfig.width = 320;
    windowConfig.height = 240;
    windowConfig.visible = false;
    windowConfig.vsync = false;
    windowConfig.title = "L3D render test";
    REQUIRE(window.initialize(windowConfig));

    l3d::OpenGLRenderer renderer;
    REQUIRE(renderer.initialize(assets));

    l3d::World world;
    const auto sun = world.create();
    world.add<l3d::Transform>(sun);
    world.add<l3d::DirectionalLight>(sun);

    const auto front = createCube(world, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f},
                                  "0b6c8a22-61f4-4f4e-9e5b-100000000011");
    const auto back = createCube(world, {0.0f, 0.0f, -2.5f}, {2.0f, 2.0f, 2.0f},
                                 "0b6c8a22-61f4-4f4e-9e5b-100000000012");

    // 相机位于 z=5 看向原点：前立方体在 z=0，后立方体在 z=-2.5，二者中心重合于视线。
    auto renderAndRead = [&]() {
        const auto frame = l3d::extractRenderFrame(world, {0.0f, 0.0f, 5.0f},
                                                   glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 45.0f,
                                                   0.1f, 100.0f, 320.0f / 240.0f);
        renderer.render(frame, 320, 240);
        renderer.presentToDefaultFramebuffer(320, 240);
        glFinish();
        return readCenterPixel(320, 240);
    };

    // 两次只渲染一个立方体用于取“基准色”：前立方体为蓝色材质，后立方体为青铜色（偏红），
    // 二者中心像素用通道差值区分；最后两个一起渲染，用来检查深度遮挡结果。
    world.get<l3d::MeshRenderer>(back).visible = false;
    const auto frontOnly = renderAndRead();
    world.get<l3d::MeshRenderer>(back).visible = true;
    world.get<l3d::MeshRenderer>(front).visible = false;
    const auto backOnly = renderAndRead();
    world.get<l3d::MeshRenderer>(front).visible = true;
    const auto both = renderAndRead();

    // 两个都可见时中心像素应与“仅前立方体”接近（容差 4），说明近处的蓝色立方体胜出。
    CHECK(frontOnly[2] > frontOnly[0] + 10);
    CHECK(backOnly[0] > backOnly[2] + 10);
    CHECK(std::abs(static_cast<int>(frontOnly[0]) - static_cast<int>(both[0])) <= 4);
    CHECK(std::abs(static_cast<int>(frontOnly[1]) - static_cast<int>(both[1])) <= 4);
    CHECK(std::abs(static_cast<int>(frontOnly[2]) - static_cast<int>(both[2])) <= 4);
    CHECK(glGetError() == GL_NO_ERROR);

    renderer.shutdown();
    window.shutdown();
    l3d::Logger::instance().setConsoleEnabled(true);
}
