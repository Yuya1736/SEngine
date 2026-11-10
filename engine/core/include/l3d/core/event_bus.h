#pragma once

// 文件职责：类型安全的同步事件总线，让模块之间通过"事件类型 + 回调"通信而互不依赖。
// 所属模块：engine_core。
// 分层位置：engine_app 组合根 → 各 runtime 模块 → engine_core，上层用它转发跨模块通知。
// 关键约束：发布是同步的（回调在 publish 返回前执行完）；假定单线程，不支持发布期间增删订阅。

#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace l3d {

// 事件总线：发送方 publish 一个事件对象，所有订阅了该类型的回调依次收到同一个 const 引用。
// 不变量：handlers_ 以 typeid(Event) 为键，键与 Handler<Event> 的实际类型一一对应。
class EventBus {
public:
    // 订阅某类事件；回调按注册顺序被调用，订阅随总线对象一起销毁（暂无取消订阅接口）。
    template <typename Event>
    void subscribe(std::function<void(const Event&)> handler) {
        auto subscription = std::make_shared<Handler<Event>>(std::move(handler));
        handlers_[std::type_index(typeid(Event))].push_back(std::move(subscription));
    }

    // 同步发布：在调用者线程上立即执行完全部回调才返回。
    // 之所以同步，是因为引擎是单线程帧循环：这样"事件已处理完"成为确定的时序点，
    // 不需要队列和额外锁。代价是回调里不能做耗时操作，也不应在回调中增删订阅。
    template <typename Event>
    void publish(const Event& event) const {
        const auto position = handlers_.find(std::type_index(typeid(Event)));
        if (position == handlers_.end()) {
            return;
        }
        for (const auto& handler : position->second) {
            static_cast<const Handler<Event>&>(*handler).callback(event);
        }
    }

private:
    // 类型擦除的基类：让不同事件类型的回调都能放进同一个容器。
    struct HandlerBase {
        virtual ~HandlerBase() = default;
    };

    // 具体事件类型的回调包装；publish 查表后按 typeid 一致再转回 Handler<Event>。
    template <typename Event>
    struct Handler final : HandlerBase {
        explicit Handler(std::function<void(const Event&)> value) : callback(std::move(value)) {}
        std::function<void(const Event&)> callback;
    };

    // 键为事件类型，值为该类型的所有回调；unordered_map 的查找是均摊 O(1)。
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<HandlerBase>>> handlers_;
};

} // namespace l3d
