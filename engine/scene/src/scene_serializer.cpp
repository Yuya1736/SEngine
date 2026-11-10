// 场景序列化实现：World 与版本 1 JSON 的双向转换。
// 所属 engine_scene 模块；engine_app 组合根调用 load/save，自身不处理文件细节。
// 关键约束：资产与脚本参数都以字符串保存；加载失败返回 Result，绝不返回半成品 World。
// JSON 实体 id 只是文件内局部编号，用于表达父子引用，不是运行期 Entity 句柄。
#include "l3d/scene/scene_serializer.h"

#include <stdexcept>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"
#include "l3d/scene/components.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/world.h"

namespace l3d {

namespace {
using Json = nlohmann::json;

// 以下辅助函数只在加载/保存内部使用，不暴露到头文件。
// 向量统一写成 [x, y, z] 三元数组，保持 JSON 紧凑且易读。
Json toJson(const glm::vec3& value) { return Json::array({value.x, value.y, value.z}); }

// 先校验是三元数组再读取；格式错误抛异常，由外层 load 统一转成错误。
glm::vec3 readVec3(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        throw std::runtime_error("Expected a three-component vector");
    }
    return {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
}

// 四元数可读性差，文件里保存角度制欧拉角，方便手改和调试。
Json rotationToJson(const glm::quat& rotation) {
    const auto euler = glm::degrees(glm::eulerAngles(rotation));
    return Json::array({euler.x, euler.y, euler.z});
}

// 读入欧拉角并转回四元数；欧拉角只用于存储，运行期统一使用四元数。
glm::quat rotationFromJson(const Json& value) {
    const auto euler = readVec3(value);
    return glm::quat(glm::radians(euler));
}

// 把一个实体写成 JSON 对象；ids 提供“运行期句柄 -> 文件 id”的映射。
// 各组件按类型可选写入，实体没有的组件不会出现在 JSON 中。
Json serializeEntity(const World& world, Entity entity,
                     const std::unordered_map<Entity, int>& ids) {
    Json object;
    object["id"] = ids.at(entity);
    object["name"] = displayName(world, entity);
    Json components = Json::object();

    if (const auto* transform = world.tryGet<Transform>(entity)) {
        components["transform"] = {{"position", toJson(transform->position)},
                                   {"rotationEuler", rotationToJson(transform->rotation)},
                                   {"scale", toJson(transform->scale)}};
    }
    if (const auto* hierarchy = world.tryGet<Hierarchy>(entity)) {
        // 父实体不在导出集合时写 -1，加载端会把它视为根节点。
        const auto parent = ids.find(hierarchy->parent);
        components["hierarchy"] = {{"parent", parent == ids.end() ? -1 : parent->second}};
    }
    if (const auto* camera = world.tryGet<CameraComponent>(entity)) {
        // 相机参数逐字段写出；加载时缺失字段会回落到默认值。
        components["camera"] = {{"fov", camera->fieldOfViewDegrees},
                                {"near", camera->nearClip},
                                {"far", camera->farClip},
                                {"primary", camera->primary}};
    }
    if (const auto* renderer = world.tryGet<MeshRenderer>(entity)) {
        // 网格与材质保存 GUID 字符串，运行期由资产数据库解析为实际资源。
        components["meshRenderer"] = {{"mesh", renderer->meshGuid},
                                      {"material", renderer->materialGuid},
                                      {"visible", renderer->visible}};
    }
    if (const auto* light = world.tryGet<DirectionalLight>(entity)) {
        // 方向光的向量字段复用统一的 [x, y, z] 表示。
        components["directionalLight"] = {{"direction", toJson(light->direction)},
                                          {"color", toJson(light->color)},
                                          {"intensity", light->intensity}};
    }
    if (const auto* light = world.tryGet<PointLight>(entity)) {
        // 点光位置来自实体 Transform，因此这里只保存颜色、强度和范围。
        components["pointLight"] = {{"color", toJson(light->color)},
                                    {"intensity", light->intensity},
                                    {"range", light->range}};
    }
    if (const auto* collider = world.tryGet<BoxCollider>(entity)) {
        // 碰撞体只保存几何参数和 trigger 标记，物理状态不进入场景文件。
        components["boxCollider"] = {{"halfExtents", toJson(collider->halfExtents)},
                                     {"trigger", collider->trigger}};
    }
    if (const auto* collider = world.tryGet<SphereCollider>(entity)) {
        // 球形碰撞体只存半径和 trigger；球心来自实体 Transform。
        components["sphereCollider"] = {{"radius", collider->radius},
                                        {"trigger", collider->trigger}};
    }
    if (const auto* controller = world.tryGet<CharacterController>(entity)) {
        // velocity 和 grounded 属于运行期状态，故意不写入场景文件。
        components["characterController"] = {{"halfExtents", toJson(controller->halfExtents)},
                                             {"gravity", controller->gravity}};
    }
    if (const auto* script = world.tryGet<ScriptComponent>(entity)) {
        // properties 保持为一个字符串，参数结构由具体脚本自行解释。
        components["script"] = {{"name", script->scriptName},
                                {"properties", script->propertiesJson}};
    }
    if (const auto* tag = world.tryGet<TagComponent>(entity)) {
        // 标签是纯字符串，不引入额外对象层级。
        components["tag"] = tag->value;
    }
    object["components"] = std::move(components);
    return object;
}

} // namespace

Result<std::unique_ptr<World>> SceneSerializer::load(const std::filesystem::path& path) {
    // 文件读取失败立即返回，调用者的现有场景不会被改动。
    auto text = readTextFile(path);
    if (!text) {
        return Result<std::unique_ptr<World>>::failure("Cannot read scene: " + pathToUtf8(path));
    }
    try {
        // 解析和字段读取都放在 try 内，JSON 类型错误会抛出异常并统一转成错误。
        const auto json = Json::parse(*text);
        const int version = json.value("version", 0);
        // 版本不匹配时拒绝加载，避免把未知格式误当成当前格式。
        if (version != 1) {
            return Result<std::unique_ptr<World>>::failure("Unsupported scene version: " +
                                                           std::to_string(version));
        }
        // 顶层必须是 entities 数组，空数组也合法。
        if (!json.contains("entities") || !json["entities"].is_array()) {
            return Result<std::unique_ptr<World>>::failure("Scene has no entities array");
        }

        // 第一遍只创建实体并记录“文件 id -> Entity 句柄”，为父子引用做准备。
        auto world = std::make_unique<World>();
        std::unordered_map<int, Entity> entities;
        for (const auto& entry : json["entities"]) {
            const int id = entry.value("id", -1);
            // id 缺失会得到 -1；负数、重复 id 都视为损坏文件。
            if (id < 0 || entities.contains(id)) {
                return Result<std::unique_ptr<World>>::failure("Invalid or duplicate entity id");
            }
            const auto entity = world->create();
            entities[id] = entity;
            if (entry.contains("name")) {
                // 名称直接放在实体对象顶层，不套组件结构，便于阅读和手改。
                world->add<NameComponent>(entity, {entry["name"].get<std::string>()});
            }
        }

        // 第二遍添加组件；所有实体都已存在，因此可以安全地解析父引用。
        for (const auto& entry : json["entities"]) {
            const int id = entry.value("id", -1);
            const auto entity = entities.at(id);
            // components 可省略：这种实体只有文件 id 和可选的 name。
            if (!entry.contains("components")) {
                continue;
            }
            const auto& components = entry["components"];

            if (components.contains("transform")) {
                // 只恢复局部 TRS；worldMatrix 是派生缓存，稍后由变换系统重算。
                const auto& value = components["transform"];
                auto& transform = world->add<Transform>(entity);
                transform.position = readVec3(value.at("position"));
                transform.rotation = rotationFromJson(value.at("rotationEuler"));
                transform.scale = readVec3(value.at("scale"));
            }
            if (components.contains("hierarchy")) {
                const int parentId = components["hierarchy"].value("parent", -1);
                if (parentId >= 0) {
                    // 引用了不存在的实体说明文件损坏，返回错误而不是静默丢失层级。
                    const auto parentPosition = entities.find(parentId);
                    if (parentPosition == entities.end()) {
                        return Result<std::unique_ptr<World>>::failure(
                            "Hierarchy references a missing entity");
                    }
                    world->add<Hierarchy>(entity, {parentPosition->second});
                }
            }
            if (components.contains("camera")) {
                // 缺失字段的 value() 会沿用组件当前的默认值，便于旧文件升级。
                const auto& value = components["camera"];
                auto& camera = world->add<CameraComponent>(entity);
                camera.fieldOfViewDegrees = value.value("fov", camera.fieldOfViewDegrees);
                camera.nearClip = value.value("near", camera.nearClip);
                camera.farClip = value.value("far", camera.farClip);
                camera.primary = value.value("primary", camera.primary);
            }
            if (components.contains("meshRenderer")) {
                // 这里只恢复 GUID 字符串，真正加载网格/材质由 assets 模块负责。
                const auto& value = components["meshRenderer"];
                auto& renderer = world->add<MeshRenderer>(entity);
                renderer.meshGuid = value.value("mesh", renderer.meshGuid);
                renderer.materialGuid = value.value("material", renderer.materialGuid);
                renderer.visible = value.value("visible", renderer.visible);
            }
            if (components.contains("directionalLight")) {
                const auto& value = components["directionalLight"];
                auto& light = world->add<DirectionalLight>(entity);
                light.direction = readVec3(value.at("direction"));
                light.color = readVec3(value.at("color"));
                light.intensity = value.value("intensity", light.intensity);
            }
            if (components.contains("pointLight")) {
                const auto& value = components["pointLight"];
                auto& light = world->add<PointLight>(entity);
                light.color = readVec3(value.at("color"));
                light.intensity = value.value("intensity", light.intensity);
                light.range = value.value("range", light.range);
            }
            if (components.contains("boxCollider")) {
                const auto& value = components["boxCollider"];
                auto& collider = world->add<BoxCollider>(entity);
                collider.halfExtents = readVec3(value.at("halfExtents"));
                collider.trigger = value.value("trigger", collider.trigger);
            }
            if (components.contains("sphereCollider")) {
                const auto& value = components["sphereCollider"];
                auto& collider = world->add<SphereCollider>(entity);
                collider.radius = value.value("radius", collider.radius);
                collider.trigger = value.value("trigger", collider.trigger);
            }
            if (components.contains("characterController")) {
                const auto& value = components["characterController"];
                auto& controller = world->add<CharacterController>(entity);
                controller.halfExtents = readVec3(value.at("halfExtents"));
                controller.gravity = value.value("gravity", controller.gravity);
            }
            if (components.contains("script")) {
                // 名称和参数都按字符串读入，场景层不依赖脚本插件的具体 ABI。
                const auto& value = components["script"];
                auto& script = world->add<ScriptComponent>(entity);
                script.scriptName = value.value("name", std::string{});
                script.propertiesJson = value.value("properties", std::string("{}"));
            }
            if (components.contains("tag")) {
                // 标签不要求唯一；需要唯一性时由上层逻辑自行检查。
                world->add<TagComponent>(entity, {components["tag"].get<std::string>()});
            }
        }

        // 世界矩阵留到加载后由组合根统一调用 updateWorldTransforms 计算。
        L3D_INFO("Loaded scene '{}' with {} entities", pathToUtf8(path), entities.size());
        return Result<std::unique_ptr<World>>::success(std::move(world));
    } catch (const std::exception& exception) {
        // 任意解析/类型异常都在这里收口；world 是局部对象，不会泄漏给调用者。
        return Result<std::unique_ptr<World>>::failure("Invalid scene '" + pathToUtf8(path) +
                                                       "': " + exception.what());
    }
}

Result<bool> SceneSerializer::save(const World& world, const std::filesystem::path& path) {
    try {
        // 保存时重新分配连续 id；文件 id 不必等于 index/generation，只要本次一致即可。
        std::unordered_map<Entity, int> ids;
        int nextId = 0;
        for (const auto entity : world.entities()) {
            ids[entity] = nextId++;
        }
        // 先建立完整映射再写实体，父实体无论遍历顺序如何都能解析出正确 id。
        // 顶层结构固定为 version + entities，version 为未来格式迁移预留。
        Json root;
        root["version"] = 1;
        root["entities"] = Json::array();
        for (const auto entity : world.entities()) {
            root["entities"].push_back(serializeEntity(world, entity, ids));
        }
        // 原子写入：先写临时文件再替换目标，避免中途失败留下半个场景。
        // dump(2) 输出两空格缩进，末尾补换行，方便文本工具和人工差异比较。
        if (!writeTextFileAtomic(path, root.dump(2) + "\n")) {
            return Result<bool>::failure("Cannot write scene: " + pathToUtf8(path));
        }
        L3D_INFO("Saved scene '{}' with {} entities", pathToUtf8(path), ids.size());
        return Result<bool>::success(true);
    } catch (const std::exception& exception) {
        // 序列化期间的异常同样转成 Result，调用方无需处理 C++ 异常。
        return Result<bool>::failure("Cannot serialize scene: " + std::string(exception.what()));
    }
}

} // namespace l3d
