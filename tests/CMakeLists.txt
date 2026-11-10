# 测试构建配置：把单元测试与脚本集成测试编成一个可执行程序，并生成测试用假插件。
# 测试资产根目录通过编译定义注入；catch_discover_tests 让 CTest 逐条注册用例，可用 -R 过滤。
# 两个测试源文件共用一个可执行程序，共享 Catch2WithMain 提供的 main()。
add_executable(engine_tests
    engine_tests.cpp
    script_integration_tests.cpp
)
target_include_directories(engine_tests PRIVATE "${CMAKE_SOURCE_DIR}/engine")
target_link_libraries(engine_tests PRIVATE
    Catch2::Catch2WithMain
    engine_core
    engine_platform
    engine_assets
    engine_scene
    engine_physics
    engine_renderer
    engine_scripting
    glad
    OpenGL::GL
)
# 把资产根目录以宏的形式注入，测试代码据此定位 assets/，不依赖运行目录的相对路径。
target_compile_definitions(engine_tests PRIVATE
    L3D_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets"
)
l3d_set_warnings(engine_tests)

# 构建“假脚本插件”动态库：导出符号齐全但 ABI 版本不匹配，专门用于加载失败路径的测试。
add_library(fake_script_plugin SHARED fake_script_plugin.cpp)
target_compile_features(fake_script_plugin PRIVATE cxx_std_20)
l3d_set_warnings(fake_script_plugin)

# 集成测试在运行时加载 C# 脚本插件，因此先构建 game_scripts 目标（若存在）。
if(TARGET game_scripts)
    add_dependencies(engine_tests game_scripts)
endif()
add_dependencies(engine_tests fake_script_plugin)

# 启用 Catch2 的 CTest 集成：构建后自动发现 TEST_CASE，并让 ctest 逐条运行。
# 工作目录设在 bin 输出目录，测试才能按相对名找到插件动态库。
include(Catch)
catch_discover_tests(engine_tests
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
)
