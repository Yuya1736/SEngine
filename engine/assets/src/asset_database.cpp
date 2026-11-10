// engine/assets 模块：AssetDatabase 的实现——扫描资产目录、维护 GUID 索引与加载缓存。
// 分层位置：engine_app 组合根 → runtime 模块（renderer/editor）→ 本文件 → engine_core；
// 这里只做 CPU 侧的文件解析与缓存，不接触 GPU。
// 约束：资产身份由 GUID 决定而非路径；缺少元数据时会自动补写 .meta.json。

#include "l3d/assets/asset_database.h"

#include <algorithm>
#include <cctype>

#include <nlohmann/json.hpp>

#include "l3d/assets/loaders.h"
#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"
#include "l3d/core/uuid.h"

namespace l3d {

namespace {
using Json = nlohmann::json;

// 扩展名统一转小写，避免 ".PNG" 与 ".png" 被识别成不同类型。
std::string lowerExtension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

// 从 JSON 数组读取 glm 向量；字段缺失或维度不符时整体使用 fallback。
// 这样材质文件只写关心的字段也能加载，不必重复一遍默认值。
template <glm::length_t Count, typename T, glm::qualifier Qualifier>
glm::vec<Count, T, Qualifier> readVector(const Json& json, glm::vec<Count, T, Qualifier> fallback) {
    if (!json.is_array() || json.size() != Count) {
        return fallback;
    }
    glm::vec<Count, T, Qualifier> result;
    for (glm::length_t index = 0; index < Count; ++index) {
        result[index] = json[static_cast<std::size_t>(index)].get<T>();
    }
    return result;
}

} // namespace

// 扫描整个资产目录树，重建记录、GUID 索引与缓存；成功返回资产数量。
// 路径不存在或遍历出错时返回错误字符串；此时数据库可能停留在已清空的部分状态。
Result<int> AssetDatabase::scan(const std::filesystem::path& assetRoot) {
    if (!std::filesystem::exists(assetRoot) || !std::filesystem::is_directory(assetRoot)) {
        return Result<int>::failure("Asset root does not exist: " + pathToUtf8(assetRoot));
    }

    // 规范化为绝对路径：refresh() 直接用 root_ 重扫，不依赖调用方当初传入的写法。
    root_ = std::filesystem::weakly_canonical(assetRoot);
    // 清空旧索引和全部缓存，保证与新扫描结果一致；此前返回的指针从这一刻起失效。
    assets_.clear();
    guidIndex_.clear();
    meshCache_.clear();
    textureCache_.clear();
    materialCache_.clear();

    // 使用 error_code 版本的迭代器：遇到无权限目录等错误时通过 error 返回而非抛异常。
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_, error)) {
        if (error) {
            return Result<int>::failure("Failed while scanning assets: " + error.message());
        }
        // 跳过 .meta.json：它是资产的“身份证”旁车文件，不是资产内容本身。
        if (!entry.is_regular_file() || entry.path().filename().string().ends_with(".meta.json")) {
            continue;
        }
        // 受支持的文件在这里读取（或补生成）同名 .meta.json 取得 GUID；
        // GUID 可理解为资产的身份证号：场景/材质记录的是号，文件改名或挪目录都不会断引用。
        auto record = buildRecord(entry.path());
        if (!record) {
            L3D_WARN("Skipping asset '{}': {}", pathToUtf8(entry.path()), record.error());
            continue;
        }
        const auto& asset = record.value();
        // 防御性检查：buildRecord 已拒绝 Unknown，这里再兜一层，避免登记不可加载的文件。
        if (asset.type == AssetType::Unknown) {
            continue;
        }
        // 重复 GUID 只保留先扫到的一条：GUID 索引必须是一对一，否则引用会指向不确定的资产。
        if (guidIndex_.contains(asset.guid)) {
            L3D_WARN("Duplicate asset GUID '{}' for '{}'", asset.guid,
                     pathToUtf8(asset.path));
            continue;
        }
        // 暂存下标，排序后会重建，见下方。
        guidIndex_[asset.guid] = assets_.size();
        assets_.push_back(std::move(record.value()));
    }

    // 按路径排序，让 assets() 的顺序稳定、便于编辑器展示；排序会打乱下标，
    // 所以紧接着必须重建 GUID 索引。
    std::ranges::sort(assets_, [](const AssetRecord& left, const AssetRecord& right) {
        return left.path.generic_string() < right.path.generic_string();
    });
    // 重建索引，使下标重新对应排序后的位置。
    guidIndex_.clear();
    for (std::size_t index = 0; index < assets_.size(); ++index) {
        guidIndex_[assets_[index].guid] = index;
    }

    L3D_INFO("Asset database scanned {} assets under '{}'", assets_.size(),
             pathToUtf8(root_));
    // 返回数量而非布尔值，便于启动日志、编辑器状态栏与测试断言使用。
    return Result<int>::success(static_cast<int>(assets_.size()));
}

// 供编辑器“重新导入”等场景手动触发；失败只记日志，调用方无法感知具体错误。
void AssetDatabase::refresh() {
    if (!root_.empty()) {
        const auto result = scan(root_);
        if (!result) {
            L3D_ERROR("Asset refresh failed: {}", result.error());
        }
    }
}

// 只查内存索引，不访问磁盘；返回 nullptr 表示数据库中没有这个 GUID。
const AssetRecord* AssetDatabase::findByGuid(std::string_view guid) const {
    const auto position = guidIndex_.find(std::string(guid));
    if (position == guidIndex_.end()) {
        return nullptr;
    }
    return &assets_[position->second];
}

// 用 equivalent 而不是字符串比较：兼容大小写、符号链接以及 "."/".." 等不同写法。
const AssetRecord* AssetDatabase::findByPath(const std::filesystem::path& path) const {
    const auto normalized = std::filesystem::weakly_canonical(path);
    const auto position = std::ranges::find_if(assets_, [&](const AssetRecord& asset) {
        std::error_code error;
        return std::filesystem::equivalent(asset.path, normalized, error) && !error;
    });
    return position == assets_.end() ? nullptr : &*position;
}

// 线性过滤即可（资产数量通常不大）；返回的指针指向 assets_ 内部元素。
std::vector<const AssetRecord*> AssetDatabase::byType(AssetType type) const {
    std::vector<const AssetRecord*> result;
    for (const auto& asset : assets_) {
        if (asset.type == type) {
            result.push_back(&asset);
        }
    }
    return result;
}

// 先查缓存，未命中再解析文件；缓存以 GUID 为键，资产移动后仍然有效。
Result<std::shared_ptr<MeshData>> AssetDatabase::loadMesh(std::string_view guid) {
    const std::string key(guid);
    // 多个场景对象可能引用同一网格，缓存避免重复读取与重复分配。
    if (const auto cache = meshCache_.find(key); cache != meshCache_.end()) {
        return Result<std::shared_ptr<MeshData>>::success(cache->second);
    }
    const auto* record = findByGuid(key);
    // 类型不符也按“不存在”处理，防止把材质或纹理的 GUID 当成网格加载。
    if (!record || record->type != AssetType::Mesh) {
        return Result<std::shared_ptr<MeshData>>::failure("Mesh asset not found: " + key);
    }
    auto mesh = loadObjMesh(record->path);
    if (!mesh) {
        return Result<std::shared_ptr<MeshData>>::failure(mesh.error());
    }
    // 解析成功后放入缓存；shared_ptr 让缓存与调用方共享同一份数据，无需拷贝。
    auto shared = std::make_shared<MeshData>(std::move(mesh.value()));
    meshCache_[key] = shared;
    return Result<std::shared_ptr<MeshData>>::success(std::move(shared));
}

// 策略与 loadMesh 相同；解码失败原样返回错误，由渲染器决定是否回退成白纹理。
Result<std::shared_ptr<TextureData>> AssetDatabase::loadTexture(std::string_view guid) {
    const std::string key(guid);
    // 纹理解码较贵，多个材质共用同一张图时只解码一次。
    if (const auto cache = textureCache_.find(key); cache != textureCache_.end()) {
        return Result<std::shared_ptr<TextureData>>::success(cache->second);
    }
    const auto* record = findByGuid(key);
    // 与 loadMesh 一致：类型不符按不存在处理，避免加载到错误的数据。
    if (!record || record->type != AssetType::Texture) {
        return Result<std::shared_ptr<TextureData>>::failure("Texture asset not found: " + key);
    }
    auto texture = loadPngTexture(record->path);
    if (!texture) {
        return Result<std::shared_ptr<TextureData>>::failure(texture.error());
    }
    auto shared = std::make_shared<TextureData>(std::move(texture.value()));
    textureCache_[key] = shared;
    return Result<std::shared_ptr<TextureData>>::success(std::move(shared));
}

// 内置默认材质不落盘、不参与扫描，直接构造一份默认值，作为上层加载失败时的兜底。
// 其余材质从 JSON 文件读取并缓存；任何解析异常都转成 Result 错误。
Result<MaterialAsset> AssetDatabase::loadMaterial(std::string_view guid) {
    const std::string key(guid);
    if (key == BuiltinDefaultMaterialGuid) {
        return Result<MaterialAsset>::success(MaterialAsset{});
    }
    if (const auto cache = materialCache_.find(key); cache != materialCache_.end()) {
        return Result<MaterialAsset>::success(cache->second);
    }
    const auto* record = findByGuid(key);
    if (!record || record->type != AssetType::Material) {
        return Result<MaterialAsset>::failure("Material asset not found: " + key);
    }
    // 材质是小体积 JSON，直接整份读入再解析，不需要流式处理。
    auto text = readTextFile(record->path);
    if (!text) {
        return Result<MaterialAsset>::failure("Cannot read material: " + pathToUtf8(record->path));
    }
    // 逐字段读取；缺省值来自 MaterialAsset 的成员初始化，因此材质文件可以只写要改的字段。
    try {
        const auto json = Json::parse(*text);
        MaterialAsset material;
        material.name = json.value("name", record->path.stem().string());
        if (json.contains("baseColor")) {
            material.baseColor = readVector(json["baseColor"], material.baseColor);
        }
        if (json.contains("specularColor")) {
            material.specularColor = readVector(json["specularColor"], material.specularColor);
        }
        material.shininess = json.value("shininess", 32.0f);
        material.albedoTexGuid = json.value("albedoTexture", std::string{});
        // 材质是值类型，直接拷贝进缓存，调用方拿到的是独立副本。
        materialCache_[key] = material;
        return Result<MaterialAsset>::success(material);
    // JSON 语法错误等异常在这里统一转成 Result 错误，维持“接口不抛异常”的约定。
    } catch (const std::exception& exception) {
        return Result<MaterialAsset>::failure("Invalid material '" + pathToUtf8(record->path) +
                                              "': " + exception.what());
    }
}

// 先判断复合后缀：path.extension() 对 "foo.mat.json" 只会得到 ".json"。
// 已知扩展名映射到类型，其余返回 Unknown（扫描时会被跳过）。
AssetType AssetDatabase::inferType(const std::filesystem::path& path) {
    const auto filename = path.filename().string();
    // 扩展名已统一转小写，下面只比较小写后缀即可。
    const auto extension = lowerExtension(path);
    if (filename.ends_with(".mat.json")) {
        return AssetType::Material;
    }
    if (filename.ends_with(".scene.json")) {
        return AssetType::Scene;
    }
    if (extension == ".obj") {
        return AssetType::Mesh;
    }
    if (extension == ".png") {
        return AssetType::Texture;
    }
    if (extension == ".vert" || extension == ".frag") {
        return AssetType::Shader;
    }
    return AssetType::Unknown;
}

// 与 AssetType 一一对应，用于写入 .meta.json 的 type 字段以及日志。
std::string_view AssetDatabase::typeName(AssetType type) {
    switch (type) {
    case AssetType::Mesh:
        return "Mesh";
    case AssetType::Texture:
        return "Texture";
    case AssetType::Material:
        return "Material";
    case AssetType::Scene:
        return "Scene";
    case AssetType::Shader:
        return "Shader";
    case AssetType::Unknown:
        return "Unknown";
    }
    return "Unknown";
}

// 读取旁车元数据 "<资产名>.meta.json"；不存在则生成新 GUID 并原子写回。
// 返回 failure 表示该文件无法登记（读写失败、JSON 损坏或 GUID 非法）。
Result<AssetRecord> AssetDatabase::buildRecord(const std::filesystem::path& path) {
    const auto type = inferType(path);
    if (type == AssetType::Unknown) {
        return Result<AssetRecord>::failure("Unsupported asset type");
    }

    // 元数据文件名直接在资产名后追加，例如 foo.obj 对应 foo.obj.meta.json。
    const auto metaPath = std::filesystem::path(path.string() + ".meta.json");
    std::string guid;
    // 已有元数据就沿用其中的 GUID：这是资产在多次启动、换机器后仍能被引用的关键。
    if (std::filesystem::exists(metaPath)) {
        const auto text = readTextFile(metaPath);
        if (!text) {
            return Result<AssetRecord>::failure("Cannot read metadata: " + pathToUtf8(metaPath));
        }
        try {
            const auto json = Json::parse(*text);
            guid = json.value("guid", std::string{});
        } catch (const std::exception& exception) {
            return Result<AssetRecord>::failure("Invalid metadata JSON: " +
                                                std::string(exception.what()));
        }
        // 非法 GUID 直接拒绝，避免把脏数据带进索引、让后续引用无法解析。
        if (!isValidUuid(guid)) {
            return Result<AssetRecord>::failure("Metadata contains an invalid GUID");
        }
    } else {
        // 首次发现该文件：生成随机 UUID 作为身份，并记录类型与导入器版本。
        guid = generateUuid();
        // type 与 importerVersion 便于人工排查，也为将来升级导入器留出判断依据。
        const Json metadata{{"guid", guid}, {"type", std::string(typeName(type))},
                            {"importerVersion", 1}};
        // 原子写：先写临时文件再改名，进程崩溃时不会留下半截 JSON。
        if (!writeTextFileAtomic(metaPath, metadata.dump(2) + "\n")) {
            return Result<AssetRecord>::failure("Cannot create metadata: " + pathToUtf8(metaPath));
        }
        L3D_INFO("Generated metadata for '{}': {}", pathToUtf8(path), guid);
    }

    return Result<AssetRecord>::success(AssetRecord{guid, path, type});
}

} // namespace l3d
