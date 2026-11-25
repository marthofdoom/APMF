#pragma once
#include "APMF_API.h"

// A Repoint that changes nothing is a heartbeat: no line (core/ControlMap.cpp, ch.25). PURE (tools/row_equip).
namespace apmf::logdedupe {
    constexpr bool SameParam(const APMF_API::APMF_Param& a, const APMF_API::APMF_Param& b) {
        return a.form == b.form && a.fval == b.fval && a.ival == b.ival && a.target == b.target && a.posX == b.posX &&
               a.posY == b.posY && a.posZ == b.posZ;
    }
}
