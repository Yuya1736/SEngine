# engine_physics：AABB/球体碰撞、触发器、射线、运动学角色控制。
# 只依赖 core 与 scene，不接触渲染或脚本。
l3d_engine_library(engine_physics
    src/physics_world.cpp
)
target_link_libraries(engine_physics PUBLIC engine_core engine_scene)
