// 文件职责：实现编辑器 UI（ImGui 面板、视口相机、鼠标拾取、渲染帧提取），所属模块 engine/editor。
// 分层位置：engine_app 中的 Application 每帧调用 draw()；本文件只读取 World 并写请求，
// Play/Stop、存读场景等改变引擎生命周期的事情由 Application 依据 EditorRequests 执行。
// 关键约束：不在遍历 World 的过程中销毁实体，所有删除延迟到 draw() 末尾统一处理。
#include "l3d/editor/editor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

// ImGui 是“即时模式”UI：每帧用代码重新描述一次界面，不保存控件对象；
// 控件函数的返回值就代表这一帧的交互结果（例如按钮是否被按下）。
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "l3d/assets/asset_database.h"
#include "l3d/core/input.h"
#include "l3d/core/logging.h"
#include "l3d/renderer/renderer.h"
#include "l3d/renderer/render_extraction.h"
#include "l3d/scripting/script_host.h"
#include "l3d/scene/components.h"
#include "l3d/scene/scene_utils.h"
#include "l3d/scene/transform_system.h"
#include "l3d/scene/world.h"

namespace l3d {

namespace {

// 视口拾取用的世界空间射线。
struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// 射线与轴对齐包围盒（AABB）求交的 slab 法：沿每个轴求出射线进入和离开“平板”的距离，
// 三个区间有公共交集才算命中，near 即最近交点距离。只用于编辑器点选，不追求极致性能。
bool intersectAabb(const Ray& ray, const glm::vec3& minimum, const glm::vec3& maximum,
                   float& distance) {
    float near = 0.0f;
    float far = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        // 方向分量接近 0 表示射线与该轴平行，无法计算交点距离，只需判断原点是否落在平板内。
        if (std::abs(ray.direction[axis]) < 1e-7f) {
            if (ray.origin[axis] < minimum[axis] || ray.origin[axis] > maximum[axis]) {
                return false;
            }
            continue;
        }
        float first = (minimum[axis] - ray.origin[axis]) / ray.direction[axis];
        float second = (maximum[axis] - ray.origin[axis]) / ray.direction[axis];
        if (first > second) {
            std::swap(first, second);
        }
        near = std::max(near, first);
        far = std::min(far, second);
        if (near > far) {
            return false;
        }
    }
    distance = near;
    // 只接受射线前方（t ≥ 0）的交点：否则相机背后的盒子也会被当成命中。
    return near >= 0.0f;
}

// 把网格本地 AABB 的 8 个角点变换到世界空间后重新求包围盒：旋转后的物体不便于做 OBB 求交，
// 用这个略大的保守 AABB 已足够应对编辑器点选。
std::pair<glm::vec3, glm::vec3> transformedBounds(const MeshBounds& bounds,
                                                  const glm::mat4& transform) {
    glm::vec3 minimum(std::numeric_limits<float>::max());
    glm::vec3 maximum(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point{corner & 1 ? bounds.max.x : bounds.min.x,
                              corner & 2 ? bounds.max.y : bounds.min.y,
                              corner & 4 ? bounds.max.z : bounds.min.z};
        const auto world = glm::vec3(transform * glm::vec4(point, 1.0f));
        minimum = glm::min(minimum, world);
        maximum = glm::max(maximum, world);
    }
    return {minimum, maximum};
}

// 内置资产没有磁盘文件，用固定可读名；普通资产显示文件名，查不到时退回原始 GUID。
std::string shortAssetLabel(const IAssetDatabase& assets, const std::string& guid) {
    if (guid == BuiltinCubeGuid) return "Cube";
    if (guid == BuiltinPlaneGuid) return "Plane";
    if (guid == BuiltinSphereGuid) return "Sphere";
    if (guid == BuiltinDefaultMaterialGuid) return "Default Material";
    if (const auto* record = assets.findByGuid(guid)) {
        return record->path.filename().string();
    }
    return guid.empty() ? "<none>" : guid;
}

// 网格下拉框：先列 3 个内置网格，再列资产库中全部 Mesh；返回 true 表示本帧改了选择。
bool meshCombo(const char* label, const IAssetDatabase& assets, std::string& guid) {
    bool changed = false;
    if (ImGui::BeginCombo(label, shortAssetLabel(assets, guid).c_str())) {
        const std::array<std::string_view, 3> builtins = {BuiltinCubeGuid, BuiltinPlaneGuid,
                                                          BuiltinSphereGuid};
        for (const auto builtin : builtins) {
            const std::string value(builtin);
            const bool selected = guid == value;
            if (ImGui::Selectable(shortAssetLabel(assets, value).c_str(), selected)) {
                guid = value;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        for (const auto* record : assets.byType(AssetType::Mesh)) {
            // "##" 之后的内容是 ImGui 的隐藏 ID：同名文件也能被区分，且不会显示在界面上。
            const auto caption = record->path.filename().string() + "##" + record->guid;
            const bool selected = guid == record->guid;
            if (ImGui::Selectable(caption.c_str(), selected)) {
                guid = record->guid;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// 材质下拉框：内置默认材质 + 资产库中的全部材质。
bool materialCombo(const char* label, const IAssetDatabase& assets, std::string& guid) {
    bool changed = false;
    if (ImGui::BeginCombo(label, shortAssetLabel(assets, guid).c_str())) {
        const std::string defaultMaterial(BuiltinDefaultMaterialGuid);
        if (ImGui::Selectable("Default Material", guid == defaultMaterial)) {
            guid = defaultMaterial;
            changed = true;
        }
        for (const auto* record : assets.byType(AssetType::Material)) {
            const auto caption = record->path.filename().string() + "##" + record->guid;
            const bool selected = guid == record->guid;
            if (ImGui::Selectable(caption.c_str(), selected)) {
                guid = record->guid;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// 脚本下拉框：ScriptComponent 里存的是“脚本类名”而非 GUID，因此直接与 catalog() 中的名字比较。
bool scriptCombo(const IScriptHost& scripts, std::string& guid) {
    bool changed = false;
    if (ImGui::BeginCombo("Script", guid.empty() ? "<none>" : guid.c_str())) {
        for (const auto& name : scripts.catalog()) {
            const bool selected = guid == name;
            if (ImGui::Selectable(name.c_str(), selected)) {
                guid = name;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

} // namespace

// 构造函数只保存引用，不分配 GPU 资源、不创建 ImGui 窗口；窗口由 draw() 每帧声明。
Editor::Editor(IAssetDatabase& assets, IRenderer& renderer, const IScriptHost& scripts,
               const InputSnapshot& input)
    : assets_(assets), renderer_(renderer), scripts_(scripts), input_(input) {}

void Editor::draw(World& world, bool playing) {
    // 请求是“本帧快照”：先清空，绘制过程中被点到的按钮才会重新置位。
    requests_ = {};
    deleteRequest_ = {};
    // 每个面板都是 Begin/End 之间的即时绘制调用，顺序固定，界面每帧完整重建一次。
    drawMenuBar(world, playing);
    drawToolbar(world, playing);
    drawHierarchy(world);
    drawInspector(world);
    drawAssetBrowser(world);
    drawConsole();
    // 视口最后绘制：它根据前面面板剩余的可用区域确定尺寸。
    drawViewport(world);

    // 删除统一放到所有面板绘制完之后：面板遍历 world.entities() 时不能修改容器，
    // 否则容器可能重排、迭代状态失效；此时绘制已结束，再真正销毁实体是安全的。
    if (deleteRequest_.valid()) {
        destroyEntityRecursive(world, deleteRequest_);
        // 删除的正好是当前选中项时同步清空，避免 selected_ 变成悬空句柄。
        if (selected_ == deleteRequest_) {
            selected_ = {};
        }
    }
    // 实体可能被脚本或本帧操作销毁，选中句柄必须做一次有效性检查。
    if (selected_.valid() && !world.alive(selected_)) {
        selected_ = {};
    }
}

// World 被整体替换（新建/加载场景）后，旧 Entity 句柄不再表示任何对象，需要丢弃选中项。
void Editor::notifyWorldReplaced(World& world) {
    if (!world.alive(selected_)) {
        selected_ = {};
    }
}

void Editor::drawMenuBar(World& /*world*/, bool /*playing*/) {
    // 当前菜单不依赖 World 或播放状态；参数保留是为了与其它面板的绘制签名统一。
    // 菜单项只置位请求标志：UI 不直接读写文件或切换场景，避免在绘制中途改变引擎状态。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Scene", "Ctrl+N")) requests_.newScene = true;
            if (ImGui::MenuItem("Open Current Scene", "Ctrl+O")) requests_.loadScene = true;
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) requests_.saveScene = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) requests_.exit = true;
            ImGui::EndMenu();
        }
        // 帮助菜单只展示操作提示，菜单项被设置为不可点击（最后一个参数 false）。
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Right mouse + WASD/QE in viewport to fly", nullptr, false, false);
            ImGui::MenuItem("C# scripts are loaded from GameScripts.dll", nullptr, false, false);
            ImGui::EndMenu();
        }
        // 菜单栏右侧显示当前场景文件路径；新建但尚未保存的场景显示为空。
        ImGui::TextDisabled("%s", scenePath_.c_str());
        ImGui::EndMainMenuBar();
    }
}

void Editor::drawToolbar(World& world, bool playing) {
    // 工具栏是常驻窗口（NoCollapse 禁止折叠）；按钮同样只产生请求。
    ImGui::Begin("Toolbar", nullptr, ImGuiWindowFlags_NoCollapse);
    // Play/Stop 由 Application 执行：它负责启动/停止物理与脚本模拟，编辑器无权直接切换。
    if (!playing) {
        if (ImGui::Button("Play")) requests_.play = true;
    } else {
        if (ImGui::Button("Stop")) requests_.stop = true;
    }
    ImGui::SameLine();
    // 保存/加载同样交给 Application：只有它知道当前场景路径和序列化流程。
    if (ImGui::Button("Save")) requests_.saveScene = true;
    ImGui::SameLine();
    if (ImGui::Button("Load")) requests_.loadScene = true;
    ImGui::SameLine();
    // 新建实体只是往 World 追加，不影响正在遍历的容器，因此可以在 UI 中直接完成。
    if (ImGui::Button("New Entity")) {
        selected_ = world.create();
        world.add<NameComponent>(selected_, {"Entity"});
        world.add<Transform>(selected_);
        world.add<MeshRenderer>(selected_);
    }
    ImGui::SameLine();
    // 对象数直接取存活实体数量（包含没有任何组件的实体），仅作为调试信息。
    ImGui::Text("Objects: %zu | Selected: %s", world.entities().size(),
                selected_.valid() ? displayName(world, selected_).c_str() : "<none>");
    ImGui::End();
}

// 由 yaw/pitch 求相机前向向量，再构造“看向前方、以世界 Y 轴为上”的朝向四元数。
// pitch 在 updateCamera 中被限制在 ±89°，避免前向与 up 向量共线导致 lookAt 退化。
// 注意：forward 公式在本文件出现了三处（朝向、视图、移动），若调整约定需同步修改。
glm::quat Editor::cameraRotation() const {
    const float yaw = glm::radians(cameraYawDegrees_);
    const float pitch = glm::radians(cameraPitchDegrees_);
    const glm::vec3 forward{std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw)};
    // quatLookAt 要求方向向量已归一化，否则结果会包含缩放或数值误差。
    return glm::quatLookAt(glm::normalize(forward), glm::vec3(0.0f, 1.0f, 0.0f));
}

// 视图矩阵必须与 cameraRotation() 使用同一套 yaw/pitch 约定，否则画面朝向会与拾取射线不一致。
glm::mat4 Editor::viewMatrix() const {
    const float yaw = glm::radians(cameraYawDegrees_);
    const float pitch = glm::radians(cameraPitchDegrees_);
    const glm::vec3 forward{std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw)};
    return glm::lookAt(cameraPosition_, cameraPosition_ + glm::normalize(forward),
                       glm::vec3(0.0f, 1.0f, 0.0f));
}

// 透视投影：fov 为纵向视角，aspect 为宽/高；宽高比非法时退化为 1，防止除零得到 NaN。
glm::mat4 Editor::projectionMatrix(float aspect) const {
    return glm::perspective(glm::radians(fovDegrees_), aspect > 0.0f ? aspect : 1.0f,
                            nearClip_, farClip_);
}

RenderFrame Editor::buildRenderFrame(World& world) const {
    // 窗口最小化时视口高度可能为 0，这里做保护，保证渲染数据始终有合法宽高比。
    const float aspect = viewportHeight_ > 0
                             ? static_cast<float>(viewportWidth_) / viewportHeight_
                             : 1.0f;
    // 提取阶段把 ECS 组件拍平成渲染器可直接消费的 POD 列表，并带上相机矩阵与位置。
    return extractRenderFrameWithCamera(world, viewMatrix(), projectionMatrix(aspect),
                                        cameraPosition_);
}

void Editor::drawHierarchy(World& world) {
    // Hierarchy 面板：实体树的唯一入口，同时负责“选中”和“请求删除”。
    // World 是 ECS 风格的组件容器：Entity 只是句柄，组件才决定实体的外观与行为，
    // 所以新建实体后要分别 add<NameComponent>、add<Transform> 等组件。
    ImGui::Begin("Hierarchy");
    // 默认实体只带名称与变换；视觉、物理、脚本组件可在 Inspector 中按需添加。
    if (ImGui::Button("Create Entity")) {
        selected_ = world.create();
        world.add<NameComponent>(selected_, {"Entity"});
        world.add<Transform>(selected_);
    }
    ImGui::SameLine();
    // 只记录删除意图，稍后在 draw() 末尾真正销毁；这样不会在面板遍历途中破坏实体容器。
    if (ImGui::Button("Delete Selected") && selected_.valid()) {
        deleteRequest_ = selected_;
    }
    ImGui::Separator();

    // 只遍历根节点（无父节点或父节点已失效），其余实体由递归的树节点负责显示。
    Entity menuDelete{};
    for (const auto entity : world.entities()) {
        const auto* hierarchy = world.tryGet<Hierarchy>(entity);
        if (!hierarchy || !hierarchy->parent.valid() || !world.alive(hierarchy->parent)) {
            drawEntityTree(world, entity, menuDelete);
        }
    }
    if (menuDelete.valid()) {
        deleteRequest_ = menuDelete;
    }
    ImGui::End();
}

void Editor::drawEntityTree(World& world, Entity entity, Entity& deleteRequest) {
    // 先收集子节点：ImGui 需要提前知道是否有子节点，才能显示展开箭头或叶子标记。
    std::vector<Entity> children;
    for (const auto candidate : world.entities()) {
        const auto* hierarchy = world.tryGet<Hierarchy>(candidate);
        if (hierarchy && hierarchy->parent == entity) {
            children.push_back(candidate);
        }
    }
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    // 叶子节点不压入 ImGui 的树栈（NoTreePushOnOpen），因此下面无需对叶子调用 TreePop。
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (selected_ == entity) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    // 用实体 index 拼出隐藏 ID（## 之后），避免两个实体同名时 ImGui 把它们视为同一个控件。
    const auto label = displayName(world, entity) + "##entity" + std::to_string(entity.index);
    const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    // 点击展开箭头只负责展开/收起，不应改变选中；只有点击行内文字才切换选中项。
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected_ = entity;
    }
    // 右键菜单同样只是记录删除意图，避免在遍历树时销毁节点。
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Delete")) {
            deleteRequest = entity;
        }
        ImGui::EndPopup();
    }
    // 节点展开且有子节点时递归绘制；TreePop 必须与 TreeNodeEx 的展开状态配对，
    // 否则 ImGui 的树栈会失衡，影响后续节点的缩进。
    if (open && !children.empty()) {
        for (const auto child : children) {
            drawEntityTree(world, child, deleteRequest);
        }
        ImGui::TreePop();
    }
}

void Editor::drawAddComponentMenu(World& world, Entity entity) {
    // 该弹出菜单由 Inspector 中的“Add Component”按钮通过同名 OpenPopup 触发。
    if (!ImGui::BeginPopup("AddComponentPopup")) {
        return;
    }
    // 通用小工具：点击菜单项 → 执行添加回调 → 关闭弹窗，避免每个组件重复三行代码。
    auto add = [&](const char* label, auto callback) {
        if (ImGui::MenuItem(label)) {
            callback();
            ImGui::CloseCurrentPopup();
        }
    };
    // 只列出实体当前缺少的组件：同一类型的组件不允许重复添加。
    if (!world.has<Transform>(entity)) add("Transform", [&] { world.add<Transform>(entity); });
    if (!world.has<Hierarchy>(entity)) add("Hierarchy", [&] { world.add<Hierarchy>(entity); });
    if (!world.has<CameraComponent>(entity)) add("Camera", [&] { world.add<CameraComponent>(entity); });
    if (!world.has<MeshRenderer>(entity)) add("Mesh Renderer", [&] { world.add<MeshRenderer>(entity); });
    if (!world.has<DirectionalLight>(entity)) add("Directional Light", [&] { world.add<DirectionalLight>(entity); });
    if (!world.has<PointLight>(entity)) add("Point Light", [&] { world.add<PointLight>(entity); });
    if (!world.has<BoxCollider>(entity)) add("Box Collider", [&] { world.add<BoxCollider>(entity); });
    if (!world.has<SphereCollider>(entity)) add("Sphere Collider", [&] { world.add<SphereCollider>(entity); });
    if (!world.has<CharacterController>(entity)) add("Character Controller", [&] { world.add<CharacterController>(entity); });
    if (!world.has<ScriptComponent>(entity)) add("Script", [&] { world.add<ScriptComponent>(entity); });
    if (!world.has<TagComponent>(entity)) add("Tag", [&] { world.add<TagComponent>(entity); });
    ImGui::EndPopup();
}

void Editor::drawInspector(World& world) {
    // Inspector 按“实体实际拥有的组件”动态生成控件：没有该组件的实体不显示对应区块。
    ImGui::Begin("Inspector");
    // 没有选中或选中实体已被销毁时只显示提示，直接返回，避免后面访问空组件指针。
    if (!selected_.valid() || !world.alive(selected_)) {
        ImGui::TextDisabled("Select an entity in the Hierarchy or Viewport.");
        ImGui::End();
        return;
    }

    // 实体可以没有 NameComponent；为了面板正常工作，这里补一个默认名字（不新建实体）。
    auto* name = world.tryGet<NameComponent>(selected_);
    if (!name) {
        name = &world.add<NameComponent>(selected_, {"Entity"});
    }
    // ImGui 的 InputText 需要可写的定长 char 缓冲区：先复制 std::string，编辑成功后再写回。
    std::array<char, 128> nameBuffer{};
    std::snprintf(nameBuffer.data(), nameBuffer.size(), "%s", name->value.c_str());
    if (ImGui::InputText("Name", nameBuffer.data(), nameBuffer.size())) {
        name->value = nameBuffer.data();
    }
    // 弹出菜单的 ID 字符串必须与 drawAddComponentMenu() 中 BeginPopup 的名字完全一致。
    if (ImGui::Button("Add Component")) {
        ImGui::OpenPopup("AddComponentPopup");
    }
    drawAddComponentMenu(world, selected_);
    ImGui::Separator();

    // Transform 决定实体的位姿，渲染、物理、拾取都依赖它；没有 Transform 的实体无法定位。
    if (auto* transform = world.tryGet<Transform>(selected_)) {
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Position", &transform->position.x, 0.05f);
            // Transform 内部用四元数保存旋转；面板用欧拉角编辑，故每帧做一次四元数↔欧拉角换算。
            auto euler = glm::degrees(glm::eulerAngles(transform->rotation));
            if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
                transform->rotation = glm::quat(glm::radians(euler));
            }
            ImGui::DragFloat3("Scale", &transform->scale.x, 0.05f, 0.01f, 100.0f);
            if (ImGui::Button("Remove##transform")) {
                // 移除组件后 transform 指针立刻失效，必须马上结束本次面板绘制，不能继续使用。
                world.remove<Transform>(selected_);
                ImGui::End();
                return;
            }
        }
    }
    // Hierarchy 组件只记录父实体；最终世界变换由 TransformSystem 沿父链递归计算。
    if (auto* hierarchy = world.tryGet<Hierarchy>(selected_)) {
        if (ImGui::CollapsingHeader("Hierarchy")) {
            ImGui::Text("Parent: %s", world.alive(hierarchy->parent)
                                          ? displayName(world, hierarchy->parent).c_str()
                                          : "<root>");
            // 解除父子关系后，该实体将在 Hierarchy 中变为根节点；选择父节点目前需通过场景文件。
            if (hierarchy->parent.valid() && ImGui::Button("Unparent")) {
                hierarchy->parent = {};
            }
        }
    }
    // 相机组件的 FOV 与近/远裁剪面会进入投影矩阵，primary 标记场景主相机。
    if (auto* camera = world.tryGet<CameraComponent>(selected_)) {
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat("Field of View", &camera->fieldOfViewDegrees, 0.5f, 10.0f, 150.0f);
            ImGui::DragFloat("Near", &camera->nearClip, 0.01f, 0.01f, 10.0f);
            ImGui::DragFloat("Far", &camera->farClip, 1.0f, 1.0f, 5000.0f);
            ImGui::Checkbox("Primary", &camera->primary);
        }
    }
    // MeshRenderer 用 GUID 引用资产库中的网格与材质，运行到提取阶段才解析成具体资源。
    if (auto* mesh = world.tryGet<MeshRenderer>(selected_)) {
        if (ImGui::CollapsingHeader("Mesh Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            meshCombo("Mesh", assets_, mesh->meshGuid);
            materialCombo("Material", assets_, mesh->materialGuid);
            ImGui::Checkbox("Visible", &mesh->visible);
            if (ImGui::Button("Remove##meshRenderer")) {
                world.remove<MeshRenderer>(selected_);
                ImGui::End();
                return;
            }
        }
    }
    // 光源组件本身不渲染，只向渲染提取阶段提供参数，由提取结果打包给着色器。
    if (auto* light = world.tryGet<DirectionalLight>(selected_)) {
        if (ImGui::CollapsingHeader("Directional Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Direction", &light->direction.x, 0.02f, -1.0f, 1.0f);
            ImGui::ColorEdit3("Color", &light->color.x);
            ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.0f, 20.0f);
        }
    }
    // 点光源额外有作用范围 range：距离超过 range 的像素不会受到这盏灯影响。
    if (auto* light = world.tryGet<PointLight>(selected_)) {
        if (ImGui::CollapsingHeader("Point Light", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::ColorEdit3("Color", &light->color.x);
            ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.0f, 50.0f);
            ImGui::DragFloat("Range", &light->range, 0.1f, 0.1f, 100.0f);
        }
    }
    // 碰撞体参与物理查询；trigger 为 true 时只产生重叠事件，不阻挡实体运动。
    if (auto* collider = world.tryGet<BoxCollider>(selected_)) {
        if (ImGui::CollapsingHeader("Box Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Half Extents", &collider->halfExtents.x, 0.05f, 0.01f, 100.0f);
            ImGui::Checkbox("Trigger", &collider->trigger);
        }
    }
    if (auto* collider = world.tryGet<SphereCollider>(selected_)) {
        if (ImGui::CollapsingHeader("Sphere Collider", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat("Radius", &collider->radius, 0.05f, 0.01f, 100.0f);
            ImGui::Checkbox("Trigger", &collider->trigger);
        }
    }
    // velocity 与 grounded 由物理系统每帧更新，这里只读回显，属于运行时状态而非可编辑配置。
    if (auto* controller = world.tryGet<CharacterController>(selected_)) {
        if (ImGui::CollapsingHeader("Character Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Half Extents", &controller->halfExtents.x, 0.05f, 0.05f, 10.0f);
            ImGui::DragFloat("Gravity", &controller->gravity, 0.5f, -100.0f, 0.0f);
            ImGui::Text("Velocity: %.2f, %.2f, %.2f", controller->velocity.x,
                        controller->velocity.y, controller->velocity.z);
            ImGui::Text("Grounded: %s", controller->grounded ? "true" : "false");
        }
    }
    // ScriptComponent 按类名匹配 DLL 目录中的脚本类型；Properties 会原样传给脚本的 onCreate。
    if (auto* script = world.tryGet<ScriptComponent>(selected_)) {
        if (ImGui::CollapsingHeader("Script", ImGuiTreeNodeFlags_DefaultOpen)) {
            scriptCombo(scripts_, script->scriptName);
            // propertiesJson 对编辑器是不透明字符串：内容格式由 C# 脚本自己解释，这里只负责透传。
            std::array<char, 2048> properties{};
            std::snprintf(properties.data(), properties.size(), "%s",
                          script->propertiesJson.c_str());
            if (ImGui::InputTextMultiline("Properties", properties.data(), properties.size(),
                                          ImVec2(-1.0f, 120.0f))) {
                script->propertiesJson = properties.data();
            }
            if (ImGui::Button("Remove##script")) {
                world.remove<ScriptComponent>(selected_);
                ImGui::End();
                return;
            }
        }
    }
    // Tag 面向逻辑（脚本按标签查找实体），Name 面向界面显示，两者彼此独立。
    if (auto* tag = world.tryGet<TagComponent>(selected_)) {
        if (ImGui::CollapsingHeader("Tag", ImGuiTreeNodeFlags_DefaultOpen)) {
            std::array<char, 64> tagBuffer{};
            std::snprintf(tagBuffer.data(), tagBuffer.size(), "%s", tag->value.c_str());
            if (ImGui::InputText("Tag", tagBuffer.data(), tagBuffer.size())) {
                tag->value = tagBuffer.data();
            }
        }
    }
    ImGui::End();
}

void Editor::drawAssetBrowser(World& world) {
    // 资产浏览器只读取 AssetDatabase；只有点击 Refresh 时才会重新扫描磁盘。
    // 本面板暂不使用 World，保留参数是为与其它面板接口一致。
    (void)world;
    ImGui::Begin("Assets");
    // 刷新会重新扫描磁盘资产目录，开销较大，因此只在用户主动点击时执行。
    if (ImGui::Button("Refresh")) {
        assets_.refresh();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu assets", assets_.assets().size());
    ImGui::Separator();
    // 按资产类型分组展示；没有该类型资产时直接跳过，不显示空分组。
    for (const auto type : {AssetType::Scene, AssetType::Mesh, AssetType::Texture,
                            AssetType::Material, AssetType::Shader}) {
        const auto records = assets_.byType(type);
        if (records.empty()) {
            continue;
        }
        const std::string header(AssetDatabase::typeName(type));
        if (ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            for (const auto* record : records) {
                const auto label = record->path.filename().string() + "##" + record->guid;
                if (ImGui::Selectable(label.c_str(), selectedAssetGuid_ == record->guid)) {
                    selectedAssetGuid_ = record->guid;
                }
                // 双击场景资产：填入路径并请求 Application 加载，不在 UI 中直接替换场景。
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0) &&
                    type == AssetType::Scene) {
                    scenePath_ = record->path.string();
                    requests_.loadScene = true;
                }
            }
        }
    }
    // 选中资产后显示 GUID 与磁盘路径：GUID 是场景文件中引用资产的稳定标识。
    if (!selectedAssetGuid_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("GUID: %s", selectedAssetGuid_.c_str());
        // 刷新后 GUID 仍保留；如果资产已被删除，则只显示 GUID、不显示路径。
        if (const auto* record = assets_.findByGuid(selectedAssetGuid_)) {
            ImGui::TextWrapped("Path: %s", record->path.string().c_str());
        }
    }
    ImGui::End();
}

void Editor::drawConsole() {
    // 日志来自全局 Logger 单例；snapshot() 先取一份拷贝，避免绘制过程中日志继续追加。
    ImGui::Begin("Console");
    if (ImGui::Button("Clear")) {
        Logger::instance().clear();
    }
    // 状态行反映脚本插件的三种状态：未加载 DLL / DLL 已加载但未运行 / 正在运行。
    ImGui::SameLine();
    ImGui::TextDisabled("C# scripts: %s", scripts_.loaded()
                                             ? (scripts_.running() ? "running" : "idle")
                                             : "unavailable");
    ImGui::Separator();
    // 用子窗口承载日志列表，使它拥有独立的滚动条，不影响上方按钮和状态行。
    ImGui::BeginChild("LogEntries", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const auto entries = Logger::instance().snapshot();
    // 只渲染最近 250 条，防止日志无限增长时每帧绘制开销失控。
    const auto start = entries.size() > 250 ? entries.size() - 250 : 0;
    for (std::size_t index = start; index < entries.size(); ++index) {
        // 按日志级别上色：警告橙色、错误红色，方便在大量输出中快速定位问题。
        ImVec4 color{0.85f, 0.85f, 0.85f, 1.0f};
        if (entries[index].level == LogLevel::Warning) {
            color = {1.0f, 0.75f, 0.25f, 1.0f};
        } else if (entries[index].level == LogLevel::Error) {
            color = {1.0f, 0.38f, 0.32f, 1.0f};
        }
        ImGui::TextColored(color, "[%s] %s", entries[index].timestamp.c_str(),
                           entries[index].message.c_str());
    }
    // 若用户已经滚到底部，就跟随新日志自动滚动；否则保留其阅读位置。
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void Editor::drawViewport(World& world) {
    // 视口把渲染器输出的离屏纹理当作普通图片贴进 ImGui 窗口，尺寸跟随窗口内容区变化。
    ImGui::Begin("Viewport");
    const auto available = ImGui::GetContentRegionAvail();
    // 至少保留 16 像素：窗口被拖到极小时，尺寸为 0 的渲染目标会触发图形 API 错误。
    viewportWidth_ = std::max(static_cast<int>(available.x), 16);
    viewportHeight_ = std::max(static_cast<int>(available.y), 16);
    const auto texture = renderer_.viewportTexture();
    if (texture != 0) {
        // 交换上下 V 坐标：GL 纹理原点在左下，而 ImGui 屏幕坐标原点在左上，翻转后画面才正立。
        ImGui::Image(static_cast<ImTextureID>(texture),
                     ImVec2(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_)),
                     ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    } else {
        // 渲染器尚未创建离屏目标时，用同尺寸的占位区域维持布局，并提示用户等待。
        ImGui::Dummy(ImVec2(static_cast<float>(viewportWidth_), static_cast<float>(viewportHeight_)));
        ImGui::TextDisabled("Renderer viewport is initializing...");
    }
    // IsItemHovered 针对“上一个绘制的控件”，因此必须紧跟在视口图像之后调用。
    const auto hovered = ImGui::IsItemHovered();
    viewportHovered_ = hovered;
    // 在视口左上角叠一层选中实体名，纯显示用，不参与布局与拾取；
    // 选中实体可能刚被脚本删除，因此仍要检查 valid()。
    if (selected_.valid()) {
        ImGui::SetCursorPos(ImVec2(12.0f, 32.0f));
        ImGui::TextColored(ImVec4(0.8f, 0.95f, 1.0f, 1.0f), "%s",
                           displayName(world, selected_).c_str());
    }

    // 只有鼠标位于视口内才响应相机飞行与点选，避免干扰其他面板的操作。
    if (hovered) {
        updateCamera(world, hovered);
        // 把鼠标相对图像左上角的偏移归一化到 [0,1]，拾取函数只关心归一化坐标。
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const auto itemMin = ImGui::GetItemRectMin();
            const auto mouse = ImGui::GetIO().MousePos;
            const float normalizedX = (mouse.x - itemMin.x) / static_cast<float>(viewportWidth_);
            const float normalizedY = (mouse.y - itemMin.y) / static_cast<float>(viewportHeight_);
            // 再检查一次归一化范围：鼠标可能在按下与释放之间移出图像区域。
            if (normalizedX >= 0.0f && normalizedX <= 1.0f && normalizedY >= 0.0f &&
                normalizedY <= 1.0f) {
                // 拾取使用的宽高比必须与画面渲染时一致，否则反投影出的射线会偏离鼠标位置。
                const float aspect = static_cast<float>(viewportWidth_) / viewportHeight_;
                pickEntity(world, normalizedX, normalizedY, aspect);
            }
        }
    }
    // 右键飞行期间隐藏光标，模拟第一人称视角的鼠标锁定；松开右键后自动恢复。
    if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }
    ImGui::End();
}

void Editor::updateCamera(World& /*world*/, bool hovered) {
    // 自由飞行相机：仅在鼠标悬停视口且按住右键时生效，WASD 平移、QE 升降、Shift 加速。
    if (!hovered || !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        return;
    }
    auto& io = ImGui::GetIO();
    // 鼠标水平位移改变 yaw，垂直位移改变 pitch；pitch 夹在 ±89° 防止与 up 向量共线。
    cameraYawDegrees_ += io.MouseDelta.x * 0.14f;
    cameraPitchDegrees_ -= io.MouseDelta.y * 0.14f;
    cameraPitchDegrees_ = std::clamp(cameraPitchDegrees_, -89.0f, 89.0f);
    // 滚轮调节飞行速度而非缩放画面，范围限制在 0.5~40 单位/秒。
    if (std::abs(io.MouseWheel) > 0.0f) {
        cameraSpeed_ = std::clamp(cameraSpeed_ + io.MouseWheel * 0.8f, 0.5f, 40.0f);
    }

    // 由 yaw/pitch 求前向；right = forward × 世界 up，保证横向移动始终水平、不随俯仰倾斜。
    const float yaw = glm::radians(cameraYawDegrees_);
    const float pitch = glm::radians(cameraPitchDegrees_);
    const glm::vec3 forward{std::cos(pitch) * std::cos(yaw), std::sin(pitch),
                            std::cos(pitch) * std::sin(yaw)};
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::vec3 up{0.0f, 1.0f, 0.0f};
    // 相机按键直接查询 ImGui IO，而不使用 InputSnapshot：这是编辑器自身的交互，
    // 不应混入每帧传给游戏脚本的输入快照。
    float forwardAxis = 0.0f;
    float rightAxis = 0.0f;
    float upAxis = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_W)) forwardAxis += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) forwardAxis -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) rightAxis += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) rightAxis -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_E)) upAxis += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) upAxis -= 1.0f;
    // 按住左 Shift 三倍速；乘 DeltaTime 使移动距离与帧率无关（每帧时间不同也匀速）。
    const float speed = cameraSpeed_ * (ImGui::IsKeyDown(ImGuiKey_LeftShift) ? 3.0f : 1.0f);
    cameraPosition_ += (forward * forwardAxis + right * rightAxis + up * upAxis) * speed *
                       static_cast<float>(io.DeltaTime);
}

// 拾取流程：把归一化鼠标坐标反投影成世界空间射线，再对每个可见网格的世界 AABB 求最近交点。
void Editor::pickEntity(World& world, float normalizedX, float normalizedY, float aspect) {
    // 逆变换 (投影 × 视图) 可把 NDC 空间中的点还原到世界空间；NDC 的 z 取 -1 和 +1 表示近/远平面。
    const auto inverseViewProjection = glm::inverse(projectionMatrix(aspect) * viewMatrix());
    const glm::vec4 nearClip = inverseViewProjection *
                               glm::vec4(normalizedX * 2.0f - 1.0f,
                                         1.0f - normalizedY * 2.0f, -1.0f, 1.0f);
    const glm::vec4 farClip = inverseViewProjection *
                              glm::vec4(normalizedX * 2.0f - 1.0f,
                                        1.0f - normalizedY * 2.0f, 1.0f, 1.0f);
    // 齐次坐标做透视除法（除以 w）后才得到三维世界坐标；两点连线的方向即射线方向。
    const glm::vec3 nearPoint = glm::vec3(nearClip) / nearClip.w;
    const glm::vec3 farPoint = glm::vec3(farClip) / farClip.w;
    const Ray ray{nearPoint, glm::normalize(farPoint - nearPoint)};

    Entity hitEntity{};
    float closest = std::numeric_limits<float>::max();
    // 先刷新世界矩阵：实体刚被移动/旋转时，缓存的 worldMatrix 可能仍是上一帧的值。
    updateWorldTransforms(world);
    for (const auto entity : world.entities()) {
        const auto* mesh = world.tryGet<MeshRenderer>(entity);
        const auto* transform = world.tryGet<Transform>(entity);
        // 不可见网格不参与拾取，保证“点到的”与“看到的”一致。
        if (!mesh || !transform || !mesh->visible) {
            continue;
        }
        // 渲染器尚未加载该网格时返回空 optional，这一帧直接跳过它。
        const auto bounds = renderer_.meshBounds(mesh->meshGuid);
        if (!bounds) {
            continue;
        }
        const auto [minimum, maximum] = transformedBounds(*bounds, transform->worldMatrix);
        // 只考虑“最近”的命中：多个 AABB 重叠时以射线距离最小的实体为选中项。
        float distance = 0.0f;
        if (intersectAabb(ray, minimum, maximum, distance) && distance < closest) {
            closest = distance;
            hitEntity = entity;
        }
    }
    // 没有任何命中时 hitEntity 是无效句柄，效果等同于点击空白处取消选中。
    selected_ = hitEntity;
}

} // namespace l3d
