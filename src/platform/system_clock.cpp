#include "system_clock.h"

namespace something::platform {

QDateTime SystemClock::nowUtc() const
{
    return QDateTime::currentDateTimeUtc();
}

QTimeZone SystemClock::localTimeZone() const
{
    return QTimeZone::systemTimeZone();
}

} // namespace something::platform
