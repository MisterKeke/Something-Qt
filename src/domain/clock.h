#pragma once

#include <QDate>
#include <QDateTime>
#include <QTimeZone>

namespace something::domain {

// UTC instants and local calendar context are separate on purpose: task due
// dates are date-only values and must never be shifted through UTC.
class Clock {
public:
    virtual ~Clock() = default;
    virtual QDateTime nowUtc() const = 0;
    virtual QTimeZone localTimeZone() const = 0;

    QDate localDate() const
    {
        const QTimeZone zone = localTimeZone();
        const QDateTime instant = nowUtc().toUTC();
        return zone.isValid() ? instant.toTimeZone(zone).date() : instant.date();
    }
};

} // namespace something::domain
