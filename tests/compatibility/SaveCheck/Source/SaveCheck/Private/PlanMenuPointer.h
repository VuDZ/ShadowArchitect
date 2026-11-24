#pragma once

#include "Containers/StringView.h"

enum class EPlanMenuPointer { Hover, Down, Up };

bool SendPlanMenuPointer(FStringView Label, EPlanMenuPointer Action);
bool IsPlanMenuButtonVisible(FStringView Label);
bool SendPlanMenuEscape();
