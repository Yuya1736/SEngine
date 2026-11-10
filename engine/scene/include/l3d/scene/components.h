#pragma once

// 场景数据组件集合：engine_scene 模块中可挂到实体上的纯数据定义。
// 分层位置：engine_app 作为组合根装配各 runtime 模块，本模块位于 engine_core 之上，
// 向渲染、物理、脚本等模块提供共享的场景数据。
// 约束：组件只保存值语义数据，不拥有实体生命周期，也不保存运行期裸指针。
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "l3d/scene/entity.h"

namespace l3d {

// 实体本身只是编号；组件是挂在该编号上的数据，按类型存入 World 的组件池。
// 实体的可读名称，供编辑器层级和日志显示；空名称会由 displayName 回退生成。
struct NameComponent { std::string value = "Entity"; };

// 实体的局部变换：位置、旋转、缩放都相对父实体；worldMatrix 是派生缓存。
// 不变量：worldMatrix 由 updateWorldTransforms 写入，不能把它当作变换输入。
struct Transform {
    glm::vec3 position{0.0f}; // 相对父实体的局部位置
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // 单位四元数表示无旋转
    glm::vec3 scale{1.0f}; // 各轴局部缩放，1 表示原始大小
    glm::mat4 worldMatrix{1.0f}; // 世界矩阵缓存，仅由变换系统更新
};

// 父子关系：parent 句柄无效时表示根节点；父实体不拥有子实体的生命周期。
struct Hierarchy { Entity parent{}; };

// 透视相机参数；primary 为 true 时运行时会优先选它作为主相机。
struct CameraComponent {
    float fieldOfViewDegrees = 60.0f;
    float nearClip = 0.1f;
    float farClip = 500.0f;
    bool primary = true;
};

// 渲染引用：GUID 字符串交给资产数据库解析，场景文件因此不保存内存地址。
struct MeshRenderer {
    std::string meshGuid = "builtin:cube";
    std::string materialGuid = "builtin:default-material";
    bool visible = true;
};

// 全局方向光：direction 是光线传播方向，通常需要保持归一化。
struct DirectionalLight {
    glm::vec3 direction{-0.45f, -1.0f, -0.3f};
    glm::vec3 color{1.0f, 0.96f, 0.9f};
    float intensity = 1.2f;
};

// 点光源：range 限制影响距离，光源位置来自同一实体上的 Transform。
struct PointLight {
    glm::vec3 color{1.0f, 0.65f, 0.3f};
    float intensity = 2.5f;
    float range = 12.0f;
};

// 盒碰撞体：halfExtents 是各轴的一半尺寸；trigger 只检测重叠，不阻挡运动。
struct BoxCollider {
    glm::vec3 halfExtents{0.5f};
    bool trigger = false;
};

// 球碰撞体：radius 为局部半径；trigger 语义与盒碰撞体相同。
struct SphereCollider {
    float radius = 0.5f;
    bool trigger = false;
};

// 运动学角色参数；velocity 和 grounded 是运行时状态，通常不写入场景文件。
struct CharacterController {
    glm::vec3 halfExtents{0.4f, 0.9f, 0.4f};
    glm::vec3 velocity{0.0f};
    float gravity = -26.0f;
    bool grounded = false;
};

// 脚本实例描述：scriptName 对应插件脚本类型，propertiesJson 是持久化参数字符串。
struct ScriptComponent {
    std::string scriptName;
    std::string propertiesJson = "{}";
};

// 任意字符串标签，用于按名字查找实体，例如玩家出生点或关卡入口。
struct TagComponent { std::string value; };

} // namespace l3d
