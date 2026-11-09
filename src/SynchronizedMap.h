#pragma once

#include "vkroots.h"

#include <mutex>

namespace HdrLayer
{

template <typename Key, typename Data>
class SynchronizedMap
{
public:
    class Locked
    {
    public:
        Locked() = default;
        Locked(std::unique_lock<std::mutex> lock, Data *data)
            : m_lock(std::move(lock))
            , m_data(data)
        {
        }

        explicit operator bool() const { return m_data != nullptr; }
        Data *operator->() const { return m_data; }
        Data &operator*() const { return *m_data; }

        void unlock()
        {
            m_lock = {};
            m_data = nullptr;
        }

    private:
        std::unique_lock<std::mutex> m_lock;
        Data *m_data = nullptr;
    };

    Locked get(const Key &key)
    {
        std::unique_lock lock{ m_mutex };
        Data *data = m_map.find(key);
        if (!data) {
            return {};
        }
        return Locked{ std::move(lock), data };
    }

    void create(const Key &key, Data data)
    {
        std::scoped_lock lock{ m_mutex };
        m_map.create(key, std::move(data));
    }

    void remove(const Key &key)
    {
        std::scoped_lock lock{ m_mutex };
        m_map.erase(key);
    }

private:
    std::mutex m_mutex;
    vkroots::ObjectMap<Key, Data> m_map;
};

}
