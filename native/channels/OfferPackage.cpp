#include "PCH.h"
#include "core/ControlMap.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Registry.h"
#include "core/PackageGate.h"

// ============================================================================
// Channel 9 -- PACKAGE-PROCEDURE ACTIVITY. Arbitration + claim lifecycle +
// the EvaluatePackage nudge live here; the real ENFORCEMENT (what
// CheckForCurrentAliasPackage actually RETURNS) lives in core/PackageGate.cpp
// (T3, graduated 2026-09-03 from the field-proven AliasPkgProbe,
// Docs/PROBE-ALLOWANCE.md "Probe 2" -- PROVEN for Phases 1-2).
//
// A client claims this facet naming the TESPackage FormID to offer
// (APMF_Param::form). PackageGate.cpp's 0x49 hook then returns that package
// for the claim's actor instead of the framework's own answer -- the engine
// adopts it and runs it NATIVELY (real pathing/procedures), one
// OnPackageChange each way. The CLIENT owns the package's own runtime target
// (a targType-0 handle, or whatever the package itself resolves against);
// APMF only delivers whichever FormID the winning claim names -- it never
// picks, validates, or interprets the package's own content.
//
// ---------------------------------------------------------------------------
// THE NUDGE, AND WHY IT IS DEFERRED (fix, 2026-09-06)
// ---------------------------------------------------------------------------
// One EvaluatePackage(true,false) nudge on Engage/OnOwnerChanged (so the
// redirect takes within one eval instead of waiting for the engine's own
// natural poll) and on Release (so the framework package resumes
// immediately) -- exactly the AliasPkgProbe.cpp mechanism.
//
// That nudge MUST NOT be issued from inside these lifecycle calls. They run
// inside ControlMap::Drain's apply loop (core/ControlMap.cpp:514 Engage,
// :575 Release), and Drain only Publish()es the new snapshot AFTER that loop
// (:261). The 0x49 thunk answers off the PUBLISHED snapshot
// (core/PackageGate.cpp:122 -> ControlMap::TryGetOwningClaim, :759), so a
// nudge issued here re-evaluates against the PREVIOUS generation:
//   * ENGAGE  -- the hook sees NO claim and hands back the framework's own
//                package. The offer is never adopted.
//   * RELEASE -- the hook still sees the claim and asserts the offered
//                package at the exact instant it is being withdrawn.
// FIELD EVIDENCE (Tuxbornrc1, 19:51-19:59, MFO aae6df64 / APMF 69c57bde,
// Docs/DIAG-2026-09-06-loot-travel.md): 0 of 6 MFO loot dispatches put a
// follower on the offered travel package (14 `onTravelPkg=false` lines), the
// first `[ch.9-redirect]` for each dispatch printed at the RELEASE and never
// at the dispatch, and the 19:57:37 heartbeat read `claimedActorsNow=2,
// claimedHits=0` six seconds into two live claims. All 16 claimed hits in the
// session reconcile to explicit nudges that happened to fire while an OLDER
// claim was already published; the engine's own evaluation cadence
// contributed ZERO. The original AliasPkgProbe had the order right (claim
// first, nudge one frame later); graduating it into the Channel lifecycle
// kept the call and inverted the ordering on both edges.
//
// So both edges post the nudge through apmf::mainthread::Post, which runs one
// hop past this Drain's Publish (Arbiter::OncePerFrame does Drain() then
// Pump()) -- the same idiom, for the same ordering reason, that the ch.8b
// CastCompose eviction teardown already uses at core/ControlMap.cpp:478-489
// (Docs/INVARIANTS.md #20). By the time the posted nudge runs, the hook sees
// exactly the state the nudge is about.
//
// The deferred nudge re-validates instead of trusting what it captured: it
// carries an RE::ActorHandle (never a raw Actor*) plus the claim state it was
// posted FOR, and drops itself if the actor no longer resolves or if the
// actor's ch.9 claim is no longer the one it was posted for (a newer claim
// posted its own nudge and that one wins; a stale release nudge must never
// re-assert a package that has since been re-offered, or vice versa). A
// dropped nudge is LOGGED and nothing else happens -- no retry, no watchdog,
// no re-assert loop (CLAUDE.md principle 7; Channel.h "a re-assert loop is a
// FAILED block").
//
// Every posted nudge prints a `[ch.9-nudge]` line when it FIRES, with the
// actor's package read back immediately afterwards. That is the observable
// (CLAUDE.md principle 5): a dispatch that works now shows CLAIMED ->
// [ch.9-nudge] FIRED -> [ch.9-redirect] within one frame, instead of the
// silence this bug produced.
// ============================================================================

namespace {

    // Post an EvaluatePackage nudge to run one hop past this Drain's Publish.
    // `expectClaim`/`wantForm` describe the ch.9 state this nudge is FOR:
    //   engage / re-point -> expectClaim=true,  wantForm = the offered package
    //   release           -> expectClaim=false, wantForm = 0
    // `edge` must be a string literal (captured by pointer for the log lines).
    void PostDeferredNudge(RE::FormID id, RE::Actor* actor, bool expectClaim,
                           RE::FormID wantForm, const char* edge) {
        // Channel.h: `actor` MAY BE NULL (a form the ControlMap could not resolve).
        // Nothing to nudge -- say so rather than dropping it silently.
        if (!actor) {
            spdlog::info("[ch.9-nudge] 0x{} {} nudge NOT POSTED -- actor does not resolve.",
                         apmf::log::Hex(id), edge);
            return;
        }

        const RE::ActorHandle handle = actor->GetHandle();
        apmf::mainthread::Post([id, handle, expectClaim, wantForm, edge] {
            auto  ptr = handle.get();   // NiPointer<Actor>; null once unloaded/deleted
            auto* a   = ptr.get();
            if (!a) {
                spdlog::info("[ch.9-nudge] 0x{} {} nudge DROPPED -- actor unloaded before it ran.",
                             apmf::log::Hex(id), edge);
                return;
            }

            // Re-validate against the NOW-PUBLISHED snapshot: only nudge for the
            // state this nudge was posted for. Anything else means a newer claim
            // (or release) landed in between and posted its own nudge.
            APMF_API::APMF_Param now{};
            const bool claimed = apmf::ControlMap::Get().TryGetOwningClaim(
                id, APMF_API::kIntent_OfferPackage, now);
            if (claimed != expectClaim || (expectClaim && now.form != wantForm)) {
                spdlog::info("[ch.9-nudge] 0x{} {} nudge DROPPED (stale) -- posted for claim={} "
                             "form=0x{}, now claim={} form=0x{}; a newer claim owns this edge.",
                             apmf::log::Hex(id), edge, expectClaim, apmf::log::Hex(wantForm),
                             claimed, apmf::log::Hex(now.form));
                return;
            }

            apmf::packagegate::EvaluatePackage(a);
            const auto* cur = a->GetCurrentPackage();
            spdlog::info("[ch.9-nudge] 0x{} {} nudge FIRED post-publish (claim={} form=0x{}); "
                         "curPkg now 0x{}. The 0x49 hook saw the published state.",
                         apmf::log::Hex(id), edge, claimed, apmf::log::Hex(now.form),
                         apmf::log::Hex(cur ? cur->GetFormID() : 0));
        });
    }

    class OfferPackageChannel final : public apmf::Channel {
    public:
        const char*      Name() const override { return "offer-package"; }
        int              ChannelNo() const override { return 9; }
        APMF_API::Intent ServesIntent() const override { return APMF_API::kIntent_OfferPackage; }

        std::span<const apmf::Hotkey> Hotkeys() const override {
            static constexpr apmf::Hotkey keys[] = {
                { 0xB5, "NumpadSlash : CLAIM the package-offer facet (arbitration only; the test surface "
                        "carries no package, so a test claim offers nothing -- see APMF_RequestEx)" },
            };
            return keys;
        }

        void Engage(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.9] 0x{} package-offer facet CLAIMED (package 0x{}). core/PackageGate.cpp's 0x49 "
                         "hook is what actually redirects the actor's package answer.",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
            PostDeferredNudge(id, actor, true, param.form, "engage");
        }

        void OnOwnerChanged(RE::FormID id, RE::Actor* actor, const APMF_API::APMF_Param& param) override {
            spdlog::info("[ch.9] 0x{} package-offer claim RE-POINTED (package 0x{}).",
                         apmf::log::Hex(id), apmf::log::Hex(param.form));
            PostDeferredNudge(id, actor, true, param.form, "re-point");
        }

        void Release(RE::FormID id, RE::Actor* actor) override {
            spdlog::info("[ch.9] 0x{} package-offer facet released -- framework package resumes.",
                         apmf::log::Hex(id));
            PostDeferredNudge(id, actor, false, 0, "release");
        }
    };

}

APMF_REGISTER_CHANNEL(OfferPackageChannel);
