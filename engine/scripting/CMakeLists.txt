# engine_scripting：C ABI 宿主，负责加载 GameScripts.dll 并驱动脚本生命周期。
# 依赖 scene/physics 只是为了通过函数表把能力暴露给 C#，C# 侧不引用任何 C++ 头文件。
l3d_engine_library(engine_scripting
    src/script_host.cpp
)
target_link_libraries(engine_scripting PUBLIC engine_core engine_scene engine_physics)
target_include_directories(engine_scripting PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
