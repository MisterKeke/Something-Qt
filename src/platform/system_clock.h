#pragma once

#include "../domain/clock.h"

namespace something::platform {

class SystemClock final : public domain::Clock {
public:
    QDateTime nowUtc() const override;
    QTimeZone localTimeZone() const override;
};

} // namespace something::platform
