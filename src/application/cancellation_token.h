#pragma once

#include <atomic>
#include <memory>

namespace something::application {

class CancellationToken final {
public:
    CancellationToken() = default;

    bool isCancellationRequested() const noexcept
    {
        return m_state && m_state->load(std::memory_order_acquire);
    }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
        : m_state(std::move(state))
    {
    }

    std::shared_ptr<std::atomic_bool> m_state;
};

class CancellationSource final {
public:
    CancellationSource()
        : m_state(std::make_shared<std::atomic_bool>(false))
    {
    }

    CancellationToken token() const { return CancellationToken(m_state); }

    bool requestCancellation() const noexcept
    {
        return !m_state->exchange(true, std::memory_order_acq_rel);
    }

private:
    std::shared_ptr<std::atomic_bool> m_state;
};

} // namespace something::application
