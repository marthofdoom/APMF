#include "PCH.h"
#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate
#include "core/Log.h"
#include "core/Clock.h"
#include "core/ControlMap.h"
#include "core/EquipSink.h"

#include <intrin.h>      // _ReturnAddress / _AddressOfReturnAddress (core/EquipGate.cpp precedent)
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <unordered_set>

// Win32 INI read for the three switches below. Declared by hand, exactly like
// core/EquipGate.cpp / core/CastClassify.cpp do -- PCH does not pull in
// <Windows.h>. The two module queries name the DLL an EXTERNAL equip caller
// lives in (diagnosis only; the verdict never depends on the name).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);
extern "C" __declspec(dllimport) int __stdcall GetModuleHandleExA(
    unsigned long a_flags, const char* a_moduleName, void** a_outModule);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(
    void* a_module, char* a_buf, unsigned long a_size);

// ============================================================================
// See core/EquipSink.h for the design and Docs/INVARIANTS.md #17a for the
// licence. This TU is the single sink thunk plus its install-time byte-verify.
// ============================================================================

namespace apmf::equipsink {

    namespace {

        constexpr const char* kIni = "Data/SKSE/Plugins/APMF.ini";

        // ---- The worker's 4th argument, as the two call sites build it on the
        // stack (AE EquipObject +0x113..+0x15d at rsp+0x28; SE +0x87..+0xd3; the
        // list sibling at rsp+0x20 with the dword 0x00010001 = queue/force/
        // sounds/applyNow). Natural layout reproduces the measured offsets; the
        // static_asserts pin them. Read-only here: the thunk never writes it. ----
        struct EquipData {
            RE::ExtraDataList*      extra;      // +0x00  EquipObject's a_extraData
            std::uint32_t           count;      // +0x08  a_count
            const RE::BGSEquipSlot* slot;       // +0x10  a_slot, or the default slot resolved by 38911/37955 (0x6cab20/0x638c30)
            std::uint64_t           zero;       // +0x18  always 0 at both sites
            bool                    queue;      // +0x20  a_queueEquip
            bool                    force;      // +0x21  a_forceEquip
            bool                    sounds;     // +0x22  a_playSounds
            bool                    applyNow;   // +0x23  a_applyNow
            bool                    tail;       // +0x24  always 0 at both sites
        };
        static_assert(offsetof(EquipData, extra)    == 0x00);
        static_assert(offsetof(EquipData, count)    == 0x08);
        static_assert(offsetof(EquipData, slot)     == 0x10);
        static_assert(offsetof(EquipData, zero)     == 0x18);
        static_assert(offsetof(EquipData, queue)    == 0x20);
        static_assert(offsetof(EquipData, force)    == 0x21);
        static_assert(offsetof(EquipData, sounds)   == 0x22);
        static_assert(offsetof(EquipData, applyNow) == 0x23);
        static_assert(offsetof(EquipData, tail)     == 0x24);

        // The worker: (ActorEquipManager*, Actor*, TESBoundObject*, EquipData*).
        // Four register args, no stack args read (prologue verified on both
        // binaries), return value UNUSED by both callers (AE +0x175 nop/lfence
        // then a call that overwrites eax; the sibling restores registers and
        // returns) -- so returning without calling it is ABI-clean.
        using Worker_t = void (*)(RE::ActorEquipManager*, RE::Actor*, RE::TESBoundObject*, EquipData*);

        // ---- Per-runtime site table (Docs/INVARIANTS.md #17a condition 3: from
        // the disassembly of THAT runtime, never a CommonLib declaration).
        // `depth` = byte offset from THIS thunk's own return-address slot to
        // EquipObject's CALLER's return-address slot, measured from the bytes:
        //   AE 38894 EquipObject : push rdi,r12,r13,r14,r15 + sub rsp,0x50 -> 8 + 0x50 + 5*8 = 0x80
        //   AE 38893 list sibling: push r12,r14,r15          + sub rsp,0x50 -> 8 + 0x50 + 3*8 = 0x70
        //   SE 37938 EquipObject : push rdi                  + sub rsp,0x50 -> 8 + 0x50 + 1*8 = 0x60
        //   SE 37937 list sibling: push r12,r14,r15          + sub rsp,0x50 -> 8 + 0x50 + 3*8 = 0x70
        // The 8 is this thunk's own return slot (the E8 at the site pushed it).
        struct SiteSpec {
            std::uint64_t id;      // Address-Library id of the function holding the site
            std::uint32_t off;     // byte offset of the E8 inside that function
            std::uint32_t depth;   // see above
            const char*   name;
        };
        constexpr SiteSpec kSitesAE[] = {
            { 38894, 0x170, 0x80, "EquipObject" },
            { 38893, 0x0BC, 0x70, "EquipObjectList" },
        };
        constexpr SiteSpec kSitesSE[] = {
            { 37938, 0x0E5, 0x60, "EquipObject" },
            { 37937, 0x0BC, 0x70, "EquipObjectList" },
        };
        constexpr std::uint64_t kWorkerAE = 38929;   // 1.6.1170 RVA 0x6CBE30
        constexpr std::uint64_t kWorkerSE = 37974;   // 1.5.97   RVA 0x639E20

        // ---- Caller path table (per runtime). An engine caller of EquipObject /
        // the list sibling whose id is not here logs as Unknown(<id>); a return
        // address outside SkyrimSE.exe logs as External(<module>). ----
        // kPlayerMenu (ABI v8): the player's own equips on the actor through the
        // trade/gift/follower-inventory menu -- ALLOWED by default (player agency,
        // marth 2026-09-15) unless the claim carries kEquipAuth_DenyPlayerMenu.
        enum class PathKind : std::uint8_t { kEngine, kScript, kConsole, kPlayerMenu };
        struct PathSpec {
            std::uint64_t id;
            const char*   name;
            PathKind      kind;
        };
        constexpr PathSpec kPathsAE[] = {
            { 418622, "OutfitApply",       PathKind::kEngine },
            { 19692,  "AddWornOutfit",     PathKind::kEngine },
            { 39649,  "AiCommand",         PathKind::kEngine },
            { 39650,  "AiCommand",         PathKind::kEngine },
            { 37797,  "RemoveItemReequip", PathKind::kEngine },
            { 19689,  "RemoveItemReequip", PathKind::kEngine },
            { 16059,  "RemoveItemReequip", PathKind::kEngine },
            { 48126,  "CombatNode",        PathKind::kEngine },
            { 48124,  "CombatNode",        PathKind::kEngine },
            { 54661,  "Script",            PathKind::kScript },
            { 22351,  "Console",           PathKind::kConsole },
            { 29383,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 29346,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 38907,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 38913,  "WorkerReentry",     PathKind::kEngine },   // the worker family's own re-entry (2nd copy / dual wield)
            // The DEFERRED APPLY of a queued equip (Fable tier-3 on d1aa66b, SEV-3 #1):
            // EquipObject(queue=1) -> worker +0x272 -> 37684 -> 39821 (AIProcess enqueue)
            // -> vtable task 34227 (its ONLY caller) -> 38906 -> EquipObject(queue=0) per
            // entry at 38906+0xFD. So APMF's OWN queued equips re-enter the seat from
            // here, one actor-update later, with tls=0. 39814 (+0xCB -> EquipObject) is
            // the other AIProcess-family caller (<- 39089/41381).
            { 38906,  "QueuedApply",       PathKind::kEngine },
            { 39814,  "QueuedApply",       PathKind::kEngine },
            // ---- Tier-C round after the first v8 deck log (2026-09-16): the one
            // criterion-3 failure was `Unknown(39637)` -- the AI equip-command
            // DISPATCHER itself (the switch over cmd 0..0x36) calls EquipObject
            // directly at +0xF47 (ret +0xF4C = 0x6F2FEC, the field line), not only
            // through its executors 39649/39650. Verified by an E8 scan of the
            // unpacked 1.6.1170 image against the address library; the same pass
            // swept every other engine caller of EquipObject / the sibling and
            // named what it could PROVE (caller chains + RTTI vtable membership):
            { 39637,  "AiCommand",         PathKind::kEngine },   // dispatcher: +0xF47 -> EquipObject; sole caller 39108
            { 39948,  "AiCommand",         PathKind::kEngine },   // +0x199 -> EquipObject; called ONLY from 39637+0x8B1
            { 39655,  "AiCommand",         PathKind::kEngine },   // +0x868 -> sibling;     called ONLY from 39637+0x195C
            { 39645,  "AiCommand",         PathKind::kEngine },   // +0x250 -> sibling; from 39108 (the dispatcher's caller) + 39209 (<- 39108)
            { 37510,  "AiCommand",         PathKind::kEngine },   // +0x226 -> sibling; every LIVE caller is the 39108 family (39223, 39653 <- 39637, 39654); 40087/40088 are orphans
            { 37520,  "DropObject",        PathKind::kEngine },   // +0x345 -> sibling; Actor vtable slot 0xCB (RTTI-read), Actor::DropObject in the pinned header
            { 37521,  "PickUpObject",      PathKind::kEngine },   // +0x437 -> sibling; Actor vtable slot 0xCC, Actor::PickUpObject
            { 38898,  "BoundItem",         PathKind::kEngine },   // +0x25F -> EquipObject; from BoundItemEffect vtable slot 4 (34229, ActiveEffect::Update): a conjured bound weapon equipping itself
            { 28422,  "ProcedureEat",      PathKind::kEngine },   // +0x17E -> sibling; from BGSProcedureEat vtable slot 0xC (28411) + 28417: the Eat package procedure equipping food (ALCH, ungoverned under v8)
            { 41244,  "InventoryReequip",  PathKind::kEngine },   // +0x150 -> EquipObject (+0x257 UnequipObject); from PlayerCharacter::UseAmmo (41243, slot 0xD2), PlayerCharacter::PickUpObject (40533, slot 0xCC) and InventoryChanges 16066
            { 38561,  "StartCombat",       PathKind::kEngine },   // +0x2BD -> sibling; Actor::StartCombat (alandtse binding 37608/38561, cross-checked: called from Actor vfunc 0x96 (37289), the 39652 executor, 37496/39695/40079/40814/41407)
            // NOT named, deliberately: 16104 (+0xCD -> EquipObject) is reached from
            // PlayerCharacter::ServePrisonTime (40657, slot 0xBA; player-only, never
            // seated) AND from 40702 <- 37925 <- console/script-range callers
            // (22338/22546/23008/54758) that could not be pinned -- a name would
            // misattribute route B. 52410 (+0x6F -> EquipObject) has NO reference
            // in .text/.rdata/.data (no E8/E9/lea/pointer): an orphan like 38919.
        };
        constexpr PathSpec kPathsSE[] = {
            { 24234,  "OutfitApply",       PathKind::kEngine },
            { 19266,  "AddWornOutfit",     PathKind::kEngine },
            { 38618,  "AiCommand",         PathKind::kEngine },
            { 38619,  "AiCommand",         PathKind::kEngine },
            { 36781,  "RemoveItemReequip", PathKind::kEngine },
            { 19263,  "RemoveItemReequip", PathKind::kEngine },
            { 15821,  "RemoveItemReequip", PathKind::kEngine },
            { 46957,  "CombatNode",        PathKind::kEngine },
            { 46955,  "CombatNode",        PathKind::kEngine },
            { 53861,  "Script",            PathKind::kScript },
            { 21869,  "Console",           PathKind::kConsole },
            { 28629,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 28593,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 37951,  "PlayerMenu",        PathKind::kPlayerMenu },
            { 37957,  "WorkerReentry",     PathKind::kEngine },
            { 37950,  "QueuedApply",       PathKind::kEngine },   // <- 33449 (its only caller); -> EquipObject at +0xF1
            { 38789,  "QueuedApply",       PathKind::kEngine },   // <- 38133/40367; -> EquipObject at +0xD1
            // ---- Tier-C round (2026-09-16): the 1.5.97 counterparts, each E8
            // verified on the unpacked 1.5.97 image with the SAME caller shape as
            // the AE row (RTTI vtable slots identical; see the AE table for the chains).
            { 38606,  "AiCommand",         PathKind::kEngine },   // dispatcher: +0xCCD -> EquipObject (ret 0x65FD12); sole caller 38150
            { 38902,  "AiCommand",         PathKind::kEngine },   // +0x1A0 -> EquipObject; called ONLY from 38606+0x798
            { 38624,  "AiCommand",         PathKind::kEngine },   // +0x857 -> sibling;     called ONLY from 38606+0x152F
            { 38614,  "AiCommand",         PathKind::kEngine },   // +0x250 -> sibling; from 38150 (the dispatcher's caller), 38227, 38249
            { 36510,  "AiCommand",         PathKind::kEngine },   // +0x226 -> sibling; callers 38263, 38622, 38623, 39020, 39021 (the 38150 family)
            { 36520,  "DropObject",        PathKind::kEngine },   // +0x345 -> sibling; Actor vtable slot 0xCB
            { 36521,  "PickUpObject",      PathKind::kEngine },   // +0x432 -> sibling; Actor vtable slot 0xCC
            { 37942,  "BoundItem",         PathKind::kEngine },   // +0x20A -> EquipObject; <- 33455 <- BoundItemEffect vtable slot 4 (33451)
            { 27700,  "ProcedureEat",      PathKind::kEngine },   // +0x16E -> sibling; <- BGSProcedureEat vtable slot 0xC (27689) + 27695
            { 40241,  "InventoryReequip",  PathKind::kEngine },   // +0x14D -> EquipObject; <- PlayerCharacter slots 0xD2 (40240) / 0xCC (39456) + InventoryChanges 15827
            { 37608,  "StartCombat",       PathKind::kEngine },   // +0x2C3 -> sibling; Actor::StartCombat (<- Actor vfunc 0x96 = 36299, 38621, 36496, 38667, 39012, 39712, 40393)
            // NOT named: 15864 (+0xCD -> EquipObject; <- PlayerCharacter::ServePrisonTime 39571 + 39616) and 51535 (+0x6F, no reference anywhere) -- same reasons as AE 16104 / 52410.
        };

        // ---- runtime state (written at Install on the main thread, read from any
        // thread afterwards; the atomics publish the install) ----
        struct InstalledSite {
            std::uintptr_t addr  = 0;   // absolute address of the E8
            std::uint32_t  depth = 0;
            std::uint64_t  id    = 0;
            std::uint32_t  off   = 0;
            const char*    name  = "";
        };
        std::atomic<bool>  g_installed{ false };
        // WHY the seat is not installed (ABI v8, MFO wiring review SEV-2 F1): read by
        // ControlMap::EnqueueRequest from any thread to REFUSE a ch.17 claim while the
        // seat is down. Always a string literal (never freed); cleared to "" on success.
        std::atomic<const char*> g_notInstalledReason{ "not yet installed (Install runs at kDataLoaded)" };
        std::atomic<bool>  g_observeOnly{ true };
        std::atomic<bool>  g_denyScript{ false };
        Worker_t           g_worker = nullptr;
        InstalledSite      g_sites[2]{};
        std::uintptr_t     g_base = 0, g_textBegin = 0, g_textEnd = 0;
        const PathSpec*    g_paths     = nullptr;
        std::size_t        g_pathCount = 0;
        // Sorted (offset -> id) copy of the Address Library, built once at
        // Install and immutable afterwards (concurrent readers are fine). NEVER
        // call its operator(): that is lower_bound + report_and_fail on a miss;
        // classification uses the public iterators with upper_bound - 1 instead.
        std::unique_ptr<REL::IDDatabase::Offset2ID> g_offset2id;

        thread_local int t_apmfDepth = 0;

        // ---- logging discipline: dedupe per (actor,item,path) for 2 s, global
        // cap 100 lines/s with a per-minute drop count (CLAUDE.md principle 8:
        // volume is not importance -- one dropped-count line beats a flood) ----
        constexpr std::uint64_t kDedupeMs   = 2000;
        constexpr std::uint32_t kCapPerSec  = 100;
        constexpr std::size_t   kDedupeMax  = 2048;
        std::mutex                                       g_logMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastLogMs;   // key -> ms
        std::uint64_t g_secStart = 0, g_minStart = 0;
        std::uint32_t g_secCount = 0, g_minDropped = 0;

        // Returns true if this line may be logged now; also emits the per-minute
        // drop report when one is due. `pending` receives the drop count to report
        // (0 == none) so the report itself is logged OUTSIDE the lock.
        bool AdmitLog(std::uint64_t key, std::uint64_t nowMs, std::uint32_t& pendingDrops) {
            pendingDrops = 0;
            std::scoped_lock lock(g_logMx);
            if (g_minStart == 0) g_minStart = nowMs;
            if (nowMs - g_minStart >= 60000) {
                pendingDrops = g_minDropped;
                g_minDropped = 0;
                g_minStart   = nowMs;
            }
            if (auto it = g_lastLogMs.find(key); it != g_lastLogMs.end() && nowMs - it->second < kDedupeMs)
                return false;   // dedupe: same (actor,item,path) inside the window -- not a drop
            if (nowMs - g_secStart >= 1000) { g_secStart = nowMs; g_secCount = 0; }
            if (g_secCount >= kCapPerSec) { ++g_minDropped; return false; }
            ++g_secCount;
            if (g_lastLogMs.size() >= kDedupeMax) {
                for (auto it = g_lastLogMs.begin(); it != g_lastLogMs.end();)
                    it = (nowMs - it->second >= kDedupeMs) ? g_lastLogMs.erase(it) : std::next(it);
                if (g_lastLogMs.size() >= kDedupeMax) g_lastLogMs.clear();   // still full: reset, never grow unbounded
            }
            g_lastLogMs[key] = nowMs;
            return true;
        }

        // ---- who asked? ----
        struct Caller {
            const char*    name  = "Unknown";
            PathKind       kind  = PathKind::kEngine;
            std::uint64_t  id    = 0;
            std::uintptr_t rva   = 0;      // inside the exe: the return address' RVA
            bool           known = false;  // true iff `name` came from the path table
            bool           external = false;
            char           ext[64]{};      // External: the module's base name
        };

        Caller Classify(std::uintptr_t ret) {
            Caller c;
            if (ret >= g_textBegin && ret < g_textEnd) {
                c.rva = ret - g_base;
                if (g_offset2id) {
                    // Greatest library offset <= rva == the function containing the
                    // return address. upper_bound on the sorted-by-offset table, then
                    // step back one (never operator(): it report_and_fails on a miss).
                    auto it = std::upper_bound(g_offset2id->begin(), g_offset2id->end(), c.rva,
                                               [](std::uintptr_t a, const auto& m) { return a < m.offset; });
                    if (it != g_offset2id->begin()) c.id = std::prev(it)->id;
                }
                for (std::size_t i = 0; i < g_pathCount; ++i) {
                    if (g_paths[i].id == c.id) { c.name = g_paths[i].name; c.kind = g_paths[i].kind; c.known = true; return c; }
                }
                return c;   // Unknown(<id>)
            }
            c.external = true;
            c.name     = "External";
            void* mod  = nullptr;
            // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x4) | UNCHANGED_REFCOUNT (0x2)
            if (GetModuleHandleExA(0x6, reinterpret_cast<const char*>(ret), &mod) && mod) {
                char path[260]{};
                const auto n = GetModuleFileNameA(mod, path, sizeof(path));
                if (n > 0 && n < sizeof(path)) {
                    const char* baseName = path;
                    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') baseName = p + 1;
                    std::strncpy(c.ext, baseName, sizeof(c.ext) - 1);
                    // SKSE's own Papyrus natives (EquipItemEx/EquipItemById) live in
                    // skse64_*.dll: a script path in every sense that matters.
                    if (std::strncmp(c.ext, "skse64", 6) == 0) c.kind = PathKind::kScript;
                }
            }
            if (c.ext[0] == '\0') std::strncpy(c.ext, "?", sizeof(c.ext) - 1);
            return c;
        }

        // ---- ENTRY-DETOUR INSPECTION (Fable tier-3 on d1aa66b SEV-3 #2; round 2
        // SEV-4: re-run after load events). The internal sites are byte-verified,
        // but a third party that inline-detours the PUBLIC entry of EquipObject /
        // the list sibling (the standard "intercept every equip" target) changes
        // what sits in the caller's return slot: it becomes the DETOUR's return, so
        // every caller classifies as External(<that dll>) -> kEngine -> the Script
        // exemption is silently unavailable and the probe cannot attribute
        // anything. The seat still installs (the deny at the worker is unaffected);
        // it says so, loudly, and only when the verdict CHANGES (first sight of a
        // detour, a different detour, or a detour gone). Main thread only, outside
        // any engine frame (kDataLoaded / kPostLoadGame / kNewGame). ----
        std::uintptr_t g_entrySites[2]{};
        std::uint64_t  g_entrySiteIds[2]{};
        const char*    g_entrySiteNames[2]{ "", "" };
        std::string    g_entryVerdict[2];   // "" == not yet inspected; "none" == clean; else "<dll>|<shape>"

        void InspectEntryDetours(const char* why) {
            for (int i = 0; i < 2; ++i) {
                const std::uintptr_t entry = g_entrySites[i];
                if (!entry) continue;
                std::uint8_t e[16]{};
                std::memcpy(e, reinterpret_cast<const void*>(entry), sizeof(e));
                std::uintptr_t target = 0;
                const char*    shape  = nullptr;
                if (e[0] == 0xE9) {                                   // jmp rel32
                    std::int32_t rel = 0; std::memcpy(&rel, e + 1, 4);
                    target = entry + 5 + static_cast<std::intptr_t>(rel); shape = "E9 jmp rel32";
                } else if (e[0] == 0xFF && e[1] == 0x25) {            // jmp [rip+disp32]
                    std::int32_t disp = 0; std::memcpy(&disp, e + 2, 4);
                    std::memcpy(&target, reinterpret_cast<const void*>(entry + 6 + static_cast<std::intptr_t>(disp)), sizeof(target));
                    shape = "FF 25 jmp [rip]";
                } else if (e[0] == 0x48 && e[1] == 0xB8) {            // mov rax, imm64 (; jmp rax)
                    std::memcpy(&target, e + 2, sizeof(target)); shape = "48 B8 mov rax,imm64";
                } else if (e[0] == 0xE8) {                            // call rel32 at entry -- not the engine's shape
                    std::int32_t rel = 0; std::memcpy(&rel, e + 1, 4);
                    target = entry + 5 + static_cast<std::intptr_t>(rel); shape = "E8 call rel32";
                }
                std::string verdict = "none";
                char module[64] = "?";
                if (shape) {
                    void* mod = nullptr;
                    if (target && GetModuleHandleExA(0x6, reinterpret_cast<const char*>(target), &mod) && mod) {
                        char path[260]{};
                        const auto n = GetModuleFileNameA(mod, path, sizeof(path));
                        if (n > 0 && n < sizeof(path)) {
                            const char* baseName = path;
                            for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') baseName = p + 1;
                            std::strncpy(module, baseName, sizeof(module) - 1);
                        }
                    }
                    verdict = std::string(module) + "|" + shape;
                }
                if (verdict == g_entryVerdict[i]) continue;   // unchanged since the last inspection
                const bool first = g_entryVerdict[i].empty();
                g_entryVerdict[i] = verdict;
                if (!shape) {
                    if (!first)
                        spdlog::info("[apmf][equip-sink] entry {} ({}) no longer detoured (at {}): attribution restored.",
                                     g_entrySiteIds[i], g_entrySiteNames[i], why);
                    continue;
                }
                spdlog::warn("[apmf][equip-sink] entry {} ({}) detoured by {} ({} -> 0x{}, seen at {}): attribution degraded, "
                             "every caller will classify as External({}); the Script/Console exemption is unavailable "
                             "and the probe criteria cannot attribute engine ids. The seat still installs (the deny "
                             "sits below the detour). A '?' module means the target is a trampoline page; the DLL "
                             "behind it is not resolved (REVIEW-BACKLOG APMF-B3).",
                             g_entrySiteIds[i], g_entrySiteNames[i], module, shape, apmf::log::Hex(target, 0), why, module);
            }
        }

        // ---- THE GOVERNED TYPES (ABI v8). The worker is the sink for EVERY
        // EquipObject, and EquipObject is also how a potion is drunk, food eaten,
        // a scroll read, an ingredient tasted, a book read (Actor::DrinkPotion and
        // the AI's own potion use end here too). Those are not the worn set, so
        // only ARMO / WEAP / AMMO / LIGH (torch) are governed; every other form
        // type passes the seat untouched -- logged once per (actor, formType) at
        // debug level, never per event. A plain member read on the engine's
        // thread, no lookup (INVARIANTS #12/#13). channels/EquipAuthority.cpp
        // mirrors this set on the equip side. ----
        bool IsGovernedType(RE::FormType t) {
            return t == RE::FormType::Armor || t == RE::FormType::Weapon ||
                   t == RE::FormType::Ammo  || t == RE::FormType::Light;
        }
        constexpr std::size_t kUngovernedSeenMax = 1024;
        std::unordered_set<std::uint64_t> g_ungovernedSeen;   // (actor << 8 | formType), under g_logMx
        // True the FIRST time this (actor, formType) pair is seen; bounded.
        bool FirstUngovernedSight(RE::FormID actor, RE::FormType t) {
            const std::uint64_t key = (static_cast<std::uint64_t>(actor) << 8) | (static_cast<std::uint64_t>(t) & 0xFF);
            std::scoped_lock lock(g_logMx);
            if (g_ungovernedSeen.size() >= kUngovernedSeenMax) g_ungovernedSeen.clear();   // never unbounded; a re-log beats growth
            return g_ungovernedSeen.insert(key).second;
        }

        // ---- THE THUNK. Same signature as the worker; the two patched E8s land
        // here through one shared trampoline stub. ----
        void SinkThunk(RE::ActorEquipManager* mgr, RE::Actor* actor, RE::TESBoundObject* obj, EquipData* data) {
            // Captured FIRST, before anything else (core/EquipGate.cpp precedent):
            // the genuine site (the E8 is 5 bytes before where it returns to) and
            // this frame's own return slot, from which the caller's slot is a fixed
            // per-site distance.
            const std::uintptr_t site      = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - 5;
            const auto           myRetSlot = reinterpret_cast<std::uint8_t*>(_AddressOfReturnAddress());

            // #17a condition 5: the player and any unclaimed actor pass through
            // untouched and unlogged. Cheapest checks first.
            if (!actor || !obj || !data || actor->IsPlayerRef()) { g_worker(mgr, actor, obj, data); return; }
            const RE::FormID actorId = actor->GetFormID();
            EquipSetView set;
            if (!ControlMap::Get().TryGetEquipSet(actorId, set)) { g_worker(mgr, actor, obj, data); return; }

            // A claimed actor. A non-governed form type (a potion, food, a scroll,
            // an ingredient, a book) passes: the worn set is ARMO/WEAP/AMMO/LIGH
            // only. Logged once per (actor, formType) at debug, not per event.
            const RE::FormType formType = obj->GetFormType();
            if (!IsGovernedType(formType)) {
                try {
                    if (FirstUngovernedSight(actorId, formType))
                        spdlog::debug("[apmf][equip-obs] actor={} formType=0x{} verdict=allow (not a governed type: "
                                      "ARMO/WEAP/AMMO/LIGH only; first sight, logged once per actor and type) item={} name='{}'",
                                      apmf::log::Hex(actorId), apmf::log::Hex(static_cast<std::uint32_t>(formType), 2),
                                      apmf::log::Hex(obj->GetFormID()), obj->GetName() ? obj->GetName() : "");
                } catch (...) {
                }
                g_worker(mgr, actor, obj, data);
                return;
            }

            // Everything from here is logged (deduped/capped).
            const RE::FormID itemId = obj->GetFormID();
            const int        tls    = t_apmfDepth;

            std::uint32_t depth = 0;
            const InstalledSite* siteSpec = nullptr;
            for (const auto& s : g_sites) if (s.addr == site) { siteSpec = &s; depth = s.depth; break; }
            Caller caller;
            if (depth) {
                std::uintptr_t ret = 0;
                std::memcpy(&ret, myRetSlot + depth, sizeof(ret));
                caller = Classify(ret);
            }
            // (depth == 0 cannot happen -- only our two E8s reach this thunk -- but a
            // classification that cannot be made is logged as Unknown, never guessed.)

            bool inSet = false;
            for (std::uint32_t i = 0; i < set.count; ++i) if (set.forms[i] == itemId) { inSet = true; break; }

            // ABI v9: what this item competes for, with the slot the engine resolved
            // for it (data->slot: the caller's a_slot or the default slot 38911/37955
            // picked). ONE map, shared with the enforce pass (Categorize). The
            // claim's owned/denied masks came out of the SAME snapshot read as the
            // set, so the two can never be from different generations.
            const std::uint32_t competes = Categorize(obj, data->slot);
            const bool          blacked  = (competes & set.denied) != 0;
            const bool          ownedCat = (competes & set.owned)  != 0;

            const bool observe    = g_observeOnly.load(std::memory_order_relaxed) ||
                                    (set.flags & APMF_API::kEquipAuth_ObserveOnly) != 0;
            const bool denyScript = g_denyScript.load(std::memory_order_relaxed) ||
                                    (set.flags & APMF_API::kEquipAuth_DenyScript) != 0;
            const bool scriptPath = (caller.kind == PathKind::kScript || caller.kind == PathKind::kConsole);
            // ABI v8: the player's own equips on the actor (the trade/gift menu)
            // pass by default -- player agency; a client wanting the strict form
            // sets kEquipAuth_DenyPlayerMenu on its claim. No INI twin.
            const bool denyPlayerMenu = (set.flags & APMF_API::kEquipAuth_DenyPlayerMenu) != 0;
            const bool playerMenuPath = (caller.kind == PathKind::kPlayerMenu);

            // The verdict (ABI v9). Order matters and is the whole policy:
            //   0. a script/console equip without DenyScript            -> allow (exempt; above both masks)
            //   1. a PlayerMenu equip without DenyPlayerMenu            -> allow (player agency; above both masks)
            //   2. competes for a DENIED category                       -> deny, even if in-set
            //   3. competes for an OWNED category, no declaration yet   -> allow (declare->enforce)
            //   4. competes for an OWNED category, a declared item      -> allow
            //   5. competes for an OWNED category, off-set              -> deny
            //   6. competes for nothing the claim holds                 -> allow (owned=0)
            // Observe-only (INI or the claim's bit) turns each deny into `would-deny`.
            // Default scope {All, 0} makes 2 unreachable and 6 unreachable, so a
            // v7/v8 client sees exactly the v8 verdict table.
            // `tls` is a LOG FIELD ONLY, never a bypass (Fable tier-3 on d1aa66b,
            // SEV-4 #4): APMF's own Enforce equips are in-set by construction, so
            // they pass on the in-set test; the worker family's 38913 re-entry
            // beneath one of them equips the SAME object it was handed, which is
            // also in-set. Letting tls>0 bypass the set would have let a second
            // copy of a DISPLACED off-set item ride back in on that re-entry, and
            // made probe criterion 2 ("zero would-deny with tls>0") vacuous.
            const char* verdict = "allow";
            bool        callWorker = true;
            bool        refuse     = false;
            if (scriptPath && !denyScript) {
                verdict = "allow";
            } else if (playerMenuPath && !denyPlayerMenu) {
                verdict = "allow (player agency)";
            } else if (blacked) {
                refuse  = true;
            } else if (ownedCat) {
                refuse  = !(set.count == 0 || inSet);
            } else {
                verdict = "allow";   // owned=0: nothing the claim holds is at stake
            }
            if (refuse) {
                if (observe) verdict = "would-deny";
                else { verdict = "deny"; callWorker = false; }
            }

            // Log line (Docs/INTEGRATION.md's probe criteria parse these fields).
            // Wrapped: nothing in the logging path may unwind into the engine's
            // equip frame (a bad_alloc here must cost a log line, never a CTD).
            try {
                const auto     nowMs = apmf::clock::MonotonicMs();
                const std::uint64_t pathKey = caller.external
                    ? (0x8000000000000000ull ^ std::hash<std::string_view>{}(caller.ext))
                    : caller.id;
                const std::uint64_t key = ((static_cast<std::uint64_t>(actorId) << 32) | itemId) ^ (pathKey * 0x9E3779B97F4A7C15ull);
                std::uint32_t drops = 0;
                const bool admit = AdmitLog(key, nowMs, drops);
                if (drops)
                    spdlog::warn("[apmf][equip-obs] dropped {} line(s) in the last minute (cap {}/s).", drops, kCapPerSec);
                if (admit) {
                    std::string path;
                    if (caller.external)     path = std::string("External(") + caller.ext + ")";
                    else if (!caller.known)  path = "Unknown(" + std::to_string(caller.id) + ")";
                    else                     path = caller.name;
                    const char* itemName = obj->GetName();
                    // ABI v9 fields, after tls=: the categories the item competes
                    // for, whether any is owned / denied by the claim, and the
                    // engine's resolved slot: `none` for a null slot, R/L for the
                    // two hand EQUPs, E for EitherHand (Skyrim.esm 0x13F44,
                    // Docs/ADDRESS-TABLE-2026-09-15.md -- a LOG LABEL only; the
                    // verdict treats it as "not a hand", both hands competed),
                    // else the slot's FormID (a body slot or a foreign one).
                    char catBuf[48];
                    std::string eslot;
                    if (!data->slot) eslot = "none";
                    else {
                        constexpr RE::FormID kEitherHandEquipSlotLogOnly = 0x00013F44;
                        const RE::FormID slotId = data->slot->GetFormID();
                        if (slotId == kRightHandEquipSlot)              eslot = "R";
                        else if (slotId == kLeftHandEquipSlot)          eslot = "L";
                        else if (slotId == kEitherHandEquipSlotLogOnly) eslot = "E";
                        else                                            eslot = apmf::log::Hex(slotId);
                    }
                    spdlog::info("[apmf][equip-obs] actor={} item={} op=equip path={} site={}+0x{} ret={} q={} f={} s={} a={} tls={} cat={} owned={} black={} eslot={} verdict={} name='{}'",
                                 apmf::log::Hex(actorId), apmf::log::Hex(itemId), path,
                                 siteSpec ? siteSpec->id : 0, apmf::log::Hex(siteSpec ? siteSpec->off : 0, 0),
                                 caller.external ? std::string("ext") : apmf::log::Hex(caller.rva, 0),
                                 data->queue ? 1 : 0, data->force ? 1 : 0, data->sounds ? 1 : 0, data->applyNow ? 1 : 0,
                                 tls, CategoryNames(competes, catBuf, sizeof(catBuf)), ownedCat ? 1 : 0, blacked ? 1 : 0, eslot,
                                 verdict, itemName ? itemName : "");
                }
            } catch (...) {
            }

            if (callWorker) g_worker(mgr, actor, obj, data);
            // deny: return without calling -- nothing queued, no re-entry, and the
            // caller's own lock/epilogue runs exactly as if the worker had returned.
        }

    }   // namespace

    // ABI v9: THE category map (declared in EquipSink.h with its table). Called by
    // the thunk on the engine's equip thread with the engine's resolved slot, and
    // by channels/EquipAuthority.cpp on the main thread with the declared hand.
    // Member reads only: GetFormType (TESForm), GetSlotMask (BGSBipedObjectForm,
    // pinned CommonLib 3.7.0 BGSBipedObjectForm.h:78), the WEAP animation-type
    // predicates (TESObjectWEAP.h:250-254) and the slot's FormID. No lookup.
    std::uint32_t Categorize(const RE::TESBoundObject* obj, const RE::BGSEquipSlot* slot) {
        using namespace APMF_API;
        if (!obj) return 0;
        switch (obj->GetFormType()) {
        case RE::FormType::Armor: {
            const auto* armo = obj->As<RE::TESObjectARMO>();
            if (!armo) return kEquipCat_Armor;   // a malformed ARMO is still armor, never a hand
            const auto mask   = static_cast<std::uint32_t>(armo->GetSlotMask());
            const auto kShield = static_cast<std::uint32_t>(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
            const bool shield = (mask & kShield) != 0;
            // Both kinds of bits -> both categories (Fable on 3d5cab8, SEV-3 F1): a
            // modded "shield on back" piece carries kShield AND a body/back bit; as
            // Shield|Left only it would land under an Armor-only scope and displace
            // the declared body piece. An ARMO with NO biped bits at all is Armor.
            const bool armor  = (mask & ~kShield) != 0 || mask == 0;
            return (shield ? (kEquipCat_Shield | kEquipCat_Left) : 0u) | (armor ? kEquipCat_Armor : 0u);
        }
        case RE::FormType::Weapon: {
            const auto* weap = obj->As<RE::TESObjectWEAP>();
            if (!weap) return kEquipCat_Right | kEquipCat_Left;   // unknown shape: both hands (conservative)
            if (weap->IsTwoHandedSword() || weap->IsTwoHandedAxe() || weap->IsBow() || weap->IsCrossbow())
                return kEquipCat_Right | kEquipCat_Left;
            // One-handed, including a staff: the slot names the hand. EitherHand,
            // a foreign slot, or no slot at all -> the engine has not picked yet,
            // so the item competes for BOTH hands (conservative on purpose).
            if (slot) {
                const RE::FormID slotId = slot->GetFormID();
                if (slotId == kLeftHandEquipSlot)  return kEquipCat_Left;
                if (slotId == kRightHandEquipSlot) return kEquipCat_Right;
            }
            return kEquipCat_Right | kEquipCat_Left;
        }
        case RE::FormType::Ammo:  return kEquipCat_Ammo;
        case RE::FormType::Light: return kEquipCat_Light | kEquipCat_Left;
        default:                  return 0;   // not a governed type
        }
    }

    const char* CategoryNames(std::uint32_t mask, char* buf, std::size_t size) {
        if (!buf || size == 0) return "";
        buf[0] = '\0';
        if (mask == 0) {
            const char*       none = "none";
            const std::size_t len  = std::min(std::strlen(none), size - 1);
            std::memcpy(buf, none, len);
            buf[len] = '\0';
            return buf;
        }
        static constexpr struct { std::uint32_t bit; const char* name; } kNames[] = {
            { APMF_API::kEquipCat_Armor,  "Armor"  }, { APMF_API::kEquipCat_Shield, "Shield" },
            { APMF_API::kEquipCat_Right,  "Right"  }, { APMF_API::kEquipCat_Left,   "Left"   },
            { APMF_API::kEquipCat_Ammo,   "Ammo"   }, { APMF_API::kEquipCat_Light,  "Light"  },
        };
        std::size_t n = 0;
        for (const auto& e : kNames) {
            if (!(mask & e.bit)) continue;
            const std::size_t len = std::strlen(e.name);
            if (n + (n ? 1 : 0) + len + 1 > size) break;   // never overrun; a truncated list beats a fault
            if (n) buf[n++] = '+';
            std::memcpy(buf + n, e.name, len);
            n += len;
            buf[n] = '\0';
        }
        if (mask & ~APMF_API::kEquipCat_All) {
            const char* extra = "+?";
            const std::size_t len = std::strlen(extra);
            if (n + len + 1 <= size) { std::memcpy(buf + n, extra, len); n += len; buf[n] = '\0'; }
        }
        return buf;
    }

    ApmfEquipScope::ApmfEquipScope()  { ++t_apmfDepth; }
    ApmfEquipScope::~ApmfEquipScope() { --t_apmfDepth; }
    int  ApmfDepth()   { return t_apmfDepth; }
    void ReinspectEntries(const char* why) {
        if (!g_installed.load(std::memory_order_acquire)) return;
        InspectEntryDetours(why ? why : "?");
    }
    bool Installed()   { return g_installed.load(std::memory_order_acquire); }
    const char* NotInstalledReason() { return g_notInstalledReason.load(std::memory_order_acquire); }
    bool Enforcing()   { return Installed() && !g_observeOnly.load(std::memory_order_relaxed); }
    bool ObserveOnly() { return g_observeOnly.load(std::memory_order_relaxed); }
    bool DenyScript()  { return g_denyScript.load(std::memory_order_relaxed); }

    void Install() {
        if (REL::Module::IsVR()) {
            g_notInstalledReason.store("VR runtime", std::memory_order_release);
            spdlog::warn("[apmf][equip-sink] VR runtime -- sites are SE/AE-only verified; seat NOT installed.");
            return;
        }
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;

        const bool enabled = GetPrivateProfileIntA("EquipAuthority", "bEquipAuthority", 1, kIni) != 0;
        g_observeOnly.store(GetPrivateProfileIntA("EquipAuthority", "bEquipObserveOnly", 1, kIni) != 0,
                            std::memory_order_relaxed);
        g_denyScript.store(GetPrivateProfileIntA("EquipAuthority", "bEquipDenyScript", 0, kIni) != 0,
                           std::memory_order_relaxed);
        if (!enabled) {
            g_notInstalledReason.store("[EquipAuthority] bEquipAuthority=0", std::memory_order_release);
            spdlog::warn("[apmf][equip-sink] [EquipAuthority] bEquipAuthority=0 -- seat NOT installed; "
                         "kIntent_EquipAuthority claims are REFUSED (kInvalidHandle) so a client keeps its own equips.");
            return;
        }

        // EXACT-VERSION gate, the same two-version predicate core/CastClassify.cpp,
        // core/AiCastSeats.cpp Group C and plugin.cpp's `[runtime]` line evaluate
        // (deliberately NOT REL::Module::IsAE()/IsSE(): in 3.7.0 IsSE() is the
        // `default:` arm, so an unknown build would classify as SE and be byte-
        // verified against SE ids it does not have). The sites, offsets and frame
        // depths are disassembly-verified on 1.6.1170 and 1.5.97 ONLY (rule 11);
        // any other build is GATED here, by name -- not a site-verify failure.
        const auto ver      = REL::Module::get().version();
        const bool onAE1170 = ver == REL::Version{ 1, 6, 1170, 0 };
        const bool onSE597  = ver == REL::Version{ 1, 5, 97, 0 };
        if (!onAE1170 && !onSE597) {
            g_notInstalledReason.store("runtime gated (not 1.6.1170 / 1.5.97)", std::memory_order_release);
            spdlog::warn("[apmf][equip-sink] runtime {} gated -- the two worker call sites and their frame "
                         "depths are disassembly-verified on 1.6.1170 and 1.5.97 only; seat NOT installed "
                         "(kIntent_EquipAuthority claims are REFUSED on this runtime: seat not installed).",
                         ver.string("."));
            return;
        }
        const bool ae = onAE1170;
        const SiteSpec* sites = ae ? kSitesAE : kSitesSE;
        g_paths     = ae ? kPathsAE : kPathsSE;
        g_pathCount = ae ? std::size(kPathsAE) : std::size(kPathsSE);

        const auto& mod  = REL::Module::get();
        g_base           = mod.base();
        const auto text  = mod.segment(REL::Segment::textx);
        g_textBegin      = text.address();
        g_textEnd        = text.address() + text.size();

        const std::uintptr_t worker = REL::ID(ae ? kWorkerAE : kWorkerSE).address();
        g_worker = reinterpret_cast<Worker_t>(worker);

        // mit-3.7 F1: the worker and both call sites must be verified addresses
        // (VerifiedAddresses.h rows EquipSink.Worker / EquipSink.Site.*, whose site rows
        // also check the E8 byte). Any refusal refuses the whole seat, like the byte
        // check below, which stays as it was.
        {
            bool verified = allowance::SeatVerified(worker, "EquipSink.Worker");
            for (int i = 0; i < 2; ++i) {
                const std::uintptr_t addr = REL::ID(sites[i].id).address() + sites[i].off;
                verified = allowance::SeatVerified(addr, fmt::format("EquipSink.Site.{}", sites[i].name)) && verified;
            }
            if (!verified) {
                g_notInstalledReason.store("self-check refused the worker or a call site", std::memory_order_release);
                spdlog::error("[apmf][equip-sink] seat NOT installed (self-check refused an address).");
                return;
            }
        }

        // #17a condition 2: byte-verify EVERY site before writing ANY. Any mismatch
        // refuses the whole seat -- a SCAR-class collision becomes a refusal.
        bool ok = true;
        for (int i = 0; i < 2; ++i) {
            const auto& s = sites[i];
            const std::uintptr_t addr = REL::ID(s.id).address() + s.off;
            std::uint8_t b[5]{};
            std::memcpy(b, reinterpret_cast<const void*>(addr), sizeof(b));
            std::int32_t rel = 0;
            std::memcpy(&rel, b + 1, sizeof(rel));
            const std::uintptr_t target = addr + 5 + static_cast<std::intptr_t>(rel);
            if (b[0] != 0xE8 || target != worker) {
                spdlog::error("[apmf][equip-sink] site-verify FAILED {}+0x{} ({}) got {} {} {} {} {} (target 0x{}, expected E8->0x{}) -- seat NOT installed",
                              s.id, apmf::log::Hex(s.off, 0), s.name,
                              apmf::log::Hex(b[0], 2), apmf::log::Hex(b[1], 2), apmf::log::Hex(b[2], 2),
                              apmf::log::Hex(b[3], 2), apmf::log::Hex(b[4], 2),
                              apmf::log::Hex(target - g_base, 0), apmf::log::Hex(worker - g_base, 0));
                ok = false;
                continue;
            }
            g_sites[i] = InstalledSite{ addr, s.depth, s.id, s.off, s.name };
            spdlog::info("[apmf][equip-sink] site {}+0x{} ({}) verified E8->0x{} (caller-ret depth 0x{})",
                         s.id, apmf::log::Hex(s.off, 0), s.name, apmf::log::Hex(worker - g_base, 0),
                         apmf::log::Hex(s.depth, 0));
        }
        if (!ok) {
            g_sites[0] = g_sites[1] = InstalledSite{};
            g_notInstalledReason.store("site-verify refused (another patcher at a worker call site)", std::memory_order_release);
            return;
        }

        // ENTRY-DETOUR INSPECTION, first pass (see InspectEntryDetours above; it is
        // re-run at kPostLoadGame / kNewGame from plugin.cpp because a plugin
        // later in load order may detour the entry after our kDataLoaded).
        g_entrySites[0] = REL::ID(sites[0].id).address(); g_entrySiteIds[0] = sites[0].id; g_entrySiteNames[0] = sites[0].name;
        g_entrySites[1] = REL::ID(sites[1].id).address(); g_entrySiteIds[1] = sites[1].id; g_entrySiteNames[1] = sites[1].name;
        InspectEntryDetours("kDataLoaded");

        // The caller classifier's table: a sorted copy of the Address Library
        // (one-time, ~0.4M AE / ~0.8M SE entries). Built BEFORE the patch so the
        // thunk never runs against a half-built table.
        {
            const auto t0 = apmf::clock::MonotonicMs();
            g_offset2id = std::make_unique<REL::IDDatabase::Offset2ID>();
            spdlog::info("[apmf][equip-sink] caller table: {} address-library entries sorted in {} ms.",
                         g_offset2id->size(), apmf::clock::MonotonicMs() - t0);
        }

        // Both sites share one 14-byte stub (write_5branch keys the stub by
        // destination). 64 leaves headroom; nothing else in APMF uses the trampoline.
        SKSE::AllocTrampoline(64);
        auto& tr = SKSE::GetTrampoline();
        for (const auto& s : g_sites) tr.write_call<5>(s.addr, &SinkThunk);

        g_notInstalledReason.store("", std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        const bool entriesClean = (g_entryVerdict[0] == "none" && g_entryVerdict[1] == "none");
        spdlog::info("[apmf][equip-sink] INSTALLED on {} ({} sites -> worker {}); observe-only={} deny-script={} "
                     "entries={}. Deny = the worker is not called; the player and unclaimed actors pass untouched "
                     "(INVARIANTS #17a).",
                     ae ? "AE" : "SE", 2, ae ? kWorkerAE : kWorkerSE,
                     g_observeOnly.load(std::memory_order_relaxed) ? 1 : 0,
                     g_denyScript.load(std::memory_order_relaxed) ? 1 : 0,
                     entriesClean ? "clean" : "DETOURED");
    }

}
