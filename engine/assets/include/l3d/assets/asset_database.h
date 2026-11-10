#pragma once

// engine/assets 模块：资产数据库的接口与默认实现声明。
// 分层位置：engine_app 组合根 → 各 runtime 模块（renderer/editor 等）→ engine_core；
// 本文件只依赖 engine_core 与 asset_types，向上提供“按 GUID 取资产”的统一入口。
// 约束：assets_ 中元素的指针在下次 scan()/refresh() 之前有效，调用方不要跨扫描保存。

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "l3d/assets/asset_types.h"
#include "l3d/core/result.h"

namespace l3d {

// 资产类型；由文件扩展名推断（见 inferType），Unknown 表示扫描时直接忽略的格式。
enum class AssetType { Unknown, Mesh, Texture, Material, Scene, Shader };

// 一条资产登记项。不变量：guid 合法且全局唯一，path 指向实际存在的文件。
// GUID 与路径的分工：GUID 是稳定身份，路径只是“此刻在哪”，资产移动后 GUID 不变。
struct AssetRecord {
    std::string guid;
    std::filesystem::path path;
    AssetType type = AssetType::Unknown;
};

// 资产数据库接口：上层（渲染器、编辑器）只依赖它，便于替换实现或做测试替身。
class IAssetDatabase {
public:
    virtual ~IAssetDatabase() = default;

    // 递归扫描 assetRoot，重建记录与 GUID 索引并清空缓存；返回资产数量。
    [[nodiscard]] virtual Result<int> scan(const std::filesystem::path& assetRoot) = 0;
    // 重新扫描上一次的根目录；失败只记日志，不向调用方抛异常。
    virtual void refresh() = 0;
    [[nodiscard]] virtual const std::filesystem::path& root() const noexcept = 0;
    [[nodiscard]] virtual const std::vector<AssetRecord>& assets() const noexcept = 0;
    // 找不到时返回 nullptr，而不是错误；用于探测资产是否存在。
    [[nodiscard]] virtual const AssetRecord* findByGuid(std::string_view guid) const = 0;
    [[nodiscard]] virtual const AssetRecord* findByPath(const std::filesystem::path& path) const = 0;
    // 返回该类型的全部记录，按路径排序，便于编辑器做资产浏览器。
    [[nodiscard]] virtual std::vector<const AssetRecord*> byType(AssetType type) const = 0;
    // 按 GUID 加载网格，带进程内缓存；GUID 不存在、类型不符或文件损坏时返回错误。
    [[nodiscard]] virtual Result<std::shared_ptr<MeshData>> loadMesh(std::string_view guid) = 0;
    // 按 GUID 加载纹理，带缓存；失败由调用方决定回退策略（渲染器会换成白纹理）。
    [[nodiscard]] virtual Result<std::shared_ptr<TextureData>> loadTexture(
        std::string_view guid) = 0;
    // 按 GUID 加载材质；内置默认材质不查表，直接返回一份默认值。
    [[nodiscard]] virtual Result<MaterialAsset> loadMaterial(std::string_view guid) = 0;
};

// 默认实现：扫描磁盘、维护 GUID→记录索引，并缓存已解析的网格/纹理/材质。
class AssetDatabase final : public IAssetDatabase {
public:
    [[nodiscard]] Result<int> scan(const std::filesystem::path& assetRoot) override;
    void refresh() override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept override { return root_; }
    [[nodiscard]] const std::vector<AssetRecord>& assets() const noexcept override {
        return assets_;
    }
    [[nodiscard]] const AssetRecord* findByGuid(std::string_view guid) const override;
    [[nodiscard]] const AssetRecord* findByPath(const std::filesystem::path& path) const override;
    [[nodiscard]] std::vector<const AssetRecord*> byType(AssetType type) const override;

    [[nodiscard]] Result<std::shared_ptr<MeshData>> loadMesh(std::string_view guid) override;
    [[nodiscard]] Result<std::shared_ptr<TextureData>> loadTexture(
        std::string_view guid) override;
    [[nodiscard]] Result<MaterialAsset> loadMaterial(std::string_view guid) override;

    // 由扩展名推断资产类型；".mat.json"/".scene.json" 这类复合后缀必须整体判断。
    [[nodiscard]] static AssetType inferType(const std::filesystem::path& path);
    // 类型名的可读字符串，用于写入 .meta.json 与日志。
    [[nodiscard]] static std::string_view typeName(AssetType type);

private:
    // 读取或创建旁车元数据，构造一条记录；失败时返回错误而不产生半成品。
    [[nodiscard]] Result<AssetRecord> buildRecord(const std::filesystem::path& path);

    std::filesystem::path root_;
    std::vector<AssetRecord> assets_; // 扫描结果，已按路径排序
    std::unordered_map<std::string, std::size_t> guidIndex_; // GUID → assets_ 下标
    // 进程内加载缓存：同一 GUID 只解析一次文件；三种资产值语义不同，故分开存放。
    std::unordered_map<std::string, std::shared_ptr<MeshData>> meshCache_;
    std::unordered_map<std::string, std::shared_ptr<TextureData>> textureCache_;
    std::unordered_map<std::string, MaterialAsset> materialCache_;
};

} // namespace l3d
