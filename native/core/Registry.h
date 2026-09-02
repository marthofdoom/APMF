#pragma once
#include "core/Channel.h"

// ============================================================================
// APMF core -- the channel REGISTRY. A flat list of every Channel instance. The
// arbiter drives it each tick; the input layer routes hotkeys through it; the
// startup help log enumerates it. Channels self-register at load via
// APMF_REGISTER_CHANNEL, so the registry is the ONLY place the core learns which
// facets exist -- no central switch to edit when a facet is added.
// ============================================================================

namespace apmf {

    class Registry {
    public:
        static Registry& Get();

        void Register(Channel* ch);
        const std::vector<Channel*>& All() const { return channels; }

        // Any channel currently holding authority over the gated target?
        bool AnyEngaged() const;

    private:
        std::vector<Channel*> channels;
    };

    // One-line self-registration. A channel .cpp ends with
    //   APMF_REGISTER_CHANNEL(MyChannel);
    // which constructs a program-lifetime instance and registers it at load.
    // Every source links directly into the DLL target (CMake GLOB, no static
    // archive) so this initializer is retained -- see Docs/INVARIANTS.md #9.
    template <class T>
    struct AutoRegister {
        AutoRegister() {
            static T instance;
            Registry::Get().Register(&instance);
        }
    };

}

#define APMF_REGISTER_CHANNEL(TYPE) \
    namespace { ::apmf::AutoRegister<TYPE> APMF_autoreg_##TYPE{}; }
