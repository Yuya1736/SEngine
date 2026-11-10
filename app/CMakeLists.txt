# engine_app = 组合根可执行文件：唯一同时链接全部运行时模块的目标。
# 分层架构中“只有 app 知道具体子系统”的约束，在构建层面就体现为这张链接列表。
add_executable(engine_app
    src/main.cpp
    src/application.cpp
    src/screenshot.cpp
)
target_include_directories(engine_app PRIVATE
    src
    ${CMAKE_SOURCE_DIR}/third_party/stb
)
target_link_libraries(engine_app PRIVATE
    engine_core
    engine_platform
    engine_assets
    engine_scene
    engine_physics
    engine_renderer
    engine_scripting
    engine_editor
    imgui
    glad
    OpenGL::GL
)
l3d_set_warnings(engine_app)
target_compile_definitions(engine_app PRIVATE
    # 源码树里的资产目录：开发时优先于“可执行文件旁的 assets”使用。
    L3D_SOURCE_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets"
    _CRT_SECURE_NO_WARNINGS
)

# 构建后把 assets 复制到 exe 旁边，保证直接运行产物时也能找到资源。
add_custom_command(TARGET engine_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:engine_app>/assets"
    COMMENT "Copying assets beside engine_app"
)

# C# 脚本插件是可选的：没有 .NET SDK 时游戏仍可运行，只是缺少脚本行为。
if(TARGET game_scripts)
    add_dependencies(engine_app game_scripts)
endif()
