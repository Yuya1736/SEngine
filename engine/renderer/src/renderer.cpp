// engine/renderer 模块：OpenGL 4.1 Core Profile 前向渲染器实现。
// 分层位置：engine_app（组合根，持有窗口与 GL 上下文）→ runtime 模块 engine_renderer
// → engine_core；输入是 render_extraction 产出的 RenderFrame 纯数据。
// 关键约束：VAO/VBO/EBO/纹理/着色器程序/FBO 全部由 Impl 创建与销毁，
// 且必须在同一个有效 GL 上下文所在的线程内调用 initialize/render/shutdown。
#include "l3d/renderer/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <unordered_map>

#include <glad/gl.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "l3d/assets/asset_database.h"
#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"

namespace l3d {

namespace {

// 内置后备顶点着色器：当 assets/shaders/lit.vert 缺失时使用，保证资源不完整也能出图。
// 属性位置 0/1/2 依次是位置、法线、UV，必须与 MeshVertex 字段顺序以及
// uploadMesh 中的 glVertexAttribPointer 调用对应。
constexpr const char* LitVertexFallback = R"GLSL(
#version 410 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat3 uNormalMatrix;
out vec3 vWorldPosition;
out vec3 vNormal;
out vec2 vUv;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPosition = world.xyz;
    vNormal = normalize(uNormalMatrix * aNormal);
    vUv = aUv;
    gl_Position = uProjection * uView * world;
}
)GLSL";

// 内置后备片元着色器，实现 Blinn-Phong 前向光照：
// 漫反射用 dot(N, L)，高光用半程向量 H = normalize(L + V) 的 dot(N, H)^shininess，
// 比原始 Phong 少算一次反射向量。点光源按距离平方衰减，超出 range 的片元直接跳过。
// 末尾 pow(color, 1/2.2) 是 gamma 校正：光照在线性空间计算，显示前再编码为 sRGB。
// 注意：着色器里 uPointLights[8] 的大小必须与 RenderFrame::pointLights、Uniforms 保持一致。
constexpr const char* LitFragmentFallback = R"GLSL(
#version 410 core
in vec3 vWorldPosition;
in vec3 vNormal;
in vec2 vUv;
out vec4 FragColor;
uniform sampler2D uAlbedo;
uniform int uHasTexture;
uniform vec4 uBaseColor;
uniform vec3 uSpecularColor;
uniform float uShininess;
uniform vec3 uCameraPosition;
uniform vec3 uDirectionalDirection;
uniform vec3 uDirectionalColor;
uniform float uDirectionalIntensity;
uniform int uPointLightCount;
struct PointLight { vec3 position; vec3 color; float intensity; float range; };
uniform PointLight uPointLights[8];

void main() {
    vec3 albedo = uBaseColor.rgb;
    if (uHasTexture != 0) {
        albedo *= texture(uAlbedo, vUv).rgb;
    }
    vec3 normal = normalize(vNormal);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    vec3 color = albedo * 0.16;

    vec3 lightDirection = normalize(-uDirectionalDirection);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    vec3 halfway = normalize(lightDirection + viewDirection);
    float specular = pow(max(dot(normal, halfway), 0.0), max(uShininess, 1.0));
    color += uDirectionalColor * uDirectionalIntensity *
             (albedo * diffuse + uSpecularColor * specular);

    for (int index = 0; index < uPointLightCount; ++index) {
        vec3 delta = uPointLights[index].position - vWorldPosition;
        float distance = length(delta);
        if (distance > uPointLights[index].range || distance < 0.0001) {
            continue;
        }
        vec3 direction = delta / distance;
        float attenuation = 1.0 - clamp(distance / uPointLights[index].range, 0.0, 1.0);
        attenuation *= attenuation;
        float pointDiffuse = max(dot(normal, direction), 0.0);
        vec3 pointHalf = normalize(direction + viewDirection);
        float pointSpecular = pow(max(dot(normal, pointHalf), 0.0), max(uShininess, 1.0));
        color += uPointLights[index].color * uPointLights[index].intensity * attenuation *
                 (albedo * pointDiffuse + uSpecularColor * pointSpecular);
    }

    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(color, uBaseColor.a);
}
)GLSL";

// 一个网格在 GPU 上的“三件套”：VAO 记录属性如何取，VBO 存顶点数据，EBO 存三角形索引。
// indexCount 是 glDrawElements 要绘制的索引数量；bounds 是 CPU 侧局部空间包围盒。
struct GpuMesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
    MeshBounds bounds;
};

// 生成边长 1、中心在原点的立方体。每个面用独立的 4 个顶点是刻意为之：
// 立方体相邻面法线不同，若共享顶点，光照会在棱边被插值“抹圆”，像软糖一样。
// 顶点顺序保证从外部看是逆时针（正面），与常见右手系约定一致。
MeshData makeCube() {
    MeshData mesh;
    const std::array<glm::vec3, 6> normals = {
        glm::vec3{0, 0, 1}, glm::vec3{0, 0, -1}, glm::vec3{1, 0, 0},
        glm::vec3{-1, 0, 0}, glm::vec3{0, 1, 0}, glm::vec3{0, -1, 0}};
    const std::array<glm::vec3, 6> rights = {
        glm::vec3{1, 0, 0}, glm::vec3{-1, 0, 0}, glm::vec3{0, 0, -1},
        glm::vec3{0, 0, 1}, glm::vec3{1, 0, 0}, glm::vec3{1, 0, 0}};
    const std::array<glm::vec3, 6> ups = {
        glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0}, glm::vec3{0, 1, 0},
        glm::vec3{0, 1, 0}, glm::vec3{0, 0, -1}, glm::vec3{0, 0, 1}};
    const std::array<glm::vec2, 4> uvs = {glm::vec2{0, 0}, glm::vec2{1, 0},
                                           glm::vec2{1, 1}, glm::vec2{0, 1}};
    const std::array<glm::vec2, 4> corners = {glm::vec2{-1, -1}, glm::vec2{1, -1},
                                               glm::vec2{1, 1}, glm::vec2{-1, 1}};
    for (std::size_t face = 0; face < normals.size(); ++face) {
        const glm::vec3 center = normals[face] * 0.5f;
        for (std::size_t corner = 0; corner < corners.size(); ++corner) {
            const glm::vec3 position = center + rights[face] * (corners[corner].x * 0.5f) +
                                       ups[face] * (corners[corner].y * 0.5f);
            mesh.vertices.push_back({position, normals[face], uvs[corner]});
        }
        const auto base = static_cast<std::uint32_t>(face * 4);
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    mesh.boundsMin = {-0.5f, -0.5f, -0.5f};
    mesh.boundsMax = {0.5f, 0.5f, 0.5f};
    return mesh;
}

// 生成 XZ 平面上边长 1、法线朝 +Y 的两个三角形，常用作地面。
MeshData makePlane() {
    MeshData mesh;
    mesh.vertices = {{{-0.5f, 0.0f, 0.5f}, {0, 1, 0}, {0, 0}},
                     {{0.5f, 0.0f, 0.5f}, {0, 1, 0}, {1, 0}},
                     {{0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 1}},
                     {{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {0, 1}}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.boundsMin = {-0.5f, 0.0f, -0.5f};
    mesh.boundsMax = {0.5f, 0.0f, 0.5f};
    return mesh;
}

// 生成 UV 球（经纬网格）：32 段经度 × 16 段纬度，半径 0.5，球心在原点。
// 顶点法线直接取单位球面方向，光照自然平滑；经线接缝处顶点重复是 UV 展开的正常代价。
MeshData makeSphere() {
    MeshData mesh;
    constexpr int segments = 32;
    constexpr int rings = 16;
    for (int ring = 0; ring <= rings; ++ring) {
        const float v = static_cast<float>(ring) / rings;
        const float phi = v * glm::pi<float>();
        for (int segment = 0; segment <= segments; ++segment) {
            const float u = static_cast<float>(segment) / segments;
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi),
                                   std::sin(phi) * std::sin(theta)};
            mesh.vertices.push_back({normal * 0.5f, normal, {u, 1.0f - v}});
        }
    }
    for (int ring = 0; ring < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            const auto first = static_cast<std::uint32_t>(ring * (segments + 1) + segment);
            const auto second = first + 1;
            const auto third = first + static_cast<std::uint32_t>(segments + 1);
            const auto fourth = third + 1;
            mesh.indices.insert(mesh.indices.end(), {first, third, second, second, third, fourth});
        }
    }
    mesh.boundsMin = {-0.5f, -0.5f, -0.5f};
    mesh.boundsMax = {0.5f, 0.5f, 0.5f};
    return mesh;
}

} // namespace

// PIMPL 定义：所有 GPU 资源与缓存集中于此，头文件因此无需暴露 glad/GL 类型。
// 生命周期不变量：Impl 构造时全为 0/空，只有 initialize 成功后才持有有效资源；
// shutdown 之后回到全 0 状态，可安全重复调用。
struct OpenGLRenderer::Impl {
    // 资产库仅被借用，不拥有其生命周期；用前必须确认非空。
    IAssetDatabase* assets = nullptr;
    GLuint shader = 0;
    GLuint whiteTexture = 0;
    // 离屏渲染目标。FBO（帧缓冲对象）可以理解成“一张离屏画布”：
    // 颜色写进 colorTexture，深度写进 depthRenderbuffer，而不是直接画到窗口。
    // 这样同一帧既能 blit 到窗口，也能作为纹理交给编辑器 UI 显示。
    GLuint framebuffer = 0;
    GLuint colorTexture = 0;
    GLuint depthRenderbuffer = 0;
    int width = 0;
    int height = 0;
    // 资源缓存：GUID → GPU 资源。同一网格/纹理只上传一次，后续帧直接复用。
    std::unordered_map<std::string, GpuMesh> meshes;
    std::unordered_map<std::string, GLuint> textures;
    std::unordered_map<std::string, MaterialAsset> materials;

    // 着色器 uniform 位置缓存。glGetUniformLocation 要按名字查表，开销不小，
    // 因此初始化时查一次保存下来；值为 -1 表示该 uniform 不存在或已被编译器优化掉。
    struct Uniforms {
        GLint model = -1;
        GLint view = -1;
        GLint projection = -1;
        GLint normalMatrix = -1;
        GLint albedo = -1;
        GLint hasTexture = -1;
        GLint baseColor = -1;
        GLint specularColor = -1;
        GLint shininess = -1;
        GLint cameraPosition = -1;
        GLint directionalDirection = -1;
        GLint directionalColor = -1;
        GLint directionalIntensity = -1;
        GLint pointLightCount = -1;
        std::array<GLint, 8> pointPosition{};
        std::array<GLint, 8> pointColor{};
        std::array<GLint, 8> pointIntensity{};
        std::array<GLint, 8> pointRange{};
    } uniforms;

    // 析构时兜底释放，保证即使调用方忘了 shutdown 也不会泄漏 GPU 资源
    // （前提是析构时 GL 上下文仍然有效）。
    ~Impl() { shutdown(); }

    // 编译单个着色器阶段。GLuint 本质上是 OpenGL 分配的不透明整数句柄。
    // 失败时输出编译日志并返回 0（0 不是合法的 GL 对象名）。
    static GLuint compile(GLenum type, const std::string& source) {
        const GLuint object = glCreateShader(type);
        const char* pointer = source.c_str();
        glShaderSource(object, 1, &pointer, nullptr);
        glCompileShader(object);
        GLint success = 0;
        glGetShaderiv(object, GL_COMPILE_STATUS, &success);
        if (!success) {
            std::array<char, 4096> log{};
            glGetShaderInfoLog(object, static_cast<GLsizei>(log.size()), nullptr, log.data());
            L3D_ERROR("Shader compilation failed: {}", log.data());
            glDeleteShader(object);
            return 0;
        }
        return object;
    }

    // 编译并链接顶点/片元着色器。链接成功后中间 shader 对象即可删除，
    // 程序（program）内部已保留所需代码，及时删除可避免句柄泄漏。
    static GLuint loadProgram(const std::string& vertexSource,
                              const std::string& fragmentSource) {
        const auto vertex = compile(GL_VERTEX_SHADER, vertexSource);
        const auto fragment = compile(GL_FRAGMENT_SHADER, fragmentSource);
        if (!vertex || !fragment) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return 0;
        }
        const auto program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint success = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            std::array<char, 4096> log{};
            glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            L3D_ERROR("Shader link failed: {}", log.data());
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    // 优先读取资产目录下的 shaders/<filename>；读不到时回退到内置字符串，
    // 这样发布包漏拷着色器文件也不会直接黑屏。
    std::string shaderSource(const char* filename, const char* fallback) const {
        const auto path = assets->root() / "shaders" / filename;
        if (const auto source = readTextFile(path)) {
            return *source;
        }
        L3D_WARN("Shader file '{}' missing; using embedded fallback", pathToUtf8(path));
        return fallback;
    }

    // 初始化 GL 资源。前置条件：调用线程已 make current 一个 OpenGL 4.1 上下文。
    // 顺序：编译程序 → 查询 uniform 位置 → 创建白色兜底纹理 → 设置固定 GL 状态。
    bool initialize(IAssetDatabase& assetDatabase) {
        assets = &assetDatabase;
        const auto vertex = shaderSource("lit.vert", LitVertexFallback);
        const auto fragment = shaderSource("lit.frag", LitFragmentFallback);
        shader = loadProgram(vertex, fragment);
        if (!shader) {
            return false;
        }

        // 一次性查询所有 uniform 的名字并保存为整数位置，后续每帧直接使用。
        uniforms.model = glGetUniformLocation(shader, "uModel");
        uniforms.view = glGetUniformLocation(shader, "uView");
        uniforms.projection = glGetUniformLocation(shader, "uProjection");
        uniforms.normalMatrix = glGetUniformLocation(shader, "uNormalMatrix");
        uniforms.albedo = glGetUniformLocation(shader, "uAlbedo");
        uniforms.hasTexture = glGetUniformLocation(shader, "uHasTexture");
        uniforms.baseColor = glGetUniformLocation(shader, "uBaseColor");
        uniforms.specularColor = glGetUniformLocation(shader, "uSpecularColor");
        uniforms.shininess = glGetUniformLocation(shader, "uShininess");
        uniforms.cameraPosition = glGetUniformLocation(shader, "uCameraPosition");
        uniforms.directionalDirection = glGetUniformLocation(shader, "uDirectionalDirection");
        uniforms.directionalColor = glGetUniformLocation(shader, "uDirectionalColor");
        uniforms.directionalIntensity = glGetUniformLocation(shader, "uDirectionalIntensity");
        uniforms.pointLightCount = glGetUniformLocation(shader, "uPointLightCount");
        for (int index = 0; index < 8; ++index) {
            const auto prefix = "uPointLights[" + std::to_string(index) + "]";
            uniforms.pointPosition[index] =
                glGetUniformLocation(shader, (prefix + ".position").c_str());
            uniforms.pointColor[index] =
                glGetUniformLocation(shader, (prefix + ".color").c_str());
            uniforms.pointIntensity[index] =
                glGetUniformLocation(shader, (prefix + ".intensity").c_str());
            uniforms.pointRange[index] =
                glGetUniformLocation(shader, (prefix + ".range").c_str());
        }

        // 1x1 白色纹理作为“没有纹理”时的兜底：采样它等价于乘以 1，光照结果不变。
        // 好处是渲染循环里始终能绑定一张有效纹理，无需切换采样器分支。
        const std::uint8_t whitePixel[4] = {255, 255, 255, 255};
        glGenTextures(1, &whiteTexture);
        glBindTexture(GL_TEXTURE_2D, whiteTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, whitePixel);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 开启深度测试，避免后绘制的远处物体盖住近处物体；LEQUAL 让共面物体覆盖更稳定。
        // 关闭背面剔除：内置平面等资产可能从两侧观察，且本渲染器不保证所有网格绕序统一。
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_CULL_FACE);
        L3D_INFO("OpenGL renderer initialized");
        return true;
    }

    // 释放所有 GL 资源。Impl 析构会再调用一次，因此本函数必须幂等（句柄删完置 0）。
    // 调用时仍须有有效的 GL 上下文，否则 glDelete* 会失败甚至崩溃。
    void shutdown() {
        // 逐个释放网格的 VBO/EBO/VAO。GL 对象被删除后会等到不再被引用才真正回收，
        // 因此这里的释放顺序只求清晰，不影响正确性。
        for (auto& [key, mesh] : meshes) {
            (void)key;
            glDeleteBuffers(1, &mesh.vbo);
            glDeleteBuffers(1, &mesh.ebo);
            glDeleteVertexArrays(1, &mesh.vao);
        }
        meshes.clear();
        for (auto& [key, texture] : textures) {
            (void)key;
            glDeleteTextures(1, &texture);
        }
        textures.clear();
        // 缓存中的资产纹理与 1x1 兜底纹理都要释放；同一句柄被删两次在 GL 中是安全的。
        if (whiteTexture) {
            glDeleteTextures(1, &whiteTexture);
            whiteTexture = 0;
        }
        if (colorTexture) {
            glDeleteTextures(1, &colorTexture);
            colorTexture = 0;
        }
        if (depthRenderbuffer) {
            glDeleteRenderbuffers(1, &depthRenderbuffer);
            depthRenderbuffer = 0;
        }
        if (framebuffer) {
            glDeleteFramebuffers(1, &framebuffer);
            framebuffer = 0;
        }
        if (shader) {
            glDeleteProgram(shader);
            shader = 0;
        }
        assets = nullptr;
    }

    // 按尺寸创建或重建离屏 FBO。尺寸没变就直接复用，避免每帧重新分配大纹理。
    // 颜色附件用纹理（便于后续采样显示），深度附件用 renderbuffer（只需读写、无需采样）。
    bool ensureFramebuffer(int requestedWidth, int requestedHeight) {
        // 窗口最小化时宽高可能为 0，强制至少 1x1，否则帧缓冲会不完整。
        requestedWidth = std::max(requestedWidth, 1);
        requestedHeight = std::max(requestedHeight, 1);
        if (framebuffer && requestedWidth == width && requestedHeight == height) {
            return true;
        }
        width = requestedWidth;
        height = requestedHeight;
        // 首次调用才创建对象；之后只是重新分配存储，句柄保持不变。
        if (!framebuffer) {
            glGenFramebuffers(1, &framebuffer);
            glGenTextures(1, &colorTexture);
            glGenRenderbuffers(1, &depthRenderbuffer);
        }

        // 颜色纹理：每像素 8 位 RGBA，线性过滤，边缘 clamp 防止 blit 时采到边框杂色。
        glBindTexture(GL_TEXTURE_2D, colorTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 深度与模板合并为一个 renderbuffer 附件，省去单独的模板纹理。
        glBindRenderbuffer(GL_RENDERBUFFER, depthRenderbuffer);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               colorTexture, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                  depthRenderbuffer);
        // 挂载附件后必须检查完整性：尺寸非法、驱动不支持格式等问题会在这里暴露。
        const auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            L3D_ERROR("Framebuffer is incomplete: 0x{:x}", status);
            return false;
        }
        return true;
    }

    // 把 CPU 网格上传到 GPU 并记录属性布局。
    // VAO 像一份“取数说明书”：记住当前绑定的 VBO、EBO，以及每个属性如何解析。
    // 之后绘制只需绑定 VAO，不必每次重新设置指针。
    GpuMesh uploadMesh(const MeshData& data) {
        GpuMesh mesh;
        mesh.indexCount = static_cast<GLsizei>(data.indices.size());
        mesh.bounds = {data.boundsMin, data.boundsMax};
        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        // GL_STATIC_DRAW 告诉驱动：数据上传一次、之后反复绘制，方便它选择存放位置。
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.vertices.size() * sizeof(MeshVertex)),
                     data.vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(data.indices.size() * sizeof(std::uint32_t)),
                     data.indices.data(), GL_STATIC_DRAW);
        // 每个 attribute：分量个数、类型、是否归一化、跨距（stride）、字段在结构体内的偏移。
        // location 0/1/2 与着色器中的 aPosition/aNormal/aUv 一一对应。
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void*>(offsetof(MeshVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void*>(offsetof(MeshVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                              reinterpret_cast<void*>(offsetof(MeshVertex, uv)));
        // 解绑 VAO，防止后续代码误改这份属性配置。
        glBindVertexArray(0);
        return mesh;
    }

    // 按 GUID 取得 GPU 网格：内置 GUID 走程序化生成，其余从资产库加载。
    // 加载或上传失败返回 nullptr，且不做负缓存——下一帧仍会重试，资产补齐后可自愈。
    GpuMesh* getMesh(const std::string& guid) {
        if (const auto cached = meshes.find(guid); cached != meshes.end()) {
            return &cached->second;
        }
        MeshData data;
        // 三个内置 GUID 由代码程序化生成，不依赖磁盘上的资产文件。
        if (guid == BuiltinCubeGuid) {
            data = makeCube();
        } else if (guid == BuiltinPlaneGuid) {
            data = makePlane();
        } else if (guid == BuiltinSphereGuid) {
            data = makeSphere();
        } else {
            auto loaded = assets->loadMesh(guid);
            if (!loaded) {
                L3D_WARN("Mesh '{}' could not be loaded: {}", guid, loaded.error());
                return nullptr;
            }
            data = *loaded.value();
        }
        if (data.vertices.empty() || data.indices.empty()) {
            return nullptr;
        }
        // 上传后立即插入缓存；这里只关心返回的迭代器，inserted 标志用不到。
        auto [position, inserted] = meshes.emplace(guid, uploadMesh(data));
        (void)inserted;
        return &position->second;
    }

    // 按 GUID 取得纹理。空 GUID 或加载失败时返回白色兜底纹理；
    // 失败结果也写入缓存，避免每帧重复尝试加载并刷屏日志。
    // 成功上传时生成 mipmap 并使用三线性过滤；UV 越界用 REPEAT 平铺。
    GLuint getTexture(const std::string& guid) {
        if (guid.empty()) {
            return whiteTexture;
        }
        if (const auto cached = textures.find(guid); cached != textures.end()) {
            return cached->second;
        }
        auto loaded = assets->loadTexture(guid);
        if (!loaded) {
            L3D_WARN("Texture '{}' could not be loaded: {}", guid, loaded.error());
            textures[guid] = whiteTexture;
            return whiteTexture;
        }
        const auto& texture = *loaded.value();
        GLuint handle = 0;
        glGenTextures(1, &handle);
        glBindTexture(GL_TEXTURE_2D, handle);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texture.width, texture.height, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, texture.pixels.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        textures[guid] = handle;
        return handle;
    }

    // 按 GUID 缓存材质；加载失败时回退到默认材质项。
    // 材质是值类型，缓存一份副本即可；返回的引用只保证在本帧循环期间有效。
    const MaterialAsset& getMaterial(const std::string& guid) {
        if (const auto cached = materials.find(guid); cached != materials.end()) {
            return cached->second;
        }
        auto material = assets->loadMaterial(guid);
        if (!material) {
            L3D_WARN("Material '{}' could not be loaded: {}", guid, material.error());
            return materials[BuiltinDefaultMaterialGuid];
        }
        return materials[guid] = material.value();
    }
};

// 构造只分配 Impl，不接触 GL；GPU 资源在 initialize 中按需创建，
// 这样“创建对象”和“绑定 GL 上下文”两步可以分开。
OpenGLRenderer::OpenGLRenderer() : impl_(std::make_unique<Impl>()) {}

// 析构经由 Impl::~Impl 间接调用 shutdown 释放资源。
OpenGLRenderer::~OpenGLRenderer() = default;

// 转发到 Impl；必须在持有当前 GL 上下文的线程上调用。
bool OpenGLRenderer::initialize(IAssetDatabase& assets) {
    return impl_ && impl_->initialize(assets);
}

// 转发到 Impl；重复调用安全，即使从未 initialize 也不会出错。
void OpenGLRenderer::shutdown() {
    if (impl_) {
        impl_->shutdown();
    }
}

// 绘制一帧。前置条件：GL 上下文有效、initialize 已成功；
// 任一项不满足就静默跳过本帧（避免在资源未就绪时崩溃）。
void OpenGLRenderer::render(const RenderFrame& frame, int width, int height) {
    if (!impl_ || !impl_->assets || !impl_->shader || !impl_->ensureFramebuffer(width, height)) {
        return;
    }
    // 把绘制目标切到离屏 FBO：画面先画到“离屏画布”，
    // 稍后由 presentToDefaultFramebuffer 统一拷到窗口。
    glBindFramebuffer(GL_FRAMEBUFFER, impl_->framebuffer);
    glViewport(0, 0, width, height);
    // 每帧重新声明这些状态，防止其他子系统（例如 UI 渲染）改过之后残留。
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    // 深灰蓝清屏色，比纯黑更接近编辑器视口的观感。
    glClearColor(0.045f, 0.06f, 0.085f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 相机与光照整帧共享，只在开始处上传一次。
    // GL_FALSE 表示矩阵按列主序直接上传、不做转置；glm 的内存布局恰好与之匹配。
    glUseProgram(impl_->shader);
    glUniformMatrix4fv(impl_->uniforms.view, 1, GL_FALSE, glm::value_ptr(frame.view));
    glUniformMatrix4fv(impl_->uniforms.projection, 1, GL_FALSE,
                       glm::value_ptr(frame.projection));
    glUniform3fv(impl_->uniforms.cameraPosition, 1, glm::value_ptr(frame.cameraPosition));
    glUniform3fv(impl_->uniforms.directionalDirection, 1,
                 glm::value_ptr(frame.directionalDirection));
    glUniform3fv(impl_->uniforms.directionalColor, 1,
                 glm::value_ptr(frame.directionalColor));
    glUniform1f(impl_->uniforms.directionalIntensity, frame.directionalIntensity);
    // 再夹一次范围，防止上游传入越界值让着色器循环读坏数组。
    glUniform1i(impl_->uniforms.pointLightCount,
                std::clamp(frame.pointLightCount, 0, 8));
    // 即使实际光源更少也照常上传 8 组数据：循环次数由 uPointLightCount 控制，
    // 多余数据不会被读取，但能保证 uniform 数组整体处于有效状态。
    for (int index = 0; index < 8; ++index) {
        const auto& light = frame.pointLights[static_cast<std::size_t>(index)];
        glUniform3fv(impl_->uniforms.pointPosition[static_cast<std::size_t>(index)], 1,
                     glm::value_ptr(light.position));
        glUniform3fv(impl_->uniforms.pointColor[static_cast<std::size_t>(index)], 1,
                     glm::value_ptr(light.color));
        glUniform1f(impl_->uniforms.pointIntensity[static_cast<std::size_t>(index)],
                    light.intensity);
        glUniform1f(impl_->uniforms.pointRange[static_cast<std::size_t>(index)], light.range);
    }
    // 采样器固定绑定纹理单元 0，与下面 glActiveTexture(GL_TEXTURE0) 对应。
    glUniform1i(impl_->uniforms.albedo, 0);

    // 前向渲染主通道：不使用 G-Buffer，每个 RenderItem 一次 draw call，
    // 在片元阶段直接算出最终颜色。
    // 未做合批或排序，物体多时 draw call 会成为瓶颈——这是课程规模下的有意取舍。
    for (const auto& item : frame.items) {
        auto* mesh = impl_->getMesh(item.meshGuid);
        // 网格加载失败时跳过该物体，不影响本帧其余内容。
        if (!mesh) {
            continue;
        }
        const auto& material = impl_->getMaterial(item.materialGuid);
        const auto texture = impl_->getTexture(material.albedoTexGuid);
        // 法线不能直接乘 model 矩阵：非均匀缩放会把它拉斜。正确做法是取 model
        // 左上 3x3 的逆转置，变换后的法线仍垂直于变换后的表面。
        const auto normalMatrix = glm::transpose(
            glm::inverse(glm::mat3(glm::vec3(item.transform[0]), glm::vec3(item.transform[1]),
                                   glm::vec3(item.transform[2]))));
        glUniformMatrix4fv(impl_->uniforms.model, 1, GL_FALSE,
                           glm::value_ptr(item.transform));
        glUniformMatrix3fv(impl_->uniforms.normalMatrix, 1, GL_FALSE,
                           glm::value_ptr(normalMatrix));
        glUniform4fv(impl_->uniforms.baseColor, 1, glm::value_ptr(material.baseColor));
        glUniform3fv(impl_->uniforms.specularColor, 1,
                     glm::value_ptr(material.specularColor));
        glUniform1f(impl_->uniforms.shininess, material.shininess);
        // 材质逐物体上传；无 albedo 贴图时仍绑定白色兜底纹理，
        // 由 uHasTexture 控制乘法是否生效。
        glUniform1i(impl_->uniforms.hasTexture, material.albedoTexGuid.empty() ? 0 : 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        // glDrawElements 按 EBO 中的索引取顶点：索引复用让共享顶点只存一份。
        glBindVertexArray(mesh->vao);
        glDrawElements(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, nullptr);
    }

    // 收尾解绑，恢复到干净状态，避免影响之后使用默认帧缓冲的 UI 绘制。
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// 把离屏颜色附件拷贝到默认帧缓冲（0 号 FBO 即窗口）。
// 源区域按内部 FBO 尺寸、目标区域按窗口尺寸，GL_LINEAR 负责缩放，避免画面变形留边。
void OpenGLRenderer::presentToDefaultFramebuffer(int width, int height) {
    if (!impl_ || !impl_->framebuffer || width <= 0 || height <= 0) {
        return;
    }
    // 读/写帧缓冲分开指定：一个作为 blit 来源（离屏 FBO），一个作为目标（窗口）。
    glBindFramebuffer(GL_READ_FRAMEBUFFER, impl_->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, impl_->width, impl_->height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
}

// 返回离屏颜色纹理句柄（0 表示尚未创建），供 UI 层当作普通 2D 贴图显示。
std::uint32_t OpenGLRenderer::viewportTexture() const {
    return impl_ ? impl_->colorTexture : 0;
}

// 按需加载网格以取得局部包围盒；未初始化或加载失败返回 nullopt。
std::optional<MeshBounds> OpenGLRenderer::meshBounds(const std::string& meshGuid) {
    if (!impl_ || !impl_->assets) {
        return std::nullopt;
    }
    const auto* mesh = impl_->getMesh(meshGuid);
    return mesh ? std::optional<MeshBounds>(mesh->bounds) : std::nullopt;
}

} // namespace l3d
