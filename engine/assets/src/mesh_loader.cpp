// engine/assets 模块：Wavefront OBJ 网格加载器。
// 分层位置：engine_core 之上、AssetDatabase 之下，只负责“文本 OBJ → 三角形 MeshData”。
// 只解析几何（v/vt/vn/f），不处理 MTL 材质库：材质由独立的 .mat.json 资产描述，
// 这样网格与材质解耦，同一网格可以被不同材质复用。
// 约束：不抛异常，所有错误都通过 Result::failure 返回。

#include "l3d/assets/loaders.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#include <glm/geometric.hpp>

#include "l3d/core/file_utils.h"
#include "l3d/core/logging.h"

namespace l3d {

namespace {

// OBJ 面中的一个“角点”：三个下标分别引用位置、UV、法线三个共享列表。
// 某个分量没写时为 -1（OBJ 允许 "v//vn" 或只有 "v" 这类省略形式）。
struct FaceVertex {
    int position = -1;
    int texCoord = -1;
    int normal = -1;
};

// 把 "v/vt/vn" 形式的 token 按 '/' 拆成三段，逐段转成整数。
// 最多切 3 段，多余的 '/' 忽略；空段（如 "v//vn" 中间那段）转成 -1。
bool parseFaceVertex(std::string_view token, FaceVertex& result) {
    std::array<std::string_view, 3> parts{};
    std::size_t partIndex = 0;
    // 逐段截取到下一个 '/'；没有更多 '/' 时，剩余整段就是最后一段。
    std::size_t start = 0;
    while (start <= token.size() && partIndex < parts.size()) {
        const auto slash = token.find('/', start);
        const auto end = slash == std::string_view::npos ? token.size() : slash;
        parts[partIndex++] = token.substr(start, end - start);
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }

    // 空段返回 -1；非法数字用 int 最小值作哨兵，供调用方统一判断。
    // 负数合法：OBJ 用负下标表示“从列表末尾往回数”。
    auto parseIndex = [](std::string_view value) -> int {
        if (value.empty()) {
            return -1;
        }
        int index = 0;
        const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), index);
        if (error != std::errc{} || pointer != value.data() + value.size()) {
            return std::numeric_limits<int>::min();
        }
        return index;
    };

    result.position = parseIndex(parts[0]);
    result.texCoord = parseIndex(parts[1]);
    result.normal = parseIndex(parts[2]);
    return result.position != std::numeric_limits<int>::min() &&
           result.texCoord != std::numeric_limits<int>::min() &&
           result.normal != std::numeric_limits<int>::min();
}

// 把 OBJ 下标转成 C++ 数组下标。注意 OBJ 下标从 1 开始（不是 0），所以要减 1。
// 正数 1..size 对应 0..size-1；负数 -1 表示最后一个，-size 表示第一个；0 或越界返回 false。
bool resolveIndex(int index, std::size_t size, std::uint32_t& resolved) {
    if (index > 0 && static_cast<std::size_t>(index) <= size) {
        resolved = static_cast<std::uint32_t>(index - 1);
        return true;
    }
    if (index < 0 && static_cast<std::size_t>(-index) <= size) {
        resolved = static_cast<std::uint32_t>(static_cast<std::int64_t>(size) + index);
        return true;
    }
    return false;
}

} // namespace

// 解析 OBJ：先收集共享的 v/vt/vn 列表与面，最后展开成 GPU 可直接绘制的三角形。
// OBJ 是纯文本格式，每行以关键字开头，所以按行扫描即可。
Result<MeshData> loadObjMesh(const std::filesystem::path& path) {
    // 以二进制方式打开，避免 Windows 文本模式自动转换换行符。
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Result<MeshData>::failure("Cannot open OBJ: " + pathToUtf8(path));
    }

    // OBJ 先集中声明位置/UV/法线，面再通过下标引用，所以需要三个共享列表。
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> texCoords;
    MeshData mesh;
    // 用浮点极值初始化包围盒，之后逐顶点做 min/max 扩展。
    mesh.boundsMin = glm::vec3(std::numeric_limits<float>::max());
    mesh.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(stream, line)) {
        ++lineNumber;
        std::istringstream parser(line);
        std::string prefix;
        parser >> prefix;
        // 空行和以 '#' 开头的注释行直接跳过。
        if (prefix.empty() || prefix[0] == '#') {
            continue;
        }

        // v：顶点位置，通常 3 个分量；若后面还有 w 分量，这里自然忽略。
        if (prefix == "v") {
            glm::vec3 value{};
            if (!(parser >> value.x >> value.y >> value.z)) {
                return Result<MeshData>::failure("Malformed vertex at OBJ line " +
                                                 std::to_string(lineNumber));
            }
            positions.push_back(value);
        // vt：UV，一般是两个分量；只写了 u 的文件把 v 当作 0。
        } else if (prefix == "vt") {
            glm::vec2 value{};
            if (!(parser >> value.x)) {
                return Result<MeshData>::failure("Malformed UV at OBJ line " +
                                                 std::to_string(lineNumber));
            }
            if (!(parser >> value.y)) {
                value.y = 0.0f;
            }
            texCoords.push_back(value);
        // vn：法线。OBJ 不保证法线已归一化，输出前会手动归一化。
        } else if (prefix == "vn") {
            glm::vec3 value{};
            if (!(parser >> value.x >> value.y >> value.z)) {
                return Result<MeshData>::failure("Malformed normal at OBJ line " +
                                                 std::to_string(lineNumber));
            }
            normals.push_back(value);
        // f：面。每个 token 是一个角点，可能写 "v"、"v/vt"、"v//vn" 或 "v/vt/vn"。
        } else if (prefix == "f") {
            std::vector<FaceVertex> face;
            std::string token;
            while (parser >> token) {
                FaceVertex vertex;
                if (!parseFaceVertex(token, vertex)) {
                    return Result<MeshData>::failure("Malformed face at OBJ line " +
                                                     std::to_string(lineNumber));
                }
                face.push_back(vertex);
            }
            // 少于 3 个角点构不成面，按格式错误处理。
            if (face.size() < 3) {
                return Result<MeshData>::failure("Face with fewer than 3 vertices at OBJ line " +
                                                 std::to_string(lineNumber));
            }

            // 先把本面各角点的位置解析出来，用于后面计算面法线。
            std::vector<glm::vec3> facePositions;
            facePositions.reserve(face.size());
            for (const auto& vertex : face) {
                std::uint32_t positionIndex = 0;
                if (!resolveIndex(vertex.position, positions.size(), positionIndex)) {
                    return Result<MeshData>::failure("Face index out of range at OBJ line " +
                                                     std::to_string(lineNumber));
                }
                facePositions.push_back(positions[positionIndex]);
            }

            // 用前三个角点做叉积得到面法线，供没有 vn 的角点回退使用。
            // 三点共线（退化面）时叉积为零，normalize 会得到 NaN，应避免这种资产。
            const auto faceNormal = glm::normalize(glm::cross(facePositions[1] - facePositions[0],
                                                              facePositions[2] - facePositions[0]));
            const auto base = static_cast<std::uint32_t>(mesh.vertices.size());

            // OBJ 的下标是“每个角点”独立的：同一位置在不同面里可能配不同 UV/法线，
            // 所以按角点展开成独立顶点，而不是直接复用位置列表。
            for (const auto& vertex : face) {
                std::uint32_t positionIndex = 0;
                if (!resolveIndex(vertex.position, positions.size(), positionIndex)) {
                    return Result<MeshData>::failure("Face index out of range at OBJ line " +
                                                     std::to_string(lineNumber));
                }

                MeshVertex output;
                output.position = positions[positionIndex];
                // 有法线就用自己的并归一化；零长度法线退回面法线，避免光照出问题。
                // 注意：这里只把非负下标当作“已提供”，负下标（合法的相对引用）会走 else。
                if (vertex.normal >= 0) {
                    std::uint32_t normalIndex = 0;
                    if (!resolveIndex(vertex.normal, normals.size(), normalIndex)) {
                        return Result<MeshData>::failure("Normal index out of range at OBJ line " +
                                                         std::to_string(lineNumber));
                    }
                    const auto length = glm::length(normals[normalIndex]);
                    output.normal = length > 0.00001f ? normals[normalIndex] / length : faceNormal;
                } else {
                    output.normal = faceNormal;
                }
                // 没有 UV 的角点保留 MeshVertex 默认的 (0,0)。
                if (vertex.texCoord >= 0) {
                    std::uint32_t uvIndex = 0;
                    if (!resolveIndex(vertex.texCoord, texCoords.size(), uvIndex)) {
                        return Result<MeshData>::failure("UV index out of range at OBJ line " +
                                                         std::to_string(lineNumber));
                    }
                    output.uv = texCoords[uvIndex];
                }

                // 包围盒按实际输出的顶点扩展，与是否三角化无关。
                mesh.boundsMin = glm::min(mesh.boundsMin, output.position);
                mesh.boundsMax = glm::max(mesh.boundsMax, output.position);
                mesh.vertices.push_back(output);
            }

            // 扇形三角化：固定第一个角点为扇心，连接相邻角点，把 n 边形拆成 n-2 个三角形。
            // 顶点已按角点展开，所以三角形直接引用 base + 本面内的局部下标。
            for (std::size_t index = 1; index + 1 < face.size(); ++index) {
                mesh.indices.push_back(base);
                mesh.indices.push_back(base + static_cast<std::uint32_t>(index));
                mesh.indices.push_back(base + static_cast<std::uint32_t>(index + 1));
            }
        }
    }

    // 只有顶点没有面（或反之）的 OBJ 无法渲染，按失败返回，交给上层决定回退。
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return Result<MeshData>::failure("OBJ contains no renderable faces: " +
                                         pathToUtf8(path));
    }
    L3D_INFO("Imported OBJ '{}': {} vertices, {} indices", pathToUtf8(path),
             mesh.vertices.size(), mesh.indices.size());
    return Result<MeshData>::success(std::move(mesh));
}

} // namespace l3d
