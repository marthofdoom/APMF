#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// REQUIRED, not stylistic. add_commonlibsse_plugin generates a TU that uses
// "..."sv literals and force-includes this PCH; without this the build dies
// with C3688 "invalid literal suffix 'sv'". MFO/MEO's PCH end with the same
// line for the same reason.
using namespace std::literals;
