# engine_platform：GLFW 窗口、OpenGL 上下文与输入。
# 只依赖 core；渲染模块依赖它，但它不反向依赖渲染。
l3d_engine_library(engine_platform
    src/window.cpp
)
target_link_libraries(engine_platform PUBLIC engine_core glfw glad)
target_include_directories(engine_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/include)
