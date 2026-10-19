#pragma once

#include "app_error.h"
#include "task.h"

namespace something::domain {

// Mutates valid values into the canonical form written by repositories.
AppError normalizeTaskInput(TaskInput &input);
AppError normalizeTaskFilter(TaskFilter &filter);
bool isValidIsoDate(const QString &date);

} // namespace something::domain
