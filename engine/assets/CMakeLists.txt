# engine_assets：GUID 资产库、OBJ/PNG/材质加载。
# 依赖 core（日志/Result）与 nlohmann/json（.meta.json/.mat.json）。
l3d_engine_library(engine_assets
    src/asset_database.cpp
    src/mesh_loader.cpp
    src/texture_loader.cpp
    src/stb_image_impl.cpp
)
target_include_directories(engine_assets PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/stb
)
target_link_libraries(engine_assets PUBLIC engine_core nlohmann_json::nlohmann_json)
