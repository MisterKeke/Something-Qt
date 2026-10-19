#pragma once

#include "../domain/app_error.h"

#include <QString>
#include <memory>

class QLockFile;

namespace something::platform {

class ProfileInstanceLock final {
public:
    explicit ProfileInstanceLock(QString profileDirectory);
    ~ProfileInstanceLock();

    ProfileInstanceLock(const ProfileInstanceLock &) = delete;
    ProfileInstanceLock &operator=(const ProfileInstanceLock &) = delete;

    bool acquire(something::domain::AppError *error = nullptr);
    void release();
    bool isHeld() const noexcept { return m_held; }
    QString lockPath() const;

private:
    QString m_profileDirectory;
    std::unique_ptr<QLockFile> m_lock;
    bool m_held{false};
};

} // namespace something::platform
