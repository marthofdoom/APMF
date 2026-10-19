#include "PCH.h"
#include "core/Sources.h"   // Harbinger 1a: seat census
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/NonAliasProbe.h"
#include "core/PackageGate.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Win32 INI read for the redirect-probe log gate below. Declared by hand, exactly
// like core/EquipGate.cpp / core/ActionGate.cpp / core/AiCastSeats.cpp do -- PCH
// does not pull in <Windows.h>, and this is the single Win32 call this file needs.
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

// ============================================================================
// T3 -- CheckForCurrentAliasPackage (ch.9, Docs/CHANNEL-MAP.md). Graduated
// (2026-09-03) from the field-proven AliasPkgProbe (Docs/PROBE-ALLOWANCE.md
// "Probe 2" -- PROVEN for Phases 1-2, engage/release; Phase 3 save/load
// unexercised but the mechanism -- drop the claim without an engine call on
// kPreLoadGame -- is now the GENERIC ControlMap::ReleaseAll/Clear path every
// channel already gets for free, so this gate needs no bespoke Phase-3 code
// at all, unlike the probe which hand-rolled ClearOnPreLoad()).
//
// SAME hook (VTABLE_Character[0] slot 0x49 ONLY -- never PlayerCharacter, the
// §0.38 scar), SAME never-null contract (a claimed actor whose named package
// FormID doesn't resolve falls back to the engine's own answer, never a
// fabricated null -- §0.25 "claimed with nothing = rooted"), SAME
// EvaluatePackage(true,false) nudge.
//
// WHERE THE NUDGE IS ISSUED FROM (corrected 2026-09-06 -- the earlier claim in
// this header, that OfferPackage.cpp's Engage/OnOwnerChanged/Release call it
// directly because they "already run on the game thread", is RETRACTED). Being
// on the game thread was never the whole contract: those lifecycle calls run
// INSIDE ControlMap::Drain's apply loop, i.e. BEFORE Drain Publish()es the
// snapshot this thunk answers from, so a nudge issued there re-evaluates
// against the PREVIOUS generation and the claim it is about is invisible.
// channels/OfferPackage.cpp therefore POSTS the nudge through
// apmf::mainthread::Post, which runs one hop later (Arbiter::OncePerFrame does
// Drain() then Pump()) -- the same one-frame-later ordering AliasPkgProbe's
// OncePerFrame pump had. See OfferPackage.cpp's header for the full rationale.
//
// WHAT'S NEW vs the probe: the offered package is no longer one compile-time
// constant (`kProbePackageForm`) handed to every claimed actor -- it is
// APMF_Param::form on a REAL ControlMap claim (APMF_API::kIntent_OfferPackage),
// read via the lock-free RCU TryGetOwningClaim (Docs/ALLOWANCE-TEMPLATE.md
// §3, same discipline every T2 thunk already uses -- no mutex, no
// follower-list touch). The CLIENT decides which package to offer and owns
// that package's own runtime target (a targType-0 handle, or whatever the
// package itself resolves against); APMF only delivers whichever FormID the
// winning claim names.
// ============================================================================

namespace apmf::packagegate {

    namespace {

        std::atomic<bool> g_installed{ false };

        // ========================================================================
        // "Does 0x49 actually redirect?" probe (marth 2026-09-06). Answers the
        // question marth's field read raised: an offered package might CLAIM and
        // ENGAGE (ch.9 logs) while the follower is really just trailing the player
        // and looting whatever it passes, with this hook never actually winning.
        // FULLY PASSIVE -- reads the SAME orig/result/claim this thunk already
        // computed below, never a new lookup, never a behavior change.
        //
        // RULE C (ActionGate.cpp's PfpHeartbeat precedent): two-level cumulative
        // counters, printed EVEN AT ZERO by Heartbeat() below so "never fires" is a
        // visible zero line, not silence indistinguishable from "nothing to log."
        //   anchor  -- every 0x49 call seen, ANY actor: proves the thunk itself is
        //              alive regardless of whether anyone currently holds a claim.
        //   claimed -- the subset where the calling actor holds a winning
        //              kIntent_OfferPackage claim: the number that answers "does
        //              0x49 get CONSULTED for our claimed actor at all."
        //   won     -- the subset of `claimed` where the claim's named package was
        //              the one actually handed back to the engine.
        std::atomic<bool>     g_redirectLogEnabled{ true };   // [PackageGate] EnableRedirectLog, default ON
        std::atomic<std::uint64_t> g_redirectAnchorHits{ 0 };
        std::atomic<std::uint64_t> g_redirectClaimedHits{ 0 };
        std::atomic<std::uint64_t> g_redirectWinHits{ 0 };

        // RULE D -- dedup by TRANSITION, never a bare timer (a prior probe printed
        // 37 identical lines of a stable condition through a 1.5s throttle). One
        // line per (actor, answer-tuple); logged again only on first sighting per
        // actor or when the answer changes. Combat-thread calls here are rare
        // (package re-evaluation cadence, not per-frame), so a small mutex is fine
        // -- same shape as ActionGate.cpp's mvcbt throttle table.
        struct RedirectAnswer {
            RE::FormID orig;
            RE::FormID result;
            RE::FormID claimForm;
            // ALWAYS TRUE in a stored tuple, and deliberately so: only the
            // claim-present path STORES, and the no-claim paths ERASE rather than
            // record a "no claim" answer (see ForgetRedirect below and the backstop
            // branch in the thunk). Kept in the struct, and in the `==`, so the
            // tuple stays a faithful record of the answer it describes -- but do not
            // read it as evidence that no-claim tuples are ever held here.
            bool       claimPresent;
            bool operator==(const RedirectAnswer&) const = default;
        };
        std::mutex                                        g_redirectMx;
        std::unordered_map<RE::FormID, RedirectAnswer>    g_redirectLast;
        // Non-zero iff g_redirectLast holds at least one entry. Lets the
        // no-claim path below skip the mutex entirely in the overwhelmingly
        // common case (every unclaimed actor in the game calls 0x49), so the
        // FIX below costs one relaxed load there and nothing else.
        std::atomic<std::uint32_t>                        g_redirectRemembered{ 0 };

        // Defensive session line cap (RULE E) -- guards against a pathological
        // flip-flop actor spamming the log. Counters above are NEVER capped.
        //
        // RESIZED + MADE LOUD (round-2 review, 2026-09-06). The old 600 was sized
        // for a line-per-ANOMALY probe. It is not the right size any more: the
        // erase-on-release fix deliberately makes this at LEAST one line per ch.9
        // dispatch, plus one per engine-original flip while claimed (combat
        // interrupts do this), so a long multi-follower loot session reaches 600 --
        // and past it every later dispatch prints nothing, which is exactly the
        // false-negative the erase fix exists to remove. Sized from the real cadence
        // rather than a guess (principle 9), and the crossing is announced ONCE
        // (principle 7: a budget that truncates the evidence must never do it
        // silently). The per-actor dedup, not this cap, is what bounds normal churn.
        constexpr std::uint64_t    kRedirectLineCap = 4000;
        std::atomic<std::uint64_t> g_redirectLineCount{ 0 };

        constexpr std::uint64_t    kRedirectHeartbeatMs = 30000;   // ~30s, matches ActionGate.cpp's mvcbt cadence
        std::atomic<std::uint64_t> g_redirectLastHeartbeatMs{ 0 };
        // ========================================================================
        // RANKED 0x49 (marth 2026-10-18, ClickUp 86e3t1gfh / 86e3t1gfp): "name AND ENFORCE at the 0x49
        // seat". The engine's own answer (`orig`) is the alias package the actor's quest aliases offer it
        // RIGHT NOW: Character slot 0x49 (AE 37398 0x66CB60, SE 0x5DB180, 1.7.104 0x67F620; the three are
        // instruction-identical) returns null for a dead actor or the 0x800000 life-state, else tail-jumps
        // to the ExtraDataList alias-package lookup (AE 12057 -> GetByType(0x88 ExtraAliasInstanceArray) ->
        // 12721 0x182390; SE 0x126A00), which, under the array's read lock, walks every alias instance's
        // instanced packages and returns the FIRST one whose conditions + schedule pass (AE 29619
        // 0x4962E0), or null. So:
        //   STANDING (marth 2026-10-18, round 2: "Plain following isn't active") = the engine's answer is a
        //     package whose every procedure is standing companion behaviour (follow, wait, sandbox, sit,
        //     sleep, eat, idle marker, wander, keep an eye on, done; ClassifyPackage below, the table in
        //     Docs/FACETS.md ch.9). Treated as IDLE: the claim's package is served, nothing is counted
        //     against the mod, and the [ch.9-rank] line names the owner and the kind.
        //   ACTIVE (marth 2026-10-18: "theyll stop winning if Auri.esp is actively doing anything") = the
        //     engine's answer is non-null, is not the claimed package, and is not standing: some quest alias
        //     has a package of its own doing something more, valid for this actor now. Its owner is the package's plugin
        //     (`orig->GetFile(0)`, registered as a plugin source if new; a runtime-created package with no
        //     file is the Engine). The claim's client priority on the Package facet (the offer's client, or
        //     ch.19's internal offer's travel claim's client) must STRICTLY outrank the owner's: a tie keeps
        //     the follower mod's package (the facet rule; marth ruling (c)). Outranked -> the claim's package
        //     is served (owner Blocked); otherwise the original stands (owner Passed).
        //   IDLE = the engine's answer is null (no alias package of any mod is valid now) or already the
        //     claimed package: nothing to rank against, the claim's package is served, nothing is counted
        //     against a source.
        // Source counters move once per transition per actor (RankAndDecide), logged once per transition per actor
        // ([ch.9-rank]). Runs on engine AI threads: the RCU snapshot, the lock-free registry, atomics, and
        // this leaf mutex (never held across an engine call or a form lookup).
        enum class RankDecision : std::uint8_t { kIdle, kStanding, kOutranks, kOwn, kYields, kError };
        struct RankLast {
            RE::FormID              claimForm;
            RE::FormID              orig;
            apmf::sources::SourceId owner;
            RankDecision            decision;
            const char*             kind;   // a string literal from the classification table (pointer compare)
            bool operator==(const RankLast&) const = default;
        };
        std::mutex                                 g_rankMx;
        std::unordered_map<RE::FormID, RankLast>   g_rankLast;
        std::atomic<std::uint32_t>                 g_rankRemembered{ 0 };
        constexpr std::uint64_t                    kRankLineCap = 4000;   // same budget as [ch.9-redirect]
        std::atomic<std::uint64_t>                 g_rankLineCount{ 0 };
        std::atomic<std::uint64_t>                 g_rankYields{ 0 };     // heartbeat: consults the claim yielded
        std::atomic<std::uint64_t>                 g_rankOutranks{ 0 };   // heartbeat: consults it outranked an active package

        // ---- STANDING vs ACTIVE: what the engine's 0x49 answer DOES (round 2, marth 2026-10-18: "Plain following
        // isn't active"). Read from the package's own immutable form data, verified on 1.6.1170 / 1.5.97 / 1.7.104:
        //   * PKDT type = TESPackage::packData.packType, the byte at +0x24 (GetObjectTypeName AE 0x496D50 indexes the
        //     type-name table by [rcx+0x24]; IsPackageValid AE 29619 switches on it). The DERIVED +0xD8
        //     `procedureType` is NOT used: AE 29582 0x495010 sets it to 0x2E for every procedure-tree package.
        //   * GetCustomData (AE 29555 0x4926F0 / SE 0x4375C0 / 1.7.104 0x499C10, byte-identical): packType 18
        //     (Package) or 19 (PackageTemplate) -> TESPackage::data (+0x30) is a TESCustomPackageData, else null.
        //   * TESCustomPackageData::InitItem (AE 29658 0x497BB0 / SE 0x43C6B0 / 1.7.104 0x49F0D0): `templateParent`
        //     (+0x30) is the template package (form type 0x4F, packType 0x13); its `procedureTree` (+0x20) is copied
        //     into the instance's when the instance's version (+0x38) is not older, else the instance keeps null.
        //   * Tree nodes: BGSProcedureTreeProcedure::procedure at +0x10 (Copy: AE 0x482F70 / SE 0x4287F0 / 1.7.104
        //     0x48A490); Sequence / Stacked / Simultaneous / Random all share BGSProcedureTreeBranch::Copy (AE
        //     0x483470 / SE 0x428CD0 / 1.7.104 0x48A990): children BSTArray at +0x10, size at +0x20.
        // Node and procedure classes are named by their own RTTI (vtable[-1] -> CompleteObjectLocator ->
        // TypeDescriptor), never by an id. RttiName VALIDATES every hop before it reads it (review 34f2779 SEV-4:
        // this DLL is not built with /EHa, so a catch(...) does NOT catch an access violation and is no safety net
        // for a bad pointer): the vtable must lie in .rdata, the locator must lie in .rdata with signature 1 and a
        // self-RVA equal to its own RVA, the TypeDescriptor must lie in .data and its name must start ".?A" and end
        // within the segment. Anything that fails is "unreadable", which classifies ACTIVE (logged kind=unknown).
        //
        // THE RULE (review 34f2779 SEV-2: Travel is in about half of Skyrim.esm's templates, inside the vanilla
        // follower and sandbox templates too -- FollowerPackageTemplate 000D530D = Follow+Travel+Wait, FollowPlayer
        // 000750BE = Follow+Sandbox+Travel, Sandbox 0001C254 = Sandbox+Travel+UnlockDoors). Each procedure is
        // STANDING (Guard and Patrol included, marth 2026-10-19: routine posture like wait / sandbox), AUXILIARY (getting there and opening the way: Travel, Lock/Unlock doors, Find, Acquire) or
        // ACTIVE. A package is ACTIVE when any procedure in its tree is active; else STANDING when any is
        // standing (its auxiliaries only carry it there); else (auxiliaries only, a pure Travel package) ACTIVE.
        // Cached per plugin package (the data never changes after load): a leaf mutex, never held across a read.
        enum class ProcRole : std::uint8_t { kStanding, kAux, kActive };
        struct ProcKind { const char* rtti; const char* label; ProcRole role; };
        constexpr ProcKind kProcKinds[] = {
            { ".?AVBGSProcedureFollow@@",           "follow",         ProcRole::kStanding },
            { ".?AVBGSProcedureFollowTo@@",         "follow",         ProcRole::kStanding },
            { ".?AVBGSProcedureWait@@",             "wait",           ProcRole::kStanding },
            { ".?AVBGSProcedureHoldPosition@@",     "wait",           ProcRole::kStanding },
            { ".?AVBGSProcedureSandbox@@",          "sandbox",        ProcRole::kStanding },
            { ".?AVBGSProcedureSit@@",              "sit",            ProcRole::kStanding },
            { ".?AVBGSProcedureSleep@@",            "sleep",          ProcRole::kStanding },
            { ".?AVBGSProcedureEat@@",              "eat",            ProcRole::kStanding },
            { ".?AVBGSProcedureUseIdleMarker@@",    "idle marker",    ProcRole::kStanding },
            { ".?AVBGSProcedureWander@@",           "wander",         ProcRole::kStanding },
            { ".?AVBGSProcedureKeepAnEyeOn@@",      "keep an eye on", ProcRole::kStanding },
            { ".?AVBGSProcedureDone@@",             "done",           ProcRole::kStanding },
            { ".?AVBGSProcedureTravel@@",           "travel",         ProcRole::kAux },
            { ".?AVBGSProcedureLock@@",             "lock doors",     ProcRole::kAux },
            { ".?AVBGSProcedureUnlock@@",           "unlock doors",   ProcRole::kAux },
            { ".?AVBGSProcedureFind@@",             "find",           ProcRole::kAux },
            { ".?AVBGSProcedureAcquire@@",          "acquire",        ProcRole::kAux },
            { ".?AVBGSProcedureEscort@@",           "escort",         ProcRole::kActive },
            { ".?AVBGSProcedureDialogue@@",         "dialogue",       ProcRole::kActive },
            { ".?AVBGSProcedureDialogueActivate@@", "dialogue",       ProcRole::kActive },
            { ".?AVBGSProcedureForceGreet@@",       "force greet",    ProcRole::kActive },
            { ".?AVBGSProcedureSay@@",              "say",            ProcRole::kActive },
            { ".?AVBGSProcedureShout@@",            "shout",          ProcRole::kActive },
            { ".?AVBGSProcedureActivate@@",         "activate",       ProcRole::kActive },
            { ".?AVBGSProcedureUseMagic@@",         "use magic",      ProcRole::kActive },
            { ".?AVBGSProcedureUseWeapon@@",        "use weapon",     ProcRole::kActive },
            { ".?AVBGSProcedurePatrol@@",           "patrol",         ProcRole::kStanding },   // marth: routine posture
            { ".?AVBGSProcedureGuard@@",            "guard",          ProcRole::kStanding },   // marth: routine posture
            { ".?AVBGSProcedureFlee@@",             "flee",           ProcRole::kActive },
            { ".?AVBGSProcedurePursue@@",           "pursue",         ProcRole::kActive },
            { ".?AVBGSProcedureHover@@",            "hover",          ProcRole::kActive },
            { ".?AVBGSProcedureOrbit@@",            "orbit",          ProcRole::kActive },
            { ".?AVBGSProcedureFlightGrab@@",       "flight grab",    ProcRole::kActive },
        };
        constexpr const char* kKindUnknown = "unknown";
        // A package without a procedure tree (an old PKDT-typed package): its type decides.
        struct TypeKind { std::uint8_t type; const char* label; };
        constexpr TypeKind kStandingTypes[] = {
            { 1, "follow" }, { 3, "eat" }, { 4, "sleep" }, { 5, "wander" }, { 7, "follow" }, { 12, "sandbox" }, { 13, "patrol" }, { 14, "guard" }, { 27, "wait" },
        };
        constexpr const char* kKindOtherType = "other type";

        struct Classified { bool standing = false; const char* label = kKindUnknown; };

        // The RTTI name of a polymorphic engine object, or null when ANY hop fails validation (see above).
        const char* RttiName(const void* a_obj) {
            if (!a_obj) return nullptr;
            static const auto  base  = REL::Module::get().base();
            static const auto  rdata = REL::Module::get().segment(REL::Segment::rdata);
            static const auto  data  = REL::Module::get().segment(REL::Segment::data);
            const auto inRdata = [](std::uintptr_t a, std::size_t n) {
                return a >= rdata.address() && a + n <= rdata.address() + rdata.size() && a + n >= a;
            };
            const auto vt = *reinterpret_cast<const std::uintptr_t*>(a_obj);
            if (vt < sizeof(void*) || !inRdata(vt - sizeof(void*), sizeof(void*))) return nullptr;
            const auto colAddr = *reinterpret_cast<const std::uintptr_t*>(vt - sizeof(void*));
            // x64 CompleteObjectLocator: signature@0 (1), offset@4, cdOffset@8, typeDescriptor RVA@0xC,
            // classDescriptor RVA@0x10, self RVA@0x14.
            if (colAddr < base || !inRdata(colAddr, 0x18)) return nullptr;
            const auto* col = reinterpret_cast<const std::uint32_t*>(colAddr);
            if (col[0] != 1 || col[5] != static_cast<std::uint32_t>(colAddr - base)) return nullptr;
            const std::uintptr_t td = base + col[3];
            // TypeDescriptor: vftable@0, spare@8, name@0x10 (NUL-terminated, ".?AV...@@" / ".?AU...@@").
            constexpr std::size_t kMaxName = 128;
            if (td < data.address() || td + 0x10 + 4 > data.address() + data.size()) return nullptr;
            const char*       name  = reinterpret_cast<const char*>(td + 0x10);
            const std::size_t avail = std::min<std::size_t>(kMaxName, data.address() + data.size() - (td + 0x10));
            if (name[0] != '.' || name[1] != '?' || name[2] != 'A') return nullptr;
            if (!std::memchr(name, '\0', avail)) return nullptr;
            return name;
        }

        struct WalkState {
            int         budget        = 256;
            const char* activeLabel   = nullptr;   // the first active procedure (or "unknown")
            const char* standingLabel = nullptr;   // the first standing procedure
            const char* auxLabel      = nullptr;   // the first auxiliary procedure
        };
        // Walks one tree; stops at the first active procedure or unknown node. Bounded (depth 8, 256 nodes).
        void WalkTree(const RE::IProcedureTreeItem* a_node, int a_depth, WalkState& a_st) {
            if (!a_node || a_st.activeLabel) return;
            if (a_depth > 8 || --a_st.budget < 0) { a_st.activeLabel = kKindUnknown; return; }
            const char* name = RttiName(a_node);
            if (!name) { a_st.activeLabel = kKindUnknown; return; }
            if (std::strcmp(name, ".?AVBGSProcedureTreeProcedure@@") == 0) {
                const auto* proc  = static_cast<const RE::BGSProcedureTreeProcedure*>(a_node)->procedure;
                const char* pname = RttiName(proc);
                if (!pname) { a_st.activeLabel = kKindUnknown; return; }
                for (const auto& k : kProcKinds) {
                    if (std::strcmp(pname, k.rtti) != 0) continue;
                    if (k.role == ProcRole::kActive) a_st.activeLabel = k.label;
                    else if (k.role == ProcRole::kStanding) { if (!a_st.standingLabel) a_st.standingLabel = k.label; }
                    else if (!a_st.auxLabel) a_st.auxLabel = k.label;
                    return;
                }
                a_st.activeLabel = kKindUnknown;   // a procedure class this table does not know
                return;
            }
            if (std::strcmp(name, ".?AVBGSProcedureTreeSequence@@") == 0 ||
                std::strcmp(name, ".?AVBGSProcedureTreeStacked@@") == 0 ||
                std::strcmp(name, ".?AVBGSProcedureTreeSimultaneous@@") == 0 ||
                std::strcmp(name, ".?AVBGSProcedureTreeRandom@@") == 0) {
                const auto& kids = static_cast<const RE::BGSProcedureTreeBranch*>(a_node)->conditions;
                for (std::uint32_t i = 0; i < kids.size() && !a_st.activeLabel; ++i)
                    WalkTree(kids[i], a_depth + 1, a_st);
                return;
            }
            a_st.activeLabel = kKindUnknown;   // a node class this table does not know
        }

        Classified ClassifyUncached(const RE::TESPackage* a_pkg) {
            Classified c;
            const auto type = static_cast<std::uint8_t>(a_pkg->packData.packType.underlying());
            if (type != 18 && type != 19) {   // no procedure tree: the PKDT type is the procedure
                for (const auto& t : kStandingTypes)
                    if (t.type == type) { c.standing = true; c.label = t.label; return c; }
                c.label = kKindOtherType;
                return c;
            }
            // The tree: the package's own, else its template chain's (an instance older than its template keeps none).
            const RE::IProcedureTreeItem* tree = nullptr;
            const RE::TESPackage*         p    = a_pkg;
            for (int hop = 0; hop < 4 && p && !tree; ++hop) {
                const auto t = static_cast<std::uint8_t>(p->packData.packType.underlying());
                if (t != 18 && t != 19) break;
                const auto* cd = static_cast<const RE::TESCustomPackageData*>(p->data);
                if (!cd) break;
                tree = cd->procedureTree;
                p    = cd->templateParent;
            }
            if (!tree) return c;   // unknown -> active
            WalkState st;
            WalkTree(tree, 0, st);
            if (st.activeLabel) { c.label = st.activeLabel; return c; }
            if (st.standingLabel) { c.standing = true; c.label = st.standingLabel; return c; }
            c.label = st.auxLabel ? st.auxLabel : kKindUnknown;   // auxiliaries only (a pure Travel package), or empty: active
            return c;
        }

        std::mutex                                                   g_kindMx;
        std::unordered_map<const RE::TESPackage*, Classified>        g_kindCache;
        // Only a package loaded from a plugin is cached: a runtime-created package (no file) can be freed and its
        // address reused, so it is classified on every consult. (No try/catch: nothing here throws, and an access
        // violation is not catchable without /EHa; RttiName's validation is the protection.)
        Classified ClassifyPackage(const RE::TESPackage* a_pkg) {
            const bool cacheable = a_pkg->GetFile(0) != nullptr;
            if (cacheable) {
                std::scoped_lock lock(g_kindMx);
                if (const auto it = g_kindCache.find(a_pkg); it != g_kindCache.end()) return it->second;
            }
            const Classified c = ClassifyUncached(a_pkg);
            if (!cacheable) return c;
            std::scoped_lock lock(g_kindMx);
            if (g_kindCache.size() >= 2048) g_kindCache.clear();
            g_kindCache.emplace(a_pkg, c);
            return c;
        }

        // Review 34f2779 SEV-5: the package's plugin -> its source id, cached per TESFile so an ACTIVE consult on
        // the AI thread does not scan the source table by name every time. Leaf mutex, bounded.
        std::mutex                                                    g_fileSrcMx;
        std::unordered_map<const RE::TESFile*, apmf::sources::SourceId> g_fileSrc;
        apmf::sources::SourceId SourceForFile(const RE::TESFile* a_file) {
            {
                std::scoped_lock lock(g_fileSrcMx);
                if (const auto it = g_fileSrc.find(a_file); it != g_fileSrc.end()) return it->second;
            }
            const auto id = apmf::sources::Register(a_file->GetFilename(), apmf::sources::Kind::kPlugin);
            if (id == apmf::sources::kNoSource) return id;   // not cached: retried next time
            std::scoped_lock lock(g_fileSrcMx);
            if (g_fileSrc.size() >= 1024) g_fileSrc.clear();
            g_fileSrc.emplace(a_file, id);
            return id;
        }

        // The claim the Package facet is enforced FOR (core/PackageSources.cpp / core/SpectatorGate.cpp
        // ClaimPrio, same rule): the offer's own client, or, for ch.19's internal offer, the winning travel
        // claim's client.
        int RankClaimPrio(RE::FormID a_actor, APMF_API::Handle a_offer, apmf::sources::SourceId& a_client) {
            APMF_API::Handle h = a_offer;
            if (apmf::sources::ClientForHandle(h) == apmf::sources::kNoSource) {
                APMF_API::APMF_Param p{};
                float                b  = 0.0f;
                APMF_API::Handle     th = APMF_API::kInvalidHandle;
                if (apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, APMF_API::kIntent_Travel, p, b, &th)) h = th;
            }
            a_client = apmf::sources::kNoSource;
            return apmf::sources::ClaimPriority(h, apmf::sources::Facet::kPackage, &a_client);
        }

        // Decide whether the claim's package replaces the engine's answer. Logs; never throws (a registry
        // failure keeps the claim winning, as before ranking existed, and says so).
        // COUNTING (review 34f2779 SEV-4 #3): the source counters (Hit, Blocked = overruled, Passed = stood, own)
        // move ONCE PER TRANSITION per actor, like the package poll and the menu's "a package takeover is counted
        // once", not once per 0x49 consult. Served (NoteServedClaim in the thunk) stays per consult: it is what keeps
        // the claim's client "seen" for the menu winner. The heartbeat's rankOutranks / rankYields stay per consult.
        bool RankAndDecide(RE::Actor* a_actor, const RE::TESPackage* a_orig, const RE::TESPackage* a_claimed,
                           APMF_API::Handle a_claimH) {
            namespace src = apmf::sources;
            const RE::FormID actorId = a_actor->GetFormID();
            RankLast         now{ a_claimed->GetFormID(), a_orig ? a_orig->GetFormID() : 0, src::kNoSource,
                                  RankDecision::kIdle, nullptr };
            std::string      ownerFile;   // the standing package's plugin, for the log only (not registered)
            int              ownerPrio = 0, claimPrio = 0;
            src::SourceId    client = src::kNoSource;
            bool             wins   = true;
            try {
                const Classified kind = (a_orig && a_orig != a_claimed) ? ClassifyPackage(a_orig) : Classified{};
                if (a_orig && a_orig != a_claimed) now.kind = kind.label;
                if (a_orig && a_orig != a_claimed && kind.standing) {
                    // STANDING companion behaviour: plain following is not active. Served, nothing counted.
                    now.decision = RankDecision::kStanding;
                    if (const auto* f = a_orig->GetFile(0)) ownerFile = std::string(f->GetFilename());
                    else ownerFile = "a runtime package";
                } else if (a_orig && a_orig != a_claimed) {
                    // ACTIVE: a quest alias's own package doing more than standing behaviour is valid now. Name its owner.
                    if (const auto* f = a_orig->GetFile(0))
                        now.owner = SourceForFile(f);
                    else
                        now.owner = src::kEngineSource;   // created at runtime: no plugin
                    if (now.owner == src::kNoSource) now.owner = src::kUnattributedSource;
                    claimPrio = RankClaimPrio(actorId, a_claimH, client);
                    if (now.owner == client) {
                        now.decision = RankDecision::kOwn;   // the claim's own client's package: no outside write
                    } else {
                        ownerPrio = src::Priority(now.owner, src::Facet::kPackage);
                        // the degrade rule: an unattributed package is never displaced unless overridden
                        const bool rankable = now.owner != src::kUnattributedSource ||
                                              src::OverrideUnknown(src::Facet::kPackage);
                        wins         = rankable && claimPrio > ownerPrio;   // strictly: a tie keeps the mod's package
                        now.decision = wins ? RankDecision::kOutranks : RankDecision::kYields;
                        (wins ? g_rankOutranks : g_rankYields).fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } catch (...) {
                wins         = true;   // pre-ranking behaviour; announced below, never silent
                now.decision = RankDecision::kError;
            }
            try {
                bool changed = false;
                {
                    std::scoped_lock lock(g_rankMx);
                    auto [it, inserted] = g_rankLast.try_emplace(actorId, now);
                    if (inserted) {
                        g_rankRemembered.fetch_add(1, std::memory_order_relaxed);
                        changed = true;
                    } else if (!(it->second == now)) {
                        it->second = now;
                        changed    = true;
                    }
                }
                if (changed) {
                    // The source counters, once per transition (see above).
                    switch (now.decision) {
                    case RankDecision::kOwn:
                        src::NoteOwn(now.owner, src::Facet::kPackage);
                        break;
                    case RankDecision::kOutranks:
                        src::NoteHit(now.owner, src::Facet::kPackage);
                        src::NoteOverruled(now.owner, src::Facet::kPackage);
                        break;
                    case RankDecision::kYields:
                        src::NoteHit(now.owner, src::Facet::kPackage);
                        src::NoteStood(now.owner, src::Facet::kPackage);
                        break;
                    default:
                        break;
                    }
                    const auto prior = g_rankLineCount.fetch_add(1, std::memory_order_relaxed);
                    if (prior < kRankLineCap) {
                        const std::string owner = now.owner != src::kNoSource ? std::string(src::Name(now.owner)) : std::string();
                        const std::string cname = client != src::kNoSource ? std::string(src::Name(client))
                                                                           : std::string("an internal claim");
                        switch (now.decision) {
                        case RankDecision::kIdle:
                            spdlog::info("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{}: no other alias package is "
                                         "valid for the actor (the follower mod is idle) -> the claim's package is served.",
                                         apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig));
                            break;
                        case RankDecision::kStanding:
                            spdlog::info("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{} owner={} kind={} -> idle: "
                                         "standing companion behaviour is not active (marth: plain following isn't active), "
                                         "the claim's package is served.",
                                         apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig),
                                         ownerFile, now.kind ? now.kind : kKindUnknown);
                            break;
                        case RankDecision::kOwn:
                            spdlog::info("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{}: the active package is {}'s, "
                                         "the claim's own client -> the claim's package is served.",
                                         apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig), owner);
                            break;
                        case RankDecision::kOutranks:
                            spdlog::info("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{} owner={} kind={}: ACTIVE (its "
                                         "package is valid now), Package priority {} vs the claim's {} ({}): the claim outranks "
                                         "it -> the claim's package is served (enforced).",
                                         apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig), owner,
                                         now.kind ? now.kind : kKindUnknown, ownerPrio, claimPrio, cname);
                            break;
                        case RankDecision::kYields:
                            spdlog::info("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{} owner={} kind={}: ACTIVE (its "
                                         "package is valid now), Package priority {} vs the claim's {} ({}): the claim does not "
                                         "strictly outrank it{} -> the claim YIELDS, {}'s package stands.",
                                         apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig), owner,
                                         now.kind ? now.kind : kKindUnknown, ownerPrio, claimPrio, cname,
                                         now.owner == src::kUnattributedSource ? " (unattributed: the degrade rule)"
                                         : ownerPrio == claimPrio              ? " (a tie keeps the follower mod's package)"
                                                                               : "",
                                         owner);
                            break;
                        case RankDecision::kError:
                            spdlog::error("[ch.9-rank] actor=0x{} claimForm=0x{} engineOrig=0x{}: ranking FAILED (a registry "
                                          "exception) -> the claim's package is served UNRANKED for this consult.",
                                          apmf::log::Hex(actorId), apmf::log::Hex(now.claimForm), apmf::log::Hex(now.orig));
                            break;
                        }
                    }
                    if (prior + 1 == kRankLineCap)
                        spdlog::warn("[ch.9-rank] SESSION LINE CAP ({}) REACHED -- further [ch.9-rank] lines are SUPPRESSED; "
                                     "the menu counters and the [ch.9-redirect] H totals stay authoritative.",
                                     kRankLineCap);
                }
            } catch (...) {
            }
            return wins;
        }

        void ForgetRank(RE::FormID a_actor) {
            if (g_rankRemembered.load(std::memory_order_relaxed) == 0) return;
            std::scoped_lock lock(g_rankMx);
            if (g_rankLast.erase(a_actor) != 0) g_rankRemembered.fetch_sub(1, std::memory_order_relaxed);
        }
        // ========================================================================

        struct PkgHook {
            static RE::TESPackage* thunk(RE::Actor* a_this) {
                RE::TESPackage* orig = func(a_this);   // the engine's own answer first
                RE::TESPackage* result = orig;         // mechanical refactor only (single exit for the
                                                        // OBSERVE-log addition below) -- every branch below
                                                        // is byte-identical to the prior early-return logic,
                                                        // `break` in place of `return`; behavior UNCHANGED.

                // Docs/SPEC-PACKAGE-HOLD.md §4.1 item 3: `claim`/`claimPresent` are
                // declared at function scope (not inside the do-while, as before)
                // purely so the OBSERVE-log below can report them -- same single
                // TryGetOwningClaim call the redirect logic already made, no new
                // lookup, no behavior change (the do-while's branches are
                // byte-identical to before this widening).
                APMF_API::APMF_Param claim{};
                APMF_API::Handle claimH = APMF_API::kInvalidHandle;   // Harbinger menu "Served" credit
                bool claimPresent = false;
                bool won = false;   // redirect-probe only: did OUR named package win this call?

                do {
                    if (apmf::ControlMap::Get().ControlledCount() == 0) break;   // near-zero cost

                    float            claimBasis = 0.0f;
                    if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_this->GetFormID(),
                                                                        APMF_API::kIntent_OfferPackage, claim,
                                                                        claimBasis, &claimH))
                        break;   // uncontrolled on this intent
                    claimPresent = true;
                    if (claim.form == 0) break;   // claimed but no package named -- channel default, no redirect

                    if (auto* pkg = RE::TESForm::LookupByID<RE::TESPackage>(claim.form)) {
                        // Ranked (2026-10-18): the claim's package replaces the engine's answer only when the
                        // follower mod behind that answer is idle, or the claim strictly outranks it.
                        if (!RankAndDecide(a_this, orig, pkg, claimH)) break;   // yields: the original stands
                        result = pkg;
                        won    = true;
                        apmf::sources::NoteServedClaim(claimH, apmf::sources::Facet::kPackage);   // menu "Served"
                        break;
                    }
                    // named FormID doesn't resolve -- degrade to the engine's own answer, never null
                } while (false);

                // "Does 0x49 actually redirect?" probe (marth 2026-09-06). RULE C
                // cumulative counters -- unconditional, no INI gate on the counting
                // itself (only the printed lines are gated), so Heartbeat() can
                // always report a true zero rather than "never counted." Reuses
                // orig/result/claim/claimPresent/won already computed above --
                // zero new lookups, zero behavior change.
                g_redirectAnchorHits.fetch_add(1, std::memory_order_relaxed);
                if (claimPresent) {
                    g_redirectClaimedHits.fetch_add(1, std::memory_order_relaxed);
                    if (won) g_redirectWinHits.fetch_add(1, std::memory_order_relaxed);

                    if (g_redirectLogEnabled.load(std::memory_order_relaxed)) {
                        const RE::FormID actorId = a_this->GetFormID();
                        const RedirectAnswer answer{
                            orig ? orig->GetFormID() : 0,
                            result ? result->GetFormID() : 0,
                            claim.form,
                            claimPresent,
                        };
                        bool changed = false;
                        {
                            std::scoped_lock lock(g_redirectMx);
                            auto [it, inserted] = g_redirectLast.try_emplace(actorId, answer);
                            if (inserted) {
                                g_redirectRemembered.fetch_add(1, std::memory_order_relaxed);
                                changed = true;
                            } else if (!(it->second == answer)) {
                                it->second = answer;
                                changed = true;
                            }
                        }
                        if (changed) {
                            // Capture fetch_add's PRIOR value so the crossing can be
                            // announced exactly once, by whichever call consumes the
                            // last allowed slot -- never silently (principle 7).
                            const auto prior =
                                g_redirectLineCount.fetch_add(1, std::memory_order_relaxed);
                            if (prior < kRedirectLineCap) {
                                spdlog::info("[ch.9-redirect] actor=0x{} engineOrig=0x{} apmfResult=0x{} "
                                             "claimForm=0x{} won={}",
                                             apmf::log::Hex(actorId), apmf::log::Hex(answer.orig),
                                             apmf::log::Hex(answer.result), apmf::log::Hex(answer.claimForm),
                                             won);
                            }
                            if (prior + 1 == kRedirectLineCap) {
                                spdlog::warn("[ch.9-redirect] SESSION LINE CAP ({}) REACHED -- further "
                                             "[ch.9-redirect] lines are SUPPRESSED for the rest of this "
                                             "session. The cumulative counters in the 30s heartbeat are "
                                             "NOT capped and remain authoritative; do not read the "
                                             "silence after this line as 'no redirects'.",
                                             kRedirectLineCap);
                            }
                        }
                    }
                } else if (g_redirectLogEnabled.load(std::memory_order_relaxed) &&
                           g_redirectRemembered.load(std::memory_order_relaxed) != 0) {
                    // BACKSTOP erase (the PRIMARY one is ForgetRedirect below, driven
                    // from the ch.9 release edge). Forget the actor's remembered answer
                    // whenever 0x49 is consulted for it with NO claim standing.
                    //
                    // WHY BOTH, and why this one is NOT the load-bearing half (round-2
                    // review, 2026-09-06). This branch can only fire if the hook is
                    // actually CONSULTED with no claim, and that consult is NOT
                    // guaranteed between a release and the next same-form claim -- a
                    // release plus a re-request inside ONE Drain leaves the release
                    // nudge correctly dropped as stale, so 0x49 is never called with no
                    // claim and the identical tuple survives into the re-engage. So the
                    // release EDGE erases (ForgetRedirect), and this stays as the
                    // backstop for the paths that drop a claim with NO channel Release
                    // at all -- specifically ControlMap::Clear() (revert / new game),
                    // which by design makes no Release calls. Two idempotent erase
                    // sites, and erasing can only ever cause an EXTRA line, never a
                    // missing one, so the belt and the braces cannot disagree.
                    //
                    // Erasing rather than recording a no-claim tuple is deliberate:
                    // this branch runs for EVERY unclaimed actor in the game, and a
                    // "no claim, no redirect" line for each of them is noise, not
                    // information (principle 8). Erase makes the next claim on this
                    // actor a fresh insert, which prints.
                    const RE::FormID actorId = a_this->GetFormID();
                    std::scoped_lock lock(g_redirectMx);
                    if (g_redirectLast.erase(actorId) != 0)
                        g_redirectRemembered.fetch_sub(1, std::memory_order_relaxed);
                }

                // OBSERVE-ONLY (Docs/PROBE-NONALIAS-PACKAGE.md §6.1, extended per
                // Docs/SPEC-PACKAGE-HOLD.md §4.1 item 2/3): does this hook even get
                // CALLED for a non-alias-package actor (e.g. Cicero, 0009BE51), and
                // is the claim continuously present when it does? Gated behind
                // core/NonAliasProbe.h's NumLock switch + shared per-actor rate limit
                // -- OFF by default, never changes `result`. Uses
                // Actor::GetCurrentPackage() (a plain accessor) rather than
                // hand-walking AIProcess::currentPackage's raw struct -- see
                // NonAliasProbe.cpp's file header for why. `tick=` uses the SAME
                // monotonic axis (nonaliasprobe::MonotonicMs()) as the new §4.1
                // item-1 periodic poll (NonAliasProbe.cpp's PollClaimedPackages,
                // "[ch.9-poll]") so the two independent log lines interleave into one
                // readable timeline.
                if (apmf::nonaliasprobe::IsEnabled() &&
                    apmf::nonaliasprobe::RateLimitOK(a_this->GetFormID())) {
                    const auto* cur = a_this->GetCurrentPackage();
                    // TESPackage has no GetPackageType() method -- the type is
                    // plain data, TESPackage::procedureType (a
                    // stl::enumeration<PACKAGE_PROCEDURE_TYPE, uint32_t> at
                    // +0xD8; see core/NonAliasProbe.cpp's file header for the
                    // real-header citation this was corrected against).
                    spdlog::info("[ch.9-observe] tick={} 0x49 CheckForCurrentAliasPackage actor=0x{} curPkg=0x{} "
                                 "curPkgType={} engineOrig=0x{} hookReturns=0x{} claim={} claimForm=0x{}",
                                 apmf::nonaliasprobe::MonotonicMs(),
                                 apmf::log::Hex(a_this->GetFormID()),
                                 apmf::log::Hex(cur ? cur->GetFormID() : 0),
                                 cur ? static_cast<std::int32_t>(cur->procedureType.underlying()) : -1,
                                 apmf::log::Hex(orig ? orig->GetFormID() : 0),
                                 apmf::log::Hex(result ? result->GetFormID() : 0),
                                 claimPresent ? "present" : "ABSENT",
                                 apmf::log::Hex(claim.form));
                }

                return result;
            }
            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0x49;   // Actor::CheckForCurrentAliasPackage
        };

    }

    void Install() {
        if (REL::Module::IsVR()) {
            spdlog::warn("[ch.9] VR runtime -- 0x49 index + EvaluatePackage reloc are SE/AE only; "
                         "package-offer allowance NOT installed.");
            return;
        }
        if (!allowance::RuntimeSupported()) {   // G1: exact build, never the 1.6 path by bucket
            spdlog::error("[ch.9] runtime {} is not exactly 1.6.1170, 1.5.97 or 1.7.104 -- the 0x49 index and the "
                          "EvaluatePackage id are verified on those three only; package-offer allowance NOT "
                          "installed (REFUSED).",
                          REL::Module::get().version().string("."));
            return;
        }
        if (g_installed.exchange(true)) return;

        REL::Relocation<std::uintptr_t> charVtbl{ RE::VTABLE_Character[0] };
        if (!allowance::SeatVerified(charVtbl.address(), "PackageGate.Character.CheckForCurrentAliasPackage")) {
            spdlog::error("[ch.9] package-offer allowance NOT installed (self-check refused the Character vtable).");
            return;
        }
        PkgHook::func = charVtbl.write_vfunc(PkgHook::idx, PkgHook::thunk);
        apmf::sources::NoteVfunc(apmf::sources::Facet::kPackage, "PackageGate.Character.CheckForCurrentAliasPackage",
                                 charVtbl.address(), PkgHook::idx, SKSE::stl::unrestricted_cast<std::uintptr_t>(&PkgHook::thunk),
                                 PkgHook::func.address());

        // "Does 0x49 actually redirect?" probe -- default ON (read-only, low
        // volume: RULE D dedup-on-transition + a 600-line/session defensive cap;
        // see the anon-namespace block above). [PackageGate] EnableRedirectLog=0
        // in Data/SKSE/Plugins/APMF.ini disables both the per-transition
        // [ch.9-redirect] lines and the [ch.9-redirect] H heartbeat; the
        // underlying counters themselves are always tallied regardless (near-zero
        // cost -- three relaxed atomic increments) so toggling the flag mid-session
        // never loses history.
        g_redirectLogEnabled.store(GetPrivateProfileIntA("PackageGate", "EnableRedirectLog", 1,
                                                          "Data/SKSE/Plugins/APMF.ini") != 0,
                                   std::memory_order_relaxed);

        spdlog::info("[ch.9] package-offer allowance hooked (Character::CheckForCurrentAliasPackage, 0x49) -- "
                     "a kIntent_OfferPackage claim with APMF_Param::form set to a TESPackage FormID redirects "
                     "that actor's alias-package answer to it; the engine runs it natively. Redirect-probe "
                     "logging ([ch.9-redirect]) is {} ([PackageGate] EnableRedirectLog, DEFAULT ON).",
                     g_redirectLogEnabled.load(std::memory_order_relaxed) ? "ARMED" : "disabled by INI");
    }

    void ForgetRedirect(RE::FormID a_actor) {
        // See PackageGate.h for WHY the release edge -- not the hook -- has to drive
        // this. Same two lines as the thunk's backstop branch, same mutex, and the
        // same relaxed size counter so the backstop's cheap pre-gate stays accurate.
        ForgetRank(a_actor);   // the [ch.9-rank] transition memory follows the same release edge
        if (g_redirectRemembered.load(std::memory_order_relaxed) == 0) return;
        std::scoped_lock lock(g_redirectMx);
        if (g_redirectLast.erase(a_actor) != 0)
            g_redirectRemembered.fetch_sub(1, std::memory_order_relaxed);
    }

    bool RankedAtSeat(RE::FormID a_actor, RE::FormID a_pkg) {
        if (g_rankRemembered.load(std::memory_order_relaxed) == 0 || a_pkg == 0) return false;
        std::scoped_lock lock(g_rankMx);
        const auto it = g_rankLast.find(a_actor);
        return it != g_rankLast.end() && it->second.orig == a_pkg;
    }

    void ForgetAllRedirects() {
        // See PackageGate.h for WHY the world boundary needs its own wholesale
        // erase: Clear() releases nothing and consults nothing, so neither the
        // release edge nor the thunk's backstop can reach these entries.
        if (g_rankRemembered.load(std::memory_order_relaxed) != 0) {
            std::scoped_lock lock(g_rankMx);
            g_rankLast.clear();
            g_rankRemembered.store(0, std::memory_order_relaxed);
        }
        if (g_redirectRemembered.load(std::memory_order_relaxed) == 0) return;
        std::scoped_lock lock(g_redirectMx);
        g_redirectLast.clear();
        g_redirectRemembered.store(0, std::memory_order_relaxed);
    }

    void EvaluatePackage(RE::Actor* a_actor) {
        if (!a_actor) return;
        using func_t = void (*)(RE::Actor*, bool, bool);
        static REL::Relocation<func_t> func{ RELOCATION_ID(36407, 37401) };
        // mit-3.7 F1: refused (logged once) unless the self-check verified the address.
        static const bool verified = allowance::SeatVerified(func.address(), "PackageGate.EvaluatePackage");
        if (!verified) return;
        func(a_actor, true, false);   // resetAI MUST stay false -- never a full AI reset
    }

    void Heartbeat() {
        // RULE C -- prints ONCE per ~30s interval, INCLUDING ZERO counts, so
        // silence is never mistaken for "0x49 never fires." Cheap when disabled or
        // unclaimed: one relaxed atomic-bool load (plus, once armed, one more for
        // the throttle) -- no ControlMap touch unless the interval has elapsed.
        if (!g_redirectLogEnabled.load(std::memory_order_relaxed)) return;

        const auto now  = apmf::clock::MonotonicMs();
        const auto last = g_redirectLastHeartbeatMs.load(std::memory_order_relaxed);
        if (now - last < kRedirectHeartbeatMs) return;
        g_redirectLastHeartbeatMs.store(now, std::memory_order_relaxed);   // main-thread-only writer, no race

        // Read-only RCU snapshot (INVARIANTS #13: a small map) -- how many actors
        // are CURRENTLY claimed on kIntent_OfferPackage, for context alongside the
        // cumulative counters below.
        const auto claimedNow = apmf::ControlMap::Get().ClaimedActors(APMF_API::kIntent_OfferPackage).size();

        const auto lineCount = g_redirectLineCount.load(std::memory_order_relaxed);
        const auto dropped   = lineCount > kRedirectLineCap ? lineCount - kRedirectLineCap : 0;

        spdlog::info("[ch.9-redirect] H claimedActorsNow={} anchorHits={} claimedHits={} winHits={} "
                     "rankOutranks={} rankYields={} transitionLinesDropped={}",
                     claimedNow,
                     g_redirectAnchorHits.load(std::memory_order_relaxed),
                     g_redirectClaimedHits.load(std::memory_order_relaxed),
                     g_redirectWinHits.load(std::memory_order_relaxed),
                     g_rankOutranks.load(std::memory_order_relaxed),
                     g_rankYields.load(std::memory_order_relaxed),
                     dropped);
    }

}
