# engine_renderer：渲染提取（ECS -> RenderFrame）+ OpenGL 前向渲染器。
# 依赖 platform 获取 GL 上下文，依赖 assets 取网格/贴图，依赖 scene 读取组件。
l3d_engine_library(engine_renderer
    src/renderer.cpp
    src/render_extraction.cpp
)
target_link_libraries(engine_renderer PUBLIC
    engine_core
    engine_platform
    engine_assets
    engine_scene
    glad
    OpenGL::GL
)
