#pragma once

// 截图工具：把默认帧缓冲（当前后缓冲）读回 CPU 并编码为 PNG。
// 主要用于自动化冒烟测试和文档配图，不参与正常渲染流程。

#include <filesystem>

namespace l3d {

// 读取默认帧缓冲并写 PNG。成功返回 true；尺寸非法、目录创建或编码失败返回 false。
// 注意：调用前必须已经渲染完一帧且尚未 swapBuffers，否则可能读到未完成/已交换的内容。
[[nodiscard]] bool writeDefaultFramebufferPng(const std::filesystem::path& path, int width,
                                              int height);

} // namespace l3d
