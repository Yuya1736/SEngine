# L3D Engine 顶层构建脚本。
# 职责：统一 C++ 标准/输出目录、拉取第三方依赖、定义模块帮助函数、按分层顺序加入子目录。
# 模块依赖规则靠各子目录的 target_link_libraries 表达，只有 app 能链接全部模块。

cmake_minimum_required(VERSION 3.25)

project(L3DEngine VERSION 0.1.0 LANGUAGES CXX C)

# 全项目统一使用 C++20，禁用编译器扩展以便跨编译器行为一致。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 所有可执行文件/测试输出到 build/bin/<Config>，资产也会复制到该目录旁。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${config}" config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/bin/${config}")
endforeach()

include(FetchContent)
find_package(OpenGL REQUIRED)

# ---- 第三方依赖 ----
# 全部用 FetchContent 固定 tag 下载：首次配置需要网络，之后复用缓存。

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glfw)

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm)

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b-docking
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(
    catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(catch2)

# ---- 本地第三方源码 ----
# GLAD 提供 OpenGL 4.1 函数指针加载；Windows 只自带 GL 1.1，必须由它补齐。
add_library(glad STATIC third_party/glad/src/gl.c)
target_include_directories(glad PUBLIC third_party/glad/include)

# Dear ImGui 以源码形式静态编译，包含 GLFW 与 OpenGL3 两个后端。
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw glad)

# ---- 项目辅助函数 ----
# 统一警告级别；/utf-8 保证源码里的中文字符串/注释按 UTF-8 解析。
function(l3d_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# 每个 engine_xxx 模块都是一个静态库，include 目录固定为 engine/<模块名>/include。
# 这样模块内的头文件可以用 "l3d/<模块>/xxx.h" 的稳定路径互相引用。
function(l3d_engine_library target)
    add_library(${target} STATIC ${ARGN})
    string(REPLACE "engine_" "" module_name "${target}")
    target_include_directories(${target} PUBLIC
        "${CMAKE_SOURCE_DIR}/engine/${module_name}/include")
    l3d_set_warnings(${target})
endfunction()

# 加入顺序大致对应依赖方向：core 最底层，app 最后。
add_subdirectory(engine/core)
add_subdirectory(engine/platform)
add_subdirectory(engine/assets)
add_subdirectory(engine/scene)
add_subdirectory(engine/physics)
add_subdirectory(engine/renderer)
add_subdirectory(engine/scripting)
add_subdirectory(engine/editor)
add_subdirectory(scripts) # C# 脚本插件（可选，需要 .NET SDK）
add_subdirectory(app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
