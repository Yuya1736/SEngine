# engine_core：最底层基础设施（Result/日志/事件/时间/输入/GUID/文件）。
# 不依赖任何其他 engine 模块，所有上层模块都可以依赖它。
l3d_engine_library(engine_core
    src/logging.cpp
    src/fixed_timestep.cpp
    src/input.cpp
    src/uuid.cpp
    src/file_utils.cpp
)
target_compile_features(engine_core PUBLIC cxx_std_20)
target_link_libraries(engine_core PUBLIC glm::glm)
