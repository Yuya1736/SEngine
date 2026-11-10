# engine_scene：ECS（Entity/World/组件）、层级变换、场景 JSON、Play 快照。
# 依赖 core 与 assets（场景里用 GUID 引用资产）。
l3d_engine_library(engine_scene
    src/world.cpp
    src/transform_system.cpp
    src/scene_serializer.cpp
    src/scene_utils.cpp
    src/editor_session.cpp
)
target_link_libraries(engine_scene PUBLIC engine_core engine_assets nlohmann_json::nlohmann_json)
