#pragma once

/**
 * HeliosView.Core —— Signal：信号槽（类似 Qt）。
 *
 * 槽为 std::function，存储于 C++23 std::flat_set（有序扁平容器，缓存友好）：
 *   - connect 返回槽 id，disconnect(id) 移除（异构查找）
 *   - 发射时拷贝槽表再迭代：槽中再连接/断开不影响本次发射
 *
 * 无其他依赖；注意信号槽在消息循环线程中执行（App::exec 的帧回调里发射）。
 */

#include <cstddef>
#include <flat_set>
#include <functional>
#include <utility>

namespace helios {

template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;
    using Id = std::size_t;

    // 连接槽，返回槽 id（Qt: connect）
    Id connect(Slot slot)
    {
        const Id id = m_nextId++;
        m_slots.emplace(Entry{id, std::move(slot)});
        return id;
    }

    // 断开槽（Qt: disconnect）。is_transparent 比较器支持直接传槽 id。
    void disconnect(Id id) { m_slots.erase(id); }

    // 断开全部槽
    void clear() { m_slots.clear(); }

    std::size_t slotCount() const { return m_slots.size(); }
    bool empty() const { return m_slots.empty(); }

    // 发射信号（Qt: emit）。拷贝槽表再迭代，槽中连接/断开不影响本次发射。
    void operator()(Args... args) const
    {
        const auto slots = m_slots; /* 拷贝：槽数量少，代价可忽略 */
        for (const auto& entry : slots)
            if (entry.slot)
                entry.slot(args...);
    }

private:
    struct Entry {
        Id id;
        Slot slot;
    };

    // 按槽 id 排序；is_transparent 开启异构查找（erase(Id) 无需构造 Entry）
    struct ById {
        using is_transparent = void;
        bool operator()(const Entry& a, const Entry& b) const { return a.id < b.id; }
        bool operator()(Id a, const Entry& b) const { return a < b.id; }
        bool operator()(const Entry& a, Id b) const { return a.id < b; }
    };

    std::flat_set<Entry, ById> m_slots;
    Id m_nextId = 1;
};

} // namespace helios
