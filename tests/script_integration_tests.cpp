// 脚本系统集成测试：加载真实的 C# NativeAOT 插件（GameScripts.dll）以及一个假插件，
// 验证脚本宿主的注册、目录枚举、实例生命周期、事件分发、托管异常隔离与 ABI 回调。
// 用例按“脚本更新 → 物理步进 → 碰撞事件分发”的顺序模拟一帧游戏循环。
// 由 CTest 运行，工作目录设在可执行输出目录，才能按文件名加载插件动态库。
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "l3d/core/file_utils.h"
#include "l3d/core/input.h"
#include "l3d/core/logging.h"
#include "l3d/physics/physics_world.h"
#include "l3d/scene/components.h"
#include "l3d/scene/scene_serializer.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/world.h"
#include "l3d/scripting/script_host.h"

namespace {

// 插件按约定放在可执行文件所在目录，这里据此拼出动态库完整路径。
std::filesystem::path pluginPath(const char* name) {
    return l3d::executableDirectory() / name;
}

// 创建地面：半长 10、半高 0.05 的实体碰撞体，顶面恰好位于 y=0。
l3d::Entity createGround(l3d::World& world) {
    const auto ground = world.create();
    world.add<l3d::Transform>(ground).position = {0.0f, -0.05f, 0.0f};
    world.add<l3d::BoxCollider>(ground, {{10.0f, 0.05f, 10.0f}, false});
    return ground;
}

// 创建玩家：带角色控制器并被标记为 "Player"，与示例场景和脚本的约定一致。
l3d::Entity createPlayer(l3d::World& world, const glm::vec3& position) {
    const auto player = world.create();
    world.add<l3d::Transform>(player).position = position;
    world.add<l3d::CharacterController>(player);
    world.add<l3d::TagComponent>(player, {"Player"});
    return player;
}

} // namespace

// 验证插件加载的失败路径：文件不存在、ABI 版本不匹配时 load 都返回 false 且保持未加载。
// 宿主不能因坏插件崩溃或停在半初始化状态，否则用户装错一次插件就无法打开编辑器。
TEST_CASE("Script host reports missing and incompatible plugins without crashing") {
    l3d::Logger::instance().setConsoleEnabled(false);

    // 不存在的动态库路径：系统加载库失败，宿主应干净地返回 false。
    l3d::ScriptHost missing;
    CHECK_FALSE(missing.load(pluginPath("this_plugin_does_not_exist.dll")));
    CHECK_FALSE(missing.loaded());

    // 假插件导出了全部符号但版本号是 2，加载应停在版本比较处并被拒绝。
    l3d::ScriptHost mismatch;
    CHECK_FALSE(mismatch.load(pluginPath("fake_script_plugin.dll")));
    CHECK_FALSE(mismatch.loaded());

    l3d::Logger::instance().setConsoleEnabled(true);
}

// 用真实的 NativeAOT 游戏脚本插件端到端验证脚本 ABI：目录枚举、实例创建与销毁、
// Update 驱动移动、碰撞/触发事件转发、托管异常隔离，以及引擎回调（生成实体、射线、输入）。
TEST_CASE("NativeAOT gameplay ABI drives movement, triggers and exception isolation") {
    l3d::Logger::instance().setConsoleEnabled(false);
    l3d::ScriptHost host;
    REQUIRE(host.load(pluginPath("GameScripts.dll")));
    // 插件必须公布全部内置脚本名；少一个都会让场景里的对应脚本静默失效。
    const auto& catalog = host.catalog();
    CHECK(std::ranges::find(catalog, "PlayerController") != catalog.end());
    CHECK(std::ranges::find(catalog, "ThirdPersonCamera") != catalog.end());
    CHECK(std::ranges::find(catalog, "DoorTrigger") != catalog.end());
    CHECK(std::ranges::find(catalog, "Pickup") != catalog.end());
    CHECK(std::ranges::find(catalog, "ThrowingScript") != catalog.end());
    CHECK(std::ranges::find(catalog, "CollisionProbe") != catalog.end());
    CHECK(std::ranges::find(catalog, "AbiProbe") != catalog.end());

    l3d::InputSnapshot input;
    l3d::SimplePhysicsWorld physics;

    {
        // 未知脚本名：宿主应跳过该组件，不创建实例，也不影响后续使用。
        l3d::World world;
        const auto entity = world.create();
        world.add<l3d::Transform>(entity);
        world.add<l3d::ScriptComponent>(entity, {"UnknownScript", "{}"});
        REQUIRE(host.start(world, physics, input));
        CHECK(host.instanceCount() == 0);
        host.stop();
    }

    {
        // 手动分发一次 Enter 碰撞事件，用日志确认脚本确实收到了回调及事件类型。
        l3d::World world;
        const auto probe = world.create();
        world.add<l3d::Transform>(probe);
        world.add<l3d::ScriptComponent>(probe, {"CollisionProbe", "{}"});
        const auto other = world.create();
        world.add<l3d::Transform>(other);
        l3d::Logger::instance().clear();
        REQUIRE(host.start(world, physics, input));
        const std::vector<l3d::CollisionEvent> events{
            {probe, other, l3d::TriggerPhase::Enter, false}};
        host.dispatchEvents(world, events, input);
        const auto entries = l3d::Logger::instance().snapshot();
        CHECK(std::ranges::any_of(entries, [](const l3d::LogEntry& entry) {
            return entry.message.find("CollisionProbe: enter") != std::string::npos;
        }));
        host.stop();
    }

    {
        // 托管异常必须被宿主隔离：脚本实例失败后实体与世界仍然可用，异常信息进入日志。
        l3d::World world;
        const auto entity = world.create();
        world.add<l3d::Transform>(entity);
        world.add<l3d::ScriptComponent>(entity, {"ThrowingScript", "{}"});
        l3d::Logger::instance().clear();
        REQUIRE(host.start(world, physics, input));
        REQUIRE(host.instanceCount() == 1);
        // 异常发生在脚本的 Update 内部，宿主捕获后继续运行，本帧不应影响实体存活。
        host.update(world, input, 1.0f / 60.0f);
        CHECK(world.alive(entity));
        const auto entries = l3d::Logger::instance().snapshot();
        CHECK(std::ranges::any_of(entries, [](const l3d::LogEntry& entry) {
            return entry.message.find("Intentional managed exception") != std::string::npos;
        }));
        host.stop();
    }

    {
        l3d::World world;
        createGround(world);
        const auto player = createPlayer(world, {0.0f, 0.95f, 0.0f});
        world.add<l3d::ScriptComponent>(
            player, {"PlayerController", "{\"moveSpeed\":6.0,\"jumpSpeed\":7.5}"});

        // 按住 W 一秒（60 帧），PlayerController 应把玩家沿 -Z 推动，并保持贴地。
        input.setKey(l3d::Key::W, true);
        REQUIRE(host.start(world, physics, input));
        // 每帧先跑脚本、再跑物理、最后分发碰撞事件，这个顺序与引擎主循环一致。
        for (int step = 0; step < 60; ++step) {
            host.update(world, input, 1.0f / 60.0f);
            std::vector<l3d::CollisionEvent> events;
            physics.step(world, 1.0f / 60.0f, events);
            host.dispatchEvents(world, events, input);
        }
        input.setKey(l3d::Key::W, false);
        const auto* transform = world.tryGet<l3d::Transform>(player);
        REQUIRE(transform != nullptr);
        CHECK(transform->position.z < -2.0f);
        const auto* controller = world.tryGet<l3d::CharacterController>(player);
        REQUIRE(controller != nullptr);
        CHECK(controller->grounded);
        host.stop();
    }

    {
        // 进入触发器后持续更新：DoorTrigger 按标签找到门，并按 speed 把它抬升。
        l3d::World world;
        const auto player = createPlayer(world, {0.0f, 1.0f, 0.0f});

        const auto door = world.create();
        world.add<l3d::Transform>(door).position = {0.0f, 1.5f, 0.0f};
        world.add<l3d::TagComponent>(door, {"Door"});

        const auto sensor = world.create();
        world.add<l3d::Transform>(sensor);
        world.add<l3d::BoxCollider>(sensor, {{1.0f, 1.0f, 1.0f}, true});
        world.add<l3d::ScriptComponent>(
            sensor, {"DoorTrigger", "{\"doorTag\":\"Door\",\"openHeight\":3.5,\"speed\":2.8}"});

        REQUIRE(host.start(world, physics, input));
        const std::vector<l3d::CollisionEvent> events{
            {player, sensor, l3d::TriggerPhase::Enter, true}};
        host.dispatchEvents(world, events, input);
        // 20 帧 × 0.1 秒，足够门从 1.5 升到 3.5 的 openHeight 附近。
        for (int step = 0; step < 20; ++step) {
            host.update(world, input, 0.1f);
        }
        CHECK(world.get<l3d::Transform>(door).position.y > 2.5f);
        host.stop();
    }

    {
        // 拾取脚本收到 Enter 后销毁自身实体，并输出带道具标签的收集日志。
        l3d::World world;
        const auto player = createPlayer(world, {0.0f, 1.0f, 0.0f});
        const auto pickup = world.create();
        world.add<l3d::Transform>(pickup);
        world.add<l3d::SphereCollider>(pickup, {0.75f, true});
        world.add<l3d::ScriptComponent>(pickup, {"Pickup", "{\"label\":\"Test Cell\"}"});
        l3d::Logger::instance().clear();

        REQUIRE(host.start(world, physics, input));
        const std::vector<l3d::CollisionEvent> events{
            {player, pickup, l3d::TriggerPhase::Enter, true}};
        host.dispatchEvents(world, events, input);
        CHECK_FALSE(world.alive(pickup));
        const auto entries = l3d::Logger::instance().snapshot();
        CHECK(std::ranges::any_of(entries, [](const l3d::LogEntry& entry) {
            return entry.message.find("Collected Test Cell") != std::string::npos;
        }));
        host.stop();
    }

    {
        // AbiProbe 通过引擎回调完成“生成实体 + 射线检测 + 读取输入”，用来校验跨语言传参。
        l3d::World world;
        const auto probe = world.create();
        world.add<l3d::Transform>(probe).position = {0.0f, 0.0f, 0.0f};
        world.add<l3d::BoxCollider>(probe, {{0.5f, 0.5f, 0.5f}, false});
        world.add<l3d::ScriptComponent>(
            probe, {"AbiProbe", "{\"spawnTag\":\"SpawnedByAbi\",\"rayDistance\":10.0}"});

        const auto target = world.create();
        world.add<l3d::Transform>(target).position = {0.0f, -1.0f, 0.0f};
        world.add<l3d::BoxCollider>(target, {{1.0f, 0.5f, 1.0f}, false});
        world.add<l3d::TagComponent>(target, {"RayTarget"});

        // 构造一份输入快照：左键按下、鼠标位移 (3,-2)、位置 (120,80)、滚轮 1.25。
        // 脚本读到的数值应与这里完全一致，用于证明 ABI 的输入结构体布局匹配。
        input.setMouseButton(l3d::MouseButton::Left, true);
        input.mouseDelta = {3.0f, -2.0f};
        input.mousePosition = {120.0f, 80.0f};
        input.scrollDelta = 1.25f;
        l3d::Logger::instance().clear();

        REQUIRE(host.start(world, physics, input));
        host.update(world, input, 1.0f / 60.0f);
        input.setMouseButton(l3d::MouseButton::Left, false);
        input.mouseDelta = {0.0f, 0.0f};
        input.mousePosition = {0.0f, 0.0f};
        input.scrollDelta = 0.0f;

        // 生成实体的位置与缩放来自 AbiProbe 的固定逻辑，这里核对约定期望值。
        const auto spawned = l3d::findEntityByTag(world, "SpawnedByAbi");
        REQUIRE(spawned.valid());
        CHECK(world.get<l3d::Transform>(spawned).position.x ==
              Catch::Approx(2.0f));
        CHECK(world.get<l3d::Transform>(spawned).position.y ==
              Catch::Approx(0.5f));
        CHECK(world.get<l3d::Transform>(spawned).scale.x == Catch::Approx(1.5f));
        CHECK(world.get<l3d::Transform>(spawned).scale.y == Catch::Approx(1.5f));
        CHECK(world.get<l3d::Transform>(spawned).scale.z == Catch::Approx(1.5f));

        const auto entries = l3d::Logger::instance().snapshot();
        CHECK(std::ranges::any_of(entries, [](const l3d::LogEntry& entry) {
            return entry.message.find("AbiProbe created entity=") != std::string::npos;
        }));
        // 日志中的实体 id 必须与 encodeEntity(target) 一致，说明 ABI 两端指向同一实体。
        const auto rayLog = "AbiProbe ray hit entity=" +
                            std::to_string(l3d::ScriptHost::encodeEntity(target));
        CHECK(std::ranges::any_of(entries, [&rayLog](const l3d::LogEntry& entry) {
            return entry.message.find(rayLog) != std::string::npos;
        }));
        CHECK(std::ranges::any_of(entries, [](const l3d::LogEntry& entry) {
            return entry.message.find(
                       "AbiProbe mouse left=1 delta=3.00,-2.00 position=120.00,80.00 "
                       "scroll=1.25") != std::string::npos;
        }));
        host.stop();
    }

    {
        // 端到端回归：加载示例场景，用 D 右移、W 前进驱动完整游戏循环。
        // 期望玩家捡到道具、穿过并打开触发门，同时第三人称相机跟随移动。
        const auto scenePath =
            std::filesystem::path(L3D_TEST_ASSET_ROOT) / "scenes" / "sample.scene.json";
        auto loaded = l3d::SceneSerializer::load(scenePath);
        REQUIRE(loaded.ok());
        auto& world = *loaded.value();
        const auto player = l3d::findEntityByTag(world, "Player");
        const auto camera = l3d::findEntityByTag(world, "MainCamera");
        const auto door = l3d::findEntityByTag(world, "Door");
        const auto pickup = l3d::findEntityByTag(world, "Pickup");
        REQUIRE(player.valid());
        REQUIRE(camera.valid());
        REQUIRE(door.valid());
        REQUIRE(pickup.valid());

        const auto doorStartHeight = world.get<l3d::Transform>(door).position.y;
        const auto cameraStart = world.get<l3d::Transform>(camera).position;
        REQUIRE(host.start(world, physics, input));

        // 把一帧的三步固定下来（脚本更新、物理步进、事件分发），避免用例里顺序写错。
        auto simulateStep = [&]() {
            host.update(world, input, 1.0f / 60.0f);
            std::vector<l3d::CollisionEvent> events;
            physics.step(world, 1.0f / 60.0f, events);
            host.dispatchEvents(world, events, input);
        };

        // 先向右（D）走 20 帧，再向前（W）走 240 帧，兼顾拾取道具与触发开门两条路径。
        input.setKey(l3d::Key::D, true);
        for (int step = 0; step < 20; ++step) {
            simulateStep();
        }
        input.setKey(l3d::Key::D, false);
        input.setKey(l3d::Key::W, true);
        for (int step = 0; step < 240; ++step) {
            simulateStep();
        }
        input.setKey(l3d::Key::W, false);

        CHECK_FALSE(world.alive(pickup));
        CHECK(world.alive(player));
        CHECK(world.get<l3d::Transform>(player).position.x > 1.5f);
        CHECK(world.get<l3d::Transform>(door).position.y > doorStartHeight + 1.0f);
        // 相机位置只要与初始值不同，就说明第三人称脚本在持续跟随玩家。
        const auto cameraPosition = world.get<l3d::Transform>(camera).position;
        CHECK((cameraPosition.z != cameraStart.z || cameraPosition.y != cameraStart.y));
        host.stop();
    }

    l3d::Logger::instance().setConsoleEnabled(true);
}
