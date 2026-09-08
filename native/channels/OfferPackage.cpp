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
// natural poll) and on Release (so the framework package resumes immediately)
// -- exactly the AliasPkgProbe.cpp mechanism.
//
// That nudge MUST NOT be issued from inside these lifecycle calls. They run
// inside ControlMap::Drain's apply loop (core/ControlMap.cpp:514 Engage,
// :575 Release), and Drain only Publish()es the new snapshot AFTER that loop
// (:261). The 0x49 thunk answers off the PUBLISHED snapshot
// (core/PackageGate.cpp -> ControlMap::TryGetOwningClaim), so a nudge issued
// here re-evaluates against the PREVIOUS generation:
//   * ENGAGE  -- the hook sees NO claim and hands back the framework's own
//                package. The offer is never adopted.
//   * RELEASE -- the hook still sees the claim and asserts the offered
//                package at the exact instant it is being withdrawn.
// The original AliasPkgProbe had the order right (claim first, nudge one frame
// later); graduating it into the Channel lifecycle kept the call and inverted
// the ordering on both edges. Right thread, wrong MOMENT.
//
// FIELD EVIDENCE for that failure -- 6 loot dispatches, 0 adoptions, every
// [ch.9-redirect] printing at the RELEASE and none at the dispatch -- is
// recorded once, in MFO's Docs/DIAG-2026-09-06-loot-travel.md. It is not
// restated here: session timestamps and DLL hashes rot in a source header.
//
// So both edges post the nudge through apmf::mainthread::Post, which runs one
// hop past this Drain's Publish (Arbiter::OncePerFrame does Drain() then
// Pump()) -- the same idiom, for the same ordering reason, that ch.8b's two
// existing deferred teardowns already use (Docs/INVARIANTS.md #20): the
// cast-claim EVICTION teardown in ControlMap::ApplyRequest
// (core/ControlMap.cpp:477) and CastComposeChannel::Release's proxy teardown
// (channels/CastCompose.cpp:126). (Naming corrected 2026-09-06: the eviction
// teardown is ControlMap's, not CastCompose's -- the earlier "CastCompose
// eviction teardown at core/ControlMap.cpp" conflated the two.) By the time
// the posted nudge runs, the hook sees exactly the state the nudge is about.
//
// RE-VALIDATION CONTRACT (this is load-bearing, not defensive padding -- a
// posted task outlives the moment it was posted for):
//   * it carries an RE::ActorHandle, never a raw Actor*;
//   * it is NOT POSTED at all unless the actor resolves AND already holds a
//     live engine handle (core/PackageGate.cpp's nudge is an engine AI write,
//     and Actor::GetHandle MINTS a handle for an actor that has none -- see
//     PostDeferredNudge below);
//   * at execution it re-reads the NOW-PUBLISHED ch.9 claim and drops itself
//     unless that claim is still the one it was posted for -- a newer claim
//     posted its own nudge and that one wins; a stale release nudge must never
//     re-assert a package that has since been re-offered, or vice versa.
// A dropped nudge is LOGGED and nothing else happens -- no retry, no watchdog,
// no re-assert loop (CLAUDE.md principle 7; Channel.h "a re-assert loop is a
// FAILED block").
//
// Every posted nudge prints a `[ch.9-nudge]` line when it FIRES, with the
// actor's package read back immediately afterwards. That is the observable
// (CLAUDE.md principle 5): a dispatch that works now shows CLAIMED ->
// [ch.9-nudge] FIRED -> [ch.9-redirect] within one frame, instead of the
// silence this bug produced.
//
// DEPENDENCY NOTE: this is the only channel that includes core/ControlMap.h
// and reads the map back. That is allowed (an any-thread lock-free RCU read),
// and core/Channel.h now says so and why.
// ============================================================================

namespace {

    // Post an EvaluatePackage nudge to run one hop past this Drain's Publish.
    // `expectClaim`/`wantForm` describe the ch.9 state this nudge is FOR:
    //   engage / re-point -> expectClaim=true,  wantForm = the offered package
    //   release           -> expectClaim=false, wantForm = 0
    // `edge` must be a string literal (captured by pointer for the log lines).
    void PostDeferredNudge(RE::FormID id, RE::Actor* actor, bool expectClaim,
                           RE::FormID wantForm, const char* edge) {
        // GATE 1 -- do not post at all unless there is a live actor to nudge.
        //
        // Two distinct ways this fires, and BOTH must be caught HERE, before
        // GetHandle:
        //   (a) Channel.h: `actor` MAY BE NULL (a form the ControlMap could not
        //       resolve).
        //   (b) the actor resolves but its engine handle has been INVALIDATED --
        //       ControlMap's unload sweep (core/ControlMap.cpp:249-252) reaches
        //       Release with exactly this shape: `ctl.handle.get()` came back null,
        //       then LookupByID handed back the still-allocated form.
        //
        // Case (b) is why this gate is not just `!actor`. The commit that
        // introduced this deferral claimed the unload sweep would be caught later,
        // by the posted task's own `handle.get()` check -- IT WOULD NOT.
        // Actor::GetHandle (RE::BSPointerHandleManagerInterface<Actor>::GetHandle,
        // RELOCATION_ID(15967, 16212)) MINTS A FRESH, VALID HANDLE for an actor
        // that has none, so that check would pass and the nudge would fire an
        // EvaluatePackage at an actor APMF has just declared unloaded. Benign (it
        // is the same engine call the old inline code made), but a guard that does
        // not exist must not be written down as if it did.
        //
        // RE::BSHandleRefObject::IsHandleValid() (CommonLibSSE-NG
        // include/RE/B/BSHandleRefObject.h; TESObjectREFR inherits it) is a plain
        // read of the kHandleValid bit (1 << 10) in the object's own refcount word
        // -- no relocation, no vfunc, nothing version-fragile. Every actor the
        // ControlMap controls already has a minted handle (core/ControlMap.cpp:440
        // does `npc.handle = actor->GetHandle()` at claim time), so a live claim's
        // engage / re-point / release always passes this gate.
        if (!actor || !actor->IsHandleValid()) {
            spdlog::info("[ch.9-nudge] 0x{} {} nudge NOT POSTED -- {}.",
                         apmf::log::Hex(id), edge,
                         actor ? "actor resolves but its engine handle is already invalid (unloaded)"
                               : "actor does not resolve");
            return;
        }

        const RE::ActorHandle handle = actor->GetHandle();
        apmf::mainthread::Post([id, handle, expectClaim, wantForm, edge] {
            // GATE 2 -- the actor went away between the post and this Pump. Note
            // what this does and does not prove: the handle was VALID when it was
            // captured (gate 1), so a null here is a real teardown in between. It
            // is NOT the unload-sweep guard -- that is gate 1's job, above.
            auto  ptr = handle.get();   // NiPointer<Actor>; null once unloaded/deleted
            auto* a   = ptr.get();
            if (!a) {
                spdlog::info("[ch.9-nudge] 0x{} {} nudge DROPPED -- actor unloaded before it ran.",
                             apmf::log::Hex(id), edge);
                return;
            }

            // GATE 3 -- re-validate against the NOW-PUBLISHED snapshot: only nudge for the
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
            // FORGET FIRST, THEN NUDGE -- the order is deliberate, not incidental.
            //
            // This is the edge that KNOWS the claim went away; core/PackageGate.cpp's
            // own no-claim branch can only learn it if the 0x49 hook happens to be
            // consulted with no claim standing, and that consult is NOT guaranteed
            // here: a release plus a same-form re-request landing in ONE Drain leaves
            // the release nudge correctly DROPPED as stale at gate 3 (the claim is
            // already back by Pump time), so 0x49 is never called with no claim, the
            // re-engage produces a byte-identical answer tuple, and the RULE D dedup
            // eats the line. The redirect works; the pass criterion reads failure.
            //
            // THE ERASE IS ITSELF POSTED -- IT IS NOT SAFE TO RUN HERE (F4-2,
            // 2026-09-07). This Release runs inside ControlMap::Drain's apply loop,
            // BEFORE Drain Publish()es the snapshot the 0x49 thunk answers from, so an
            // erase performed inline lands in exactly the window where the PUBLISHED
            // map still holds the claim. A combat-thread 0x49 consult in that window
            // takes the thunk's claim-present path and RE-INSERTS the byte-identical
            // tuple (core/PackageGate.cpp's try_emplace); a release plus a same-form
            // re-request landing in ONE Drain then hands the re-engage a remembered
            // tuple again, the RULE D dedup swallows the line, and the false negative
            // this erase exists to remove is back. Same class of bug as the nudge
            // itself: right thread, wrong MOMENT.
            //
            // Posting it fixes that by ORDER, not by luck. mainthread::Pump runs one
            // hop past this Drain's Publish, in FIFO order, so:
            //   * queued FIRST (ahead of both nudges posted below and of any nudge a
            //     re-request applied later in this same Drain posts), the erase always
            //     runs before the 0x49 consult a nudge causes;
            //   * by the time it runs the new generation is published, so a consult
            //     racing it either sees NO claim (the thunk's own no-claim backstop
            //     erases) or sees the re-offered claim and inserts a tuple that PRINTS
            //     on its own. Either way the dispatch stays visible in the log.
            // A bare Post, NOT routed through PostDeferredNudge, because the erase must
            // be unconditional: gate 1 legitimately refuses to post a nudge at all (the
            // ControlMap unload sweep reaches this Release with an already-invalid
            // handle) and the remembered tuple must be dropped in that case too. It
            // captures only the FormID -- no actor, no handle, nothing to re-validate:
            // erasing a log memory for an actor that has since gone away is a no-op by
            // construction (idempotent, one mutex + one hash erase).
            //
            // THE ONE PATH THAT DROPS IT, stated rather than buried: at kPreLoadGame
            // ReleaseAll drives this Release and plugin.cpp then calls
            // mainthread::Discard(), so this task never runs. That is correct and costs
            // nothing -- the world boundary erases WHOLESALE through
            // packagegate::ForgetAllRedirects() on the revert callback, and the thunk's
            // no-claim backstop catches anything after it. A missed erase can only ever
            // suppress a LOG LINE; it can never change what 0x49 returns.
            apmf::mainthread::Post([id] { apmf::packagegate::ForgetRedirect(id); });
            PostDeferredNudge(id, actor, false, 0, "release");
        }
    };

}

APMF_REGISTER_CHANNEL(OfferPackageChannel);
