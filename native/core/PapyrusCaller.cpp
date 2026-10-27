#include "PCH.h"
#include "core/PapyrusCaller.h"

#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Win32, declared by hand like core/EquipSink.cpp (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);
extern "C" __declspec(dllimport) int __stdcall GetModuleHandleExA(unsigned long a_flags, const char* a_name, void** a_module);
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void* a_module, char* a_name, unsigned long a_size);

// See core/PapyrusCaller.h for the design. Values below were read on the disassembly
// (agentlog apmf-papyrus-attribution-2.md, ClickUp 86e3q082a comment "Milestone 1"):
//   IFunction slot 0x0F Call(this, const BSTSmartPointer<Stack>& rdx, ErrorLogger* r8,
//     VirtualMachine* r9, bool) -- the engine's NativeFunctionBase::Call (AE 104651 @0x143DC00,
//     SE 97923 @0x12507F0, 1.7.104 @0x14A9C10) is what every engine native's slot 0x0F holds.
//   Stack+0x60 top frame, StackFrame+0x08 previousFrame, +0x10 owningFunction, +0x18
//     owningObjectType, +0x28 self (fork Stack.h / StackFrame.h at 57be9d67).
//   VirtualMachine+0x90 handlePolicy, +0xC0 typeInfoLock, +0x158 objectTypeMap (all three builds).
//   BSScript::Object+0x20 handle: read raw (Object::GetHandle is an id-based engine call, and the
//     frame holds the object alive; an aligned 8-byte read cannot tear).
//   HandlePolicy: a form handle's low 32 bits are its FormID; an alias handle (types 139-141) is
//     the owning quest's FormID | aliasID << 32; a handle with any of its top 16 bits set is a
//     special one (an ActiveMagicEffect, type 142), resolved through the policy.

namespace apmf::papyruscaller {

    namespace {
        using VM        = RE::BSScript::Internal::VirtualMachine;
        using IFunction = RE::BSScript::IFunction;
        using Stack     = RE::BSScript::Stack;
        using Frame     = RE::BSScript::StackFrame;
        using CallFn    = IFunction::CallResult (*)(IFunction*, const RE::BSTSmartPointer<Stack>&,
                                                    RE::BSScript::ErrorLogger*, VM*, bool);

        constexpr const char* kIni      = "Data/SKSE/Plugins/APMF.ini";
        constexpr const char* kRow      = "PapyrusCaller.NativeFunctionBase.Call";
        constexpr std::size_t kCallSlot = 0x0F;

        // ---- the per-thread record (the whole state the thunk writes) ----
        struct Running {
            IFunction* fn    = nullptr;
            Stack*     stack = nullptr;
            VM*        vm    = nullptr;
        };
        thread_local Running t_cur{};

        // ---- vtable -> the slot-0x0F function it held before us. Open addressing, written on the
        // MAIN thread only (under g_installMx), never erased: `orig` is stored before `vt` is
        // published (release), and the vtable slot is written only after both. Readers on any
        // thread (the thunk) acquire `vt`. ----
        constexpr std::size_t kCap = 4096;   // power of two
        struct Slot {
            std::atomic<std::uintptr_t> vt{ 0 };
            std::atomic<std::uintptr_t> orig{ 0 };
        };
        Slot g_slots[kCap];
        std::size_t g_used = 0;   // under g_installMx

        std::size_t HashOf(std::uintptr_t a_vt) { return static_cast<std::size_t>((a_vt >> 3) * 0x9E3779B97F4A7C15ull) & (kCap - 1); }

        std::uintptr_t Lookup(std::uintptr_t a_vt) {
            for (std::size_t i = HashOf(a_vt), n = 0; n < kCap; i = (i + 1) & (kCap - 1), ++n) {
                const auto v = g_slots[i].vt.load(std::memory_order_acquire);
                if (v == a_vt) return g_slots[i].orig.load(std::memory_order_acquire);
                if (v == 0) return 0;
            }
            return 0;
        }
        Slot* Find(std::uintptr_t a_vt) {   // MAIN thread
            for (std::size_t i = HashOf(a_vt), n = 0; n < kCap; i = (i + 1) & (kCap - 1), ++n) {
                const auto v = g_slots[i].vt.load(std::memory_order_relaxed);
                if (v == a_vt) return &g_slots[i];
                if (v == 0) return nullptr;
            }
            return nullptr;
        }
        Slot* Insert(std::uintptr_t a_vt, std::uintptr_t a_orig) {   // MAIN thread, under g_installMx
            if (g_used >= kCap / 2) return nullptr;   // keep the probe chains short; refused loudly by the caller
            for (std::size_t i = HashOf(a_vt), n = 0; n < kCap; i = (i + 1) & (kCap - 1), ++n) {
                if (g_slots[i].vt.load(std::memory_order_relaxed) == 0) {
                    g_slots[i].orig.store(a_orig, std::memory_order_release);
                    g_slots[i].vt.store(a_vt, std::memory_order_release);
                    ++g_used;
                    return &g_slots[i];
                }
            }
            return nullptr;
        }

        std::atomic<VM*>            g_vm{ nullptr };
        std::atomic<bool>           g_installed{ false };
        std::atomic<bool>           g_refused{ false };   // a gate refused: never retried
        std::atomic<bool>           g_gateRan{ false };
        std::uintptr_t              g_engineCall = 0;     // the verified engine Call (written before g_installed)
        std::mutex                  g_installMx;
        std::atomic<bool>           g_firstCallLogged{ false };
        std::atomic<std::uint64_t>  g_calls{ 0 };
        std::atomic<std::uint64_t>  g_misses{ 0 };
        std::mutex                  g_missMx;
        std::unordered_set<std::uintptr_t> g_missLogged;   // under g_missMx

        // The base name of the module whose image holds `a_addr` ("" when none). No dereference of
        // `a_addr` (it may be a data address: a vtable).
        void ModuleBaseName(std::uintptr_t a_addr, char* a_out, std::size_t a_size) {
            if (!a_out || a_size == 0) return;
            a_out[0] = '\0';
            void* mod = nullptr;
            // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS (0x4) | UNCHANGED_REFCOUNT (0x2)
            if (!GetModuleHandleExA(0x6, reinterpret_cast<const char*>(a_addr), &mod) || !mod) return;
            char path[260]{};
            const auto n = GetModuleFileNameA(mod, path, sizeof(path));
            if (n == 0 || n >= sizeof(path)) return;
            const char* base = path;
            for (const char* p = path; *p; ++p)
                if (*p == '\\' || *p == '/') base = p + 1;
            std::strncpy(a_out, base, a_size - 1);
            a_out[a_size - 1] = '\0';
        }

        std::string FnName(const IFunction* a_fn) {
            if (!a_fn) return "?";
            const char* t = a_fn->GetObjectTypeName().c_str();
            const char* n = a_fn->GetName().c_str();
            return std::string(t ? t : "?") + "." + (n ? n : "?");
        }

        // ---- THE THUNK (slot 0x0F of every seated native vtable). Records, calls, restores. ----
        IFunction::CallResult CallThunk(IFunction* a_this, const RE::BSTSmartPointer<Stack>& a_stack,
                                        RE::BSScript::ErrorLogger* a_logger, VM* a_vm, bool a_arg4) {
            const auto vt   = *reinterpret_cast<const std::uintptr_t*>(a_this);
            std::uintptr_t orig = Lookup(vt);
            if (!orig) {
                // Not reachable by construction (only a vtable in the table holds this thunk, and a
                // hook chained above us calls us with the same object). If it ever happens it is a
                // DEFECT, logged per vtable at error and counted; the engine's own Call runs the
                // native (every native's Call is it or forwards to it), so the native still works.
                orig = g_engineCall;
                g_misses.fetch_add(1, std::memory_order_relaxed);
                bool first = false;
                {
                    std::scoped_lock lock(g_missMx);
                    first = g_missLogged.insert(vt).second;
                }
                if (first)
                    spdlog::error("[apmf][papyrus-attr] DEFECT: the Call thunk was reached for vtable 0x{} that is not "
                                  "in the seat table (native {}); the engine's Call ran it. Its caller is not recorded.",
                                  apmf::log::Hex(vt, 0), FnName(a_this));
                return reinterpret_cast<CallFn>(orig)(a_this, a_stack, a_logger, a_vm, a_arg4);
            }
            const Running saved = t_cur;
            t_cur = Running{ a_this, a_stack.get(), a_vm };
            g_calls.fetch_add(1, std::memory_order_relaxed);
            if (!g_firstCallLogged.load(std::memory_order_relaxed) && !g_firstCallLogged.exchange(true)) {
                spdlog::info("[apmf][papyrus-attr] seat live: first Papyrus native call seen ({}, thread {}).",
                             FnName(a_this), REX::W32::GetCurrentThreadId());
            }
            const auto r = reinterpret_cast<CallFn>(orig)(a_this, a_stack, a_logger, a_vm, a_arg4);
            t_cur = saved;
            return r;
        }

        // ---- who is calling: read on the native's own thread, from its own stack ----
        struct Info {
            std::string native;            // "Actor.EquipItem"
            bool        topIsNative = false;   // stack->top->owningFunction == the native (layout self-check)
            std::string caller;            // "<Script>.<Function>" of the frame that called the native
            std::string bound;             // the script type of the first frame bound to an object
            int         hops = 0;          // frames walked from the caller to that frame (0 = the caller itself)
            const char* kind = "none";     // form | alias | effect | unbound | static | none
            RE::FormID  owner = 0;         // the form owning that object (an alias's quest, an effect's MGEF)
            std::string definer;           // GetFile(0) of the owner (or of a created reference's base)
            std::string winner;            // GetFile(-1)
        };

        std::string ScriptOf(const Frame* a_f) {
            if (!a_f) return "?";
            const auto* t  = a_f->owningObjectType.get();
            const auto* fn = a_f->owningFunction.get();
            const char* tn = t ? t->GetName() : nullptr;
            const char* fnn = fn ? fn->GetName().c_str() : nullptr;
            return std::string(tn ? tn : "?") + "." + (fnn ? fnn : "?");
        }

        void FilesOf(const RE::TESForm* a_form, Info& a_out) {
            if (!a_form) return;
            const RE::TESFile* d = a_form->GetFile(0);
            const RE::TESFile* w = a_form->GetFile(-1);
            const char*        note = "";
            if (!d) {
                // A created reference (0xFF) has no file: its scripts come from its base object.
                if (const auto* ref = a_form->AsReference(); ref) {
                    if (const auto* base = ref->GetBaseObject(); base) {
                        d    = base->GetFile(0);
                        w    = base->GetFile(-1);
                        note = " (base)";
                    }
                }
            }
            a_out.definer = d ? std::string(d->GetFilename()) + note : std::string("?");
            a_out.winner  = w ? std::string(w->GetFilename()) + note : std::string("?");
        }

        // Fills `a_out` when a Papyrus native is running on this thread. Never throws.
        bool Describe(Info& a_out) noexcept {
            const Running cur = t_cur;
            if (!cur.fn) return false;
            try {
                a_out.native = FnName(cur.fn);
                const Frame* top = cur.stack ? cur.stack->top : nullptr;
                a_out.topIsNative = top && top->owningFunction.get() == cur.fn;
                const Frame* f = top ? top->previousFrame : nullptr;
                if (!f) {
                    a_out.caller = "<none: the native is its stack's first frame>";
                    return true;
                }
                a_out.caller = ScriptOf(f);
                // The first frame bound to an object (a script's own global function is unbound). The
                // object is taken through Variable's public GetObject() (a counted reference; the frame
                // holds its own, so this one only keeps it alive for the read below).
                const Frame*                                b = f;
                int                                         hops = 0;
                RE::BSTSmartPointer<RE::BSScript::Object>   obj;
                while (b && hops < 8) {
                    if (b->self.IsObject()) {
                        obj = b->self.GetObject();
                        if (obj) break;
                    }
                    b = b->previousFrame;
                    ++hops;
                }
                if (!b || hops >= 8 || !obj) {
                    a_out.kind = "static";
                    return true;
                }
                a_out.hops  = hops;
                a_out.bound = ScriptOf(b);
                const RE::VMHandle h = obj->handle;
                auto* policy = cur.vm ? cur.vm->handlePolicy : nullptr;
                if (!policy || h == policy->EmptyHandle()) {
                    a_out.kind = "unbound";
                    return true;
                }
                if ((h >> 48) != 0) {
                    if (policy->HandleIsType(RE::ActiveEffect::VMTYPEID, h)) {
                        auto* ae = static_cast<RE::ActiveEffect*>(policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, h));
                        const RE::EffectSetting* mgef = ae ? ae->GetBaseObject() : nullptr;
                        a_out.kind  = "effect";
                        a_out.owner = mgef ? mgef->GetFormID() : 0;
                        FilesOf(mgef, a_out);
                    } else {
                        a_out.kind = "special";
                    }
                    return true;
                }
                const auto formId = static_cast<RE::FormID>(h & 0xFFFFFFFFull);
                const bool alias  = policy->HandleIsType(RE::BGSRefAlias::VMTYPEID, h) ||
                                    policy->HandleIsType(RE::BGSLocAlias::VMTYPEID, h) ||
                                    policy->HandleIsType(RE::BGSBaseAlias::VMTYPEID, h);
                a_out.kind  = alias ? "alias" : "form";
                a_out.owner = formId;
                FilesOf(RE::TESForm::LookupByID(formId), a_out);
                return true;
            } catch (...) {
                a_out.kind = "error";
                return true;
            }
        }

        // ---- probe log discipline: one line per (actor, item) per 2 s, at most 20 lines/s ----
        std::mutex                                       g_logMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_lastMs;
        std::uint64_t                                    g_secStart = 0;
        std::uint32_t                                    g_secCount = 0;
        bool Admit(RE::FormID a_actor, RE::FormID a_item) {
            const std::uint64_t now = apmf::clock::MonotonicMs();
            const std::uint64_t key = (static_cast<std::uint64_t>(a_actor) << 32) | a_item;
            std::scoped_lock lock(g_logMx);
            if (auto it = g_lastMs.find(key); it != g_lastMs.end() && now - it->second < 2000) return false;
            if (now - g_secStart >= 1000) { g_secStart = now; g_secCount = 0; }
            if (g_secCount >= 20) return false;
            ++g_secCount;
            if (g_lastMs.size() >= 2048) g_lastMs.clear();   // bounded; a re-log beats growth
            g_lastMs[key] = now;
            return true;
        }

        bool OnPapyrusRegister(VM* a_vm) {
            g_vm.store(a_vm, std::memory_order_release);
            return true;
        }

        // The gates, once. MAIN thread. False = refused (logged).
        bool Gate() {
            if (g_gateRan.exchange(true)) return !g_refused.load();
            const auto refuse = [](const char* why) {
                g_refused.store(true);
                spdlog::warn("[apmf][papyrus-attr] caller seat NOT installed: {}. Papyrus equips keep ranking as "
                             "today (DirectCalls / the external module).", why);
                return false;
            };
            if (REL::Module::IsVR()) return refuse("VR runtime");
            if (!apmf::allowance::RuntimeSupported()) return refuse("runtime is not exactly 1.6.1170, 1.5.97 or 1.7.104");
            if (GetPrivateProfileIntA("Probe", "bPapyrusCaller", 0, kIni) == 0) {
                g_refused.store(true);
                spdlog::info("[apmf][papyrus-attr] [Probe] bPapyrusCaller=0 (the default): caller seat off (no vtable written).");
                return false;
            }
            const std::uintptr_t call = REL::Relocation<std::uintptr_t>{ RELOCATION_ID(97923, 104651) }.address();
            if (!apmf::allowance::SeatVerified(call, kRow)) return refuse("self-check row PapyrusCaller.NativeFunctionBase.Call failed");
            g_engineCall = call;
            return true;
        }
    }

    void Init() {
        const auto* papyrus = SKSE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(OnPapyrusRegister))
            spdlog::error("[apmf][papyrus-attr] could not register with SKSE's Papyrus interface: the VM pointer will "
                          "not be known and the caller seat cannot install.");
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    void Install(const char* a_why) {
        const char* why = a_why ? a_why : "?";
        if (!Gate()) return;
        VM* vm = g_vm.load(std::memory_order_acquire);
        if (!vm) {
            spdlog::error("[apmf][papyrus-attr] {}: the Papyrus VM was never handed to APMF (SKSE Papyrus "
                          "registration did not run): caller seat NOT installed yet.", why);
            return;
        }
        std::scoped_lock install(g_installMx);

        // 1. Collect every native function's vtable from the VM's type table, under its own lock
        //    (VM threads may load a type meanwhile). Nothing is written here.
        std::unordered_set<std::uintptr_t> vts;
        std::size_t types = 0, natives = 0, unlinkedTypes = 0;
        const auto take = [&](IFunction* a_fn) {
            if (!a_fn || !a_fn->GetIsNative()) return;
            ++natives;
            vts.insert(*reinterpret_cast<const std::uintptr_t*>(a_fn));
        };
        try {
            RE::BSSpinLockGuard lock(vm->typeInfoLock);
            for (auto& entry : vm->objectTypeMap) {
                auto* ti = entry.second.get();
                if (!ti) continue;
                ++types;
                if (!ti->IsLinked()) {
                    ++unlinkedTypes;
                    for (auto* u = ti->GetUnlinkedFunctionIter(); u; u = u->next) take(u->func.get());
                    continue;
                }
                auto* g = ti->GetGlobalFuncIter();
                for (std::uint32_t i = 0; g && i < ti->GetNumGlobalFuncs(); ++i) take(g[i].func.get());
                auto* m = ti->GetMemberFuncIter();
                for (std::uint32_t i = 0; m && i < ti->GetNumMemberFuncs(); ++i) take(m[i].func.get());
            }
        } catch (...) {
            spdlog::error("[apmf][papyrus-attr] {}: reading the VM type table threw; nothing seated this pass.", why);
            return;
        }

        // 2. Seat each vtable not seated yet: store its slot-0x0F function, then write the thunk.
        const auto thunk = reinterpret_cast<std::uintptr_t>(&CallThunk);
        std::size_t added = 0, already = 0, engineForeign = 0, full = 0;
        std::map<std::string, std::size_t> perModule;   // vtable module -> vtables seated this pass
        char exeName[64]{};
        ModuleBaseName(g_engineCall, exeName, sizeof(exeName));
        for (const auto vt : vts) {
            if (Find(vt)) { ++already; continue; }
            const auto entry = vt + kCallSlot * sizeof(void*);
            const auto cur   = *reinterpret_cast<const std::uintptr_t*>(entry);
            if (cur == thunk) { ++already; continue; }
            char vmod[64]{};
            ModuleBaseName(vt, vmod, sizeof(vmod));
            if (std::strcmp(vmod, exeName) == 0 && cur != g_engineCall) {
                ++engineForeign;
                char cmod[64]{};
                ModuleBaseName(cur, cmod, sizeof(cmod));
                spdlog::info("[apmf][papyrus-attr] engine native vtable 0x{} slot 0x0F already holds 0x{} ({}), not the "
                             "engine's Call: another hook is inside ours (chained, its native still runs through it).",
                             apmf::log::Hex(vt - REL::Module::get().base(), 0), apmf::log::Hex(cur, 0),
                             cmod[0] ? cmod : "no module");
            }
            Slot* s = Insert(vt, cur);
            if (!s) { ++full; continue; }
            REL::Relocation<std::uintptr_t> rel{ vt };
            const std::uintptr_t prev = rel.write_vfunc(kCallSlot, thunk);
            if (prev != cur) s->orig.store(prev, std::memory_order_release);   // moved between the read and the write
            ++added;
            ++perModule[vmod[0] ? vmod : "?"];
        }
        if (added) g_installed.store(true, std::memory_order_release);

        std::string mods;
        for (const auto& [m, n] : perModule) mods += fmt::format("{}{}={}", mods.empty() ? "" : ", ", m, n);
        spdlog::info("[apmf][papyrus-attr] caller seat {} ({}): {} script types ({} unlinked), {} native functions, {} "
                     "distinct vtables; {} seated now, {} already seated; {} engine vtables already hooked inside; "
                     "native calls seen so far {}, table misses {}. Seated per module: {}",
                     added ? "INSTALLED" : "unchanged", why, types, unlinkedTypes, natives, vts.size(), added, already,
                     engineForeign, g_calls.load(std::memory_order_relaxed), g_misses.load(std::memory_order_relaxed),
                     mods.empty() ? "none" : mods);
        if (full)
            spdlog::error("[apmf][papyrus-attr] {}: {} native vtables NOT seated, the seat table is full ({} of {}). "
                          "Calls through them are not recorded (a DEFECT: raise kCap).", why, full, g_used, kCap);
    }

    void ProbeEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, bool a_scriptPath,
                    const char* a_rankedAs) noexcept {
        try {
            if (!g_installed.load(std::memory_order_acquire)) return;
            Info inf;
            const bool running = Describe(inf);
            if (!running && !a_scriptPath) return;   // not Papyrus by either reading: nothing to say
            if (!Admit(a_actor, a_item)) return;
            const char* path   = a_path ? a_path : "?";
            const char* ranked = a_rankedAs ? a_rankedAs : "?";
            if (!running) {
                spdlog::warn("[apmf][papyrus-attr] PROBE equip actor=0x{} item=0x{} path={} ranked-as={} | NO Papyrus "
                             "native is running on this thread ({}): the native that asked is not seated, or its equip "
                             "ran later (a queued apply or a delay functor). Probe only: nothing changed.",
                             apmf::log::Hex(a_actor), apmf::log::Hex(a_item), path, ranked, REX::W32::GetCurrentThreadId());
                return;
            }
            spdlog::info("[apmf][papyrus-attr] PROBE equip actor=0x{} item=0x{} path={} ranked-as={} | native={} "
                         "top-frame={} caller={} bound={} (+{} frames) {} owner=0x{} definer={} winner={} -> would rank "
                         "as '{}'. Probe only: nothing changed.",
                         apmf::log::Hex(a_actor), apmf::log::Hex(a_item), path, ranked, inf.native,
                         inf.topIsNative ? "native" : "MISMATCH", inf.caller, inf.bound.empty() ? "-" : inf.bound, inf.hops,
                         inf.kind, apmf::log::Hex(inf.owner), inf.definer.empty() ? "-" : inf.definer,
                         inf.winner.empty() ? "-" : inf.winner, inf.definer.empty() ? "?" : inf.definer);
        } catch (...) {
        }
    }

}
