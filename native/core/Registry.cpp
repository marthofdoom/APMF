#include "PCH.h"
#include "core/Registry.h"

namespace apmf {

    Registry& Registry::Get() {
        static Registry s_instance;   // Meyers singleton; safe under dynamic-init self-registration
        return s_instance;
    }

    void Registry::Register(Channel* ch) {
        if (ch) channels.push_back(ch);
    }

    bool Registry::AnyEngaged() const {
        for (auto* ch : channels) {
            if (ch->Engaged()) return true;
        }
        return false;
    }

}
