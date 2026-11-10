#pragma once

// engine/renderer 模块：渲染器抽象接口与 OpenGL 4.1 实现声明。
// 分层位置：engine_app（组合根）→ runtime 模块 engine_renderer → engine_core；
// 上层代码只依赖 IRenderer，不感知具体图形 API。
// 关键约束：GL 上下文由引擎/窗口层创建，所有 GL 调用都发生在该上下文有效的线程上。
#include <cstdint>
#include <memory>
#include <optional>

#include "l3d/renderer/render_types.h"

namespace l3d {

class IAssetDatabase;

// 渲染器抽象：把“渲染一帧”与具体图形 API 隔离，方便替换实现或写假实现做测试。
// 除 viewportTexture 外，其余方法都可能触发 GL 调用（meshBounds 会按需上传网格），
// 因此都假定调用线程持有当前 GL 上下文。
class IRenderer {
public:
    virtual ~IRenderer() = default;
    // 加载着色器、创建 1x1 白纹理等资源；需要在 GL 上下文就绪后调用，失败返回 false。
    virtual bool initialize(IAssetDatabase& assets) = 0;
    // 释放所有 GPU 资源；可重复调用（幂等）。
    virtual void shutdown() = 0;
    // 渲染一帧到内部离屏 FBO，不会直接显示到窗口。
    virtual void render(const RenderFrame& frame, int width, int height) = 0;
    // 把离屏 FBO 的彩色结果缩放拷贝到默认帧缓冲，即真正显示到窗口。
    virtual void presentToDefaultFramebuffer(int width, int height) = 0;
    // 返回离屏彩色纹理的 GL 句柄，供 ImGui 等以贴图形式显示视口。
    virtual std::uint32_t viewportTexture() const = 0;
    // 查询网格局部包围盒；网格不存在或加载失败时返回 nullopt。
    virtual std::optional<MeshBounds> meshBounds(const std::string& meshGuid) = 0;
};

// OpenGL 4.1 Core Profile 前向渲染器。
// 使用 PIMPL：GL 相关细节全部藏在 Impl 中，头文件不暴露 GL 类型，减少上层重编译。
// 不变量：GL 资源唯一所有权归 Impl，禁止拷贝，只能移动（由 unique_ptr 保证）。
class OpenGLRenderer final : public IRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer() override;

    OpenGLRenderer(const OpenGLRenderer&) = delete;
    OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;

    bool initialize(IAssetDatabase& assets) override;
    void shutdown() override;
    void render(const RenderFrame& frame, int width, int height) override;
    void presentToDefaultFramebuffer(int width, int height) override;
    std::uint32_t viewportTexture() const override;
    std::optional<MeshBounds> meshBounds(const std::string& meshGuid) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace l3d
