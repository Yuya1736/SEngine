#pragma once

// 层级变换系统：把每个实体的局部 Transform 合成为世界矩阵。
// 分层位置：engine_app、渲染和物理在需要最新世界矩阵时调用本模块。
// 约束：worldMatrix 是输出缓存；本模块不改变 position/rotation/scale。
#include <glm/mat4x4.hpp>

namespace l3d {

class World;
struct Transform;

// 生成局部矩阵：平移 * 旋转 * 缩放，按列向量约定先缩放、再旋转、最后平移。
[[nodiscard]] glm::mat4 localMatrix(const Transform& transform);
// 解析全部父子链，把合成结果写回 Transform::worldMatrix；发现环时告警并停止回溯。
void updateWorldTransforms(World& world);

} // namespace l3d
