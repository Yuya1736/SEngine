// 进程入口：只负责解析命令行并构造组合根。
// 所有引擎能力都在 Application 内部装配，main 不直接触碰任何子系统。
#include <iostream>
#include <string>
#include <string_view>

#include "application.h"

namespace {

void printUsage() {
    std::cout << "Usage: engine_app [--runtime] [--hidden] [--frames <count>] "
                 "[--no-vsync] [--screenshot <file>] [--scene <file>] "
                 "[--assets <directory>]\n";
}

} // namespace

int main(int argc, char** argv) {
    // 命令行参数全部映射为 ApplicationConfig；非法输入返回 2（区别于运行失败）。
    l3d::ApplicationConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--runtime") {
            config.runtimeMode = true;
        } else if (argument == "--hidden") {
            config.hiddenWindow = true;
        } else if (argument == "--no-vsync") {
            config.vsync = false;
        } else if (argument == "--screenshot" && index + 1 < argc) {
            // 带值的参数会“吃掉”下一个 argv，因此循环里手动 ++index。
            config.screenshotPath = argv[++index];
        } else if (argument == "--frames" && index + 1 < argc) {
            try {
                config.maxFrames = std::stoi(argv[++index]);
            } catch (...) {
                std::cerr << "Invalid frame count\n";
                return 2;
            }
        } else if (argument == "--scene" && index + 1 < argc) {
            config.scenePath = argv[++index];
        } else if (argument == "--assets" && index + 1 < argc) {
            config.assetRoot = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            printUsage();
            return 2;
        }
    }
    l3d::Application application(config);
    return application.run();
}
