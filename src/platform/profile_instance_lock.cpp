#include "profile_instance_lock.h"

#include <QDir>
#include <QLockFile>

#include <utility>

namespace something::platform {

ProfileInstanceLock::ProfileInstanceLock(QString profileDirectory)
    : m_profileDirectory(std::move(profileDirectory))
{
}

ProfileInstanceLock::~ProfileInstanceLock()
{
    release();
}

bool ProfileInstanceLock::acquire(something::domain::AppError *error)
{
    if (m_held) {
        if (error) {
            *error = something::domain::AppError::none();
        }
        return true;
    }

    if (m_profileDirectory.isEmpty() || !QDir().mkpath(m_profileDirectory)) {
        if (error) {
            *error = something::domain::AppError(something::domain::ErrorCategory::InstanceLock);
        }
        return false;
    }

    m_lock = std::make_unique<QLockFile>(lockPath());
    if (!m_lock->tryLock(0)) {
        const auto category = m_lock->error() == QLockFile::LockFailedError
                                  ? something::domain::ErrorCategory::InstanceAlreadyRunning
                                  : something::domain::ErrorCategory::InstanceLock;
        m_lock.reset();
        if (error) {
            *error = something::domain::AppError(category);
        }
        return false;
    }

    m_held = true;
    if (error) {
        *error = something::domain::AppError::none();
    }
    return true;
}

void ProfileInstanceLock::release()
{
    if (!m_lock) {
        m_held = false;
        return;
    }
    if (m_held) {
        m_lock->unlock();
    }
    m_held = false;
    m_lock.reset();
}

QString ProfileInstanceLock::lockPath() const
{
    return QDir(m_profileDirectory).filePath(QStringLiteral("instance.lock"));
}

} // namespace something::platform
