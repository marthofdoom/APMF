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

    Channel* Registry::ChannelForIntent(APMF_API::Intent intent) const {
        if (intent == APMF_API::kIntent_None) return nullptr;
        for (auto* ch : channels) {
            if (ch->ServesIntent() == intent) return ch;
        }
        return nullptr;
    }

    Channel* Registry::ChannelForHotkey(std::uint32_t code) const {
        for (auto* ch : channels) {
            for (const auto& hk : ch->Hotkeys()) {
                if (hk.code == code) return ch;
            }
        }
        return nullptr;
    }

}
