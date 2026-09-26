#include "PCH.h"
#include "core/ClientAPI.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"
#include "core/SpaceQuery.h"
#include "channels/Travel.h"

// The C-ABI implementation behind APMF_API.h. These free functions forward to the
// in-process multi-NPC engine (core/ControlMap) over its thread-safe enqueue path,
// so a client's worker thread may call them directly. No client-specific code here.
namespace {

    // NO exception may cross the DLL boundary (APMF_API.h). A throw unwinding across
    // MFO's separately-compiled DLL is UB, so every exported body is a hard catch-all
    // (bad_alloc from m_queue.push_back, an spdlog throw, ...). A swallowed throw
    // degrades to "no control taken", never a crash in the client.
    APMF_API::Handle APMF_Request(RE::FormID actor, APMF_API::Intent intent, float basis) {
        try {
            return apmf::ControlMap::Get().EnqueueRequest(actor, intent, basis, nullptr);
        } catch (...) {
            return APMF_API::kInvalidHandle;
        }
    }

    // ABI v2: carries the POD param down to the channel (e.g. the cast-select spell).
    // The pointer is read+copied synchronously here; APMF never retains it.
    APMF_API::Handle APMF_RequestEx(RE::FormID actor, APMF_API::Intent intent, float basis,
                                    const APMF_API::APMF_Param* param) {
        try {
            return apmf::ControlMap::Get().EnqueueRequest(actor, intent, basis, param);
        } catch (...) {
            return APMF_API::kInvalidHandle;
        }
    }

    void APMF_Release(APMF_API::Handle handle) {
        try {
            apmf::ControlMap::Get().EnqueueRelease(handle);
        } catch (...) {
        }
    }

    // ABI v3: re-point an existing claim's param in place (same handle) -- e.g.
    // combat-target switches the held foe without a release/re-request. Copied
    // synchronously; APMF never retains the pointer.
    void APMF_Repoint(APMF_API::Handle handle, const APMF_API::APMF_Param* param) {
        try {
            apmf::ControlMap::Get().EnqueueRepoint(handle, param);
        } catch (...) {
        }
    }

    // ABI v4: attach a bounded ADDITIONAL allow-set of spell FormIDs to an
    // existing kIntent_SelectSpell claim (see APMF_API.h's APMF_API_v4 doc
    // comment). `forms` is READ AND COPIED synchronously here -- APMF never
    // retains the pointer, same contract as RequestEx/Repoint's `param`; `count`
    // is clamped to kMaxSpellAllowList inside EnqueueSetSpellAllowList.
    void APMF_SetSpellAllowList(APMF_API::Handle handle, const RE::FormID* forms, std::uint32_t count) {
        try {
            apmf::ControlMap::Get().EnqueueSetSpellAllowList(handle, forms, count);
        } catch (...) {
        }
    }

    // ABI v5: claim the cast-EXECUTION facet (ch.8b). The rich APMF_CastRequest is
    // read+copied synchronously here (APMF never retains the pointer, same contract
    // as RequestEx's `param`). APMF fires NO cast -- it records the claim, denies the
    // AI's competing cast/re-arm at the three gates, and auto-releases at the TTL;
    // the CLIENT executes its own animated cast (design.md §1a).
    APMF_API::Handle APMF_RequestCast(RE::FormID actor, float basis,
                                      const APMF_API::APMF_CastRequest* req) {
        try {
            return apmf::ControlMap::Get().EnqueueCast(actor, basis, req);
        } catch (...) {
            return APMF_API::kInvalidHandle;
        }
    }

    // ABI v6: read-only cast-claim observability (see APMF_API_v6's doc comment).
    // Neither call mutates anything -- both forward straight to ControlMap's
    // handle-keyed RCU-snapshot scan. Safe from any thread, same contract as
    // every other exported entry here.
    RE::FormID APMF_GetCastProxy(APMF_API::Handle handle) {
        try {
            return apmf::ControlMap::Get().GetCastProxy(handle);
        } catch (...) {
            return 0;
        }
    }

    bool APMF_IsClaimLive(APMF_API::Handle handle) {
        try {
            return apmf::ControlMap::Get().IsClaimLive(handle);
        } catch (...) {
            return false;
        }
    }

    // ABI v7 (ch.17, kIntent_EquipAuthority): DECLARE the worn set on an existing
    // equip-authority claim (see APMF_API_v7's doc comment). `forms` is READ AND
    // COPIED synchronously here -- APMF never retains the pointer, same contract as
    // SetSpellAllowList's array; `count` is clamped to kMaxEquipSet inside
    // EnqueueSetEquipSet. Applied on the game thread at the next Drain.
    void APMF_SetEquipSet(APMF_API::Handle handle, const RE::FormID* forms, std::uint32_t count) {
        try {
            apmf::ControlMap::Get().EnqueueSetEquipSet(handle, forms, count);
        } catch (...) {
        }
    }

    // ABI v8 (ch.17): the same declaration with a HAND per item (see APMF_API_v8's
    // doc comment). `entries` is READ AND COPIED synchronously inside
    // EnqueueSetEquipSetEx -- APMF never retains the pointer; `count` is clamped
    // to kMaxEquipSet there. Applied on the game thread at the next Drain.
    void APMF_SetEquipSetEx(APMF_API::Handle handle, const APMF_API::APMF_EquipEntry* entries, std::uint32_t count) {
        try {
            apmf::ControlMap::Get().EnqueueSetEquipSetEx(handle, entries, count);
        } catch (...) {
        }
    }

    // ABI v8: seat status -- installed AND not observe-only (see APMF_API_v8's doc
    // comment). Two atomic loads; safe from any thread.
    bool APMF_IsEquipAuthorityEnforced() {
        try {
            return apmf::equipsink::Enforcing();
        } catch (...) {
            return false;
        }
    }

    // ABI v9 (ch.17): SCOPE an equip-authority claim to owned/denied categories
    // (see APMF_API_v9's doc comment). `scope` is READ AND COPIED synchronously
    // inside EnqueueSetEquipScope -- APMF never retains the pointer; nullptr
    // resets to the defaults. Applied on the game thread at the next Drain.
    void APMF_SetEquipScope(APMF_API::Handle handle, const APMF_API::APMF_EquipScope* scope) {
        try {
            apmf::ControlMap::Get().EnqueueSetEquipScope(handle, scope);
        } catch (...) {
        }
    }

    // ABI v11: the two read-only SPACE QUERIES (see APMF_API.h, "ABI v11: SPACE
    // QUERIES"). Synchronous and main-thread-only by contract; the refusal for any
    // other thread lives inside core/SpaceQuery.cpp. A throw never crosses the
    // boundary: it becomes kQuery_Failed.
    std::uint32_t APMF_FindEmptySpace(const APMF_API::APMF_SpaceQuery* q, APMF_API::APMF_SpaceResult* out) {
        try {
            return apmf::spacequery::FindEmptySpace(q, out);
        } catch (...) {
            return APMF_API::kQuery_Failed;
        }
    }

    std::uint32_t APMF_FindHostilesInSpace(const APMF_API::APMF_HostileQuery* q, RE::FormID* outActors,
                                           std::uint32_t capacity, std::uint32_t* outCount, std::uint32_t* outTotal) {
        try {
            return apmf::spacequery::FindHostilesInSpace(q, outActors, capacity, outCount, outTotal);
        } catch (...) {
            return APMF_API::kQuery_Failed;
        }
    }

    // ABI v12: the travel leg-state read (see APMF_API_v12's doc comment). Read-only,
    // any thread: channels/Travel.cpp copies a per-actor record under its own mutex. A
    // throw never crosses the boundary: it becomes kLeg_None with nothing written.
    std::uint32_t APMF_GetTravelLegState(RE::FormID actor, APMF_API::APMF_TravelLegInfo* out) {
        try {
            return apmf::travel::GetLegState(actor, out);
        } catch (...) {
            return APMF_API::kLeg_None;
        }
    }

    // ABI -> the first APMF release that implements it, for the "client too new"
    // refusal log below (MFO wiring review SEV-3 F4): a user running an older
    // APMF under a newer client must be able to read WHICH APMF they need. Keep in
    // step with kABIVersion bumps (git tags: v0.2.0 v1, v0.2.3 v2, v0.3.0-rc.1 v3,
    // v0.3.0-rc.3 v4, v0.9.1 v5, v0.9.3 v6; v7, v8, v9 and v10 ship together in the
    // first release after 0.9.4 -- REVIEW-BACKLOG APMF-B10: name it at the cut; v11 and
    // v12 ship together in 0.9.8; v13 and v14 ship in 0.9.9; v15 and v16 are Unreleased in
    // CHANGELOG.md -- name their release here at the cut).
    const char* MinReleaseForAbi(std::uint32_t abi) {
        switch (abi) {
        case 1:  return "0.2.0";
        case 2:  return "0.2.3";
        case 3:  return "0.3.0-rc.1";
        case 4:  return "0.3.0-rc.3";
        case 5:  return "0.9.1";
        case 6:  return "0.9.3";
        case 7:
        case 8:
        case 9:
        case 10: return "0.9.5";
        case 11:
        case 12: return "0.9.8";
        case 13:
        case 14: return "0.9.9";
        case 15:
        case 16: return "the first release after 0.9.9";
        default: return "a release newer than this one";
        }
    }

    // The single static POD interface handed to clients. It is the NEWEST revision
    // (APMF_API_v12), constant-initialized (the pointers are to static functions), so
    // it is valid the instant the DLL loads. Because each revision's leading members
    // are exactly the previous revision's (v9 extends v8 extends v7 extends v6
    // extends v5 extends v4, base laid out first), a v1..v8 client reading it through
    // its own struct pointer sees only its prefix. The v4 base subobject is
    // brace-initialized explicitly.
    //
    // ABI v10 (ch.19 kIntent_Travel) added NO function-pointer slot, so there is
    // deliberately no APMF_API_v10: the new intent rode the existing
    // RequestEx/Repoint/Release slots. ABI v11 appends the two space-query slots, so
    // the object is now an APMF_API_v11 (which extends v9 directly). `abiVersion`
    // reports 11; a v1..v10 client still reads exactly its own prefix. ABI v12 appends
    // the one travel leg-state slot, so the object is now an APMF_API_v12 (extends v11)
    // and `abiVersion` reports 12. ABI v13 (ch.20 kIntent_TargetPin) adds NO slot, like
    // v10: the object stays an APMF_API_v12 and `abiVersion` reports 13. ABI v14 (ch.21
    // kIntent_CombatEntry) adds NO slot either: still an APMF_API_v12, `abiVersion` 14.
    // ABI v15 (ch.22 kIntent_CombatReentryDeny) adds NO slot either: still an APMF_API_v12,
    // `abiVersion` 15. ABI v16 (ch.23 kIntent_PursuitLeash, reading the existing
    // param.target / param.fval) adds NO slot either: still an APMF_API_v12, `abiVersion` 16.
    // ABI v11 layout proof (review F8d): the two query slots start exactly where the
    // v9 prefix ends, so a v1..v10 client reading its own prefix never overlaps them.
    // offsetof on a derived struct is conditionally-supported; MSVC (the only compiler
    // this DLL is built with, CI) accepts it. The sizeof twin proves the same with no
    // offsetof at all: base first, two pointers appended, no padding between.
    static_assert(offsetof(APMF_API::APMF_API_v11, FindEmptySpace) == sizeof(APMF_API::APMF_API_v9),
                  "APMF_API_v11's first slot must start right after the v9 prefix");
    static_assert(sizeof(APMF_API::APMF_API_v11) == sizeof(APMF_API::APMF_API_v9) + 2 * sizeof(void*),
                  "APMF_API_v11 = the v9 prefix plus exactly two function pointers");
    // ABI v12 layout proof, same two forms: the one leg-state slot starts exactly where
    // the v11 prefix ends, and the struct is that prefix plus one pointer.
    static_assert(offsetof(APMF_API::APMF_API_v12, GetTravelLegState) == sizeof(APMF_API::APMF_API_v11),
                  "APMF_API_v12's slot must start right after the v11 prefix");
    static_assert(sizeof(APMF_API::APMF_API_v12) == sizeof(APMF_API::APMF_API_v11) + sizeof(void*),
                  "APMF_API_v12 = the v11 prefix plus exactly one function pointer");

    constexpr APMF_API::APMF_API_v12 g_api{
        {
        {
        {
            {
                {
                    {
                        {
                            APMF_API::kABIVersion,
                            &APMF_Request,
                            &APMF_Release,
                            &APMF_RequestEx,
                            &APMF_Repoint,
                            &APMF_SetSpellAllowList,
                        },
                        &APMF_RequestCast,
                    },
                    &APMF_GetCastProxy,
                    &APMF_IsClaimLive,
                },
                &APMF_SetEquipSet,
            },
            &APMF_SetEquipSetEx,
            &APMF_IsEquipAuthorityEnforced,
        },
        &APMF_SetEquipScope,
        },
        &APMF_FindEmptySpace,
        &APMF_FindHostilesInSpace,
        },
        &APMF_GetTravelLegState,
    };

}

// Exported, undecorated (extern "C"). A client does
// GetProcAddress(GetModuleHandleA("APMF.dll"), "APMF_GetInterface"). Returns the
// base type; a v2 client checks p->abiVersion and casts up to APMF_API_v2*.
extern "C" __declspec(dllexport) const APMF_API::APMF_API_v1* APMF_GetInterface(std::uint32_t abiVersion) {
    try {
        if (abiVersion > APMF_API::kABIVersion) {
            // Name the running APMF and the one the client needs (SEV-3 F4): with
            // this null every channel the client would use stays off, and the only
            // diagnosis a user gets is this line.
            const auto* self = SKSE::PluginDeclaration::GetSingleton();
            spdlog::warn("[api] APMF_GetInterface: client wants ABI v{} but this APMF ({}) implements v{} -- returning "
                         "null, so EVERY facet that client would claim stays off. The client needs APMF {} or newer "
                         "(the first release implementing ABI v{}).",
                         abiVersion, self ? self->GetVersion().string(".") : "version unknown", APMF_API::kABIVersion,
                         MinReleaseForAbi(abiVersion), abiVersion);
            return nullptr;   // we cannot satisfy a newer contract than we implement
        }
        spdlog::info("[api] APMF_GetInterface: handed v{} interface to a client (requested v{}).",
                     APMF_API::kABIVersion, abiVersion);
        // Hand back the base-type view of the newest struct; every shipped revision
        // shares v1's identical initial sequence, so this is layout-safe.
        return reinterpret_cast<const APMF_API::APMF_API_v1*>(&g_api);
    } catch (...) {
        return nullptr;   // an spdlog throw must not unwind into the client
    }
}
