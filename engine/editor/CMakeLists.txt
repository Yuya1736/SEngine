# engine_editor：基于 Dear ImGui 的编辑器面板与 Play 模式控制。
# 它可以依赖所有运行时模块，但运行时模块绝不能反过来依赖它（反向依赖禁令）。
l3d_engine_library(engine_editor
    src/editor.cpp
)
target_link_libraries(engine_editor PUBLIC
    engine_core
    engine_platform
    engine_assets
    engine_scene
    engine_physics
    engine_scripting
    engine_renderer
    imgui
)
