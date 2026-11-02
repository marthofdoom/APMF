#include "PCH.h"
#include "core/PapyrusCaller.h"

#include "core/Allowance.h"
#include "core/Clock.h"
#include "core/Log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
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
        std::atomic<bool>           g_rank{ false };      // [Harbinger] bPapyrusAttribution: ScriptSource ranks
        std::atomic<bool>           g_probe{ false };     // [Probe] bPapyrusCaller: ProbeEquip logs
        std::atomic<bool>           g_ownerDefining{ false };   // [Harbinger] iPapyrusOwnerFile=1: name the defining plugin
        std::atomic<std::uint32_t>  g_mainTid{ 0 };       // the main thread (Install runs there)
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
            std::string file;              // STEP 2: the file that owns the script ("" = none: not attributable)
            const char* rule = "none";     // how `file` was found (see the header, "THE OWNING FILE")
        };

        std::string ScriptOf(const Frame* a_f) {
            if (!a_f) return "?";
            const auto* t  = a_f->owningObjectType.get();
            const auto* fn = a_f->owningFunction.get();
            const char* tn = t ? t->GetName() : nullptr;
            const char* fnn = fn ? fn->GetName().c_str() : nullptr;
            return std::string(tn ? tn : "?") + "." + (fnn ? fnn : "?");
        }

        // STEP 2, the owning file of a script bound to `a_form` (header, "THE OWNING FILE"). `a_viaOwner`:
        // the form is the record that carries the script by construction (an alias's quest, an effect's
        // magic effect), so its winning file is the owner.
        void OwnerOf(const RE::TESForm* a_form, bool a_viaOwner, Info& a_out) {
            if (!a_form) return;
            const auto nameOf = [](const RE::TESFile* f) { return f ? std::string(f->GetFilename()) : std::string(); };
            const auto* ref = a_viaOwner ? nullptr : a_form->AsReference();
            if (g_ownerDefining.load(std::memory_order_relaxed)) {
                // [Harbinger] iPapyrusOwnerFile=1 (marth 2026-11-01, "both, toggle between"): the owner form's
                // defining plugin. A created reference has no file: its base's.
                const RE::TESForm* f = a_form;
                if (ref && !ref->GetFile(0)) f = ref->GetBaseObject();
                a_out.file = nameOf(f ? f->GetFile(0) : nullptr);
                a_out.rule = "iPapyrusOwnerFile=1: the owner's defining plugin";
                return;
            }
            if (!ref) {
                a_out.file = nameOf(a_form->GetFile(-1));
                a_out.rule = a_viaOwner ? "the owner's winning file" : "the form's winning file";
                return;
            }
            const RE::TESForm* base     = ref->GetBaseObject();
            const std::string  refWin   = nameOf(ref->GetFile(-1));
            const std::string  baseWin  = nameOf(base ? base->GetFile(-1) : nullptr);
            if (refWin.empty()) {
                a_out.file = baseWin;   // a created reference: only its base's record can carry a script
                a_out.rule = "a created reference: its base's winning file";
            } else if (baseWin.empty() || refWin == baseWin) {
                a_out.file = refWin;
                a_out.rule = baseWin.empty() ? "the reference's winning file" : "the reference and its base agree";
            } else {
                a_out.file = nameOf(ref->GetFile(0));   // ruling 2's fallback: the defining plugin
                a_out.rule = "ambiguous (the reference's and its base's winning files differ): the defining plugin";
            }
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

        // A counted reference on `a_ref` only while it is still alive (its count is not 0): a CAS on the engine's
        // own count (NiRefObject::_refCount, public in the fork's NiRefObject.h, low 10 bits, BSHandleRefObject
        // kRefCountMask), so a reference whose last count is being dropped is never revived. Released with
        // DecRefCount (the engine's own release).
        bool TryAddRef(RE::TESObjectREFR* a_ref) {
            std::atomic_ref<std::uint32_t> rc(*const_cast<std::uint32_t*>(&a_ref->_refCount));
            std::uint32_t v = rc.load(std::memory_order_relaxed);
            while ((v & RE::BSHandleRefObject::kRefCountMask) != 0) {
                if (rc.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel)) return true;
            }
            return false;
        }

        // The owner of a form handle, read so that nothing can dangle (2026-11-01 review): the whole read runs
        // under the engine's all-forms READ lock (TESForm::GetAllForms, the lock the fork's LookupByID takes:
        // a form leaves the map only under its WRITE lock), and a reference is also held by a counted reference
        // for the read (a reference is refcounted and may be released on another thread). Only member reads and
        // the AsReference vfunc happen under the lock: no engine call that locks.
        void ReadFormLocked(RE::FormID a_id, bool a_alias, Info& a_out) {
            RE::TESObjectREFR* held = nullptr;
            {
                const auto& [map, lock] = RE::TESForm::GetAllForms();
                const RE::BSReadLockGuard locker{ lock.get() };
                if (!map) return;
                const auto it = map->find(a_id);
                if (it == map->end() || !it->second) return;
                RE::TESForm* form = it->second;
                if (!a_alias) {
                    if (auto* ref = form->AsReference(); ref) {
                        if (!TryAddRef(ref)) {
                            a_out.rule = "a reference being released: not read";
                            return;
                        }
                        held = ref;
                    }
                }
                FilesOf(form, a_out);
                OwnerOf(form, a_alias, a_out);
            }
            if (held) held->DecRefCount();
        }

        // STEP 2 review (2026-11-01, SEV-3): the actor and the item the RUNNING equip native was asked for, from
        // its own frame. Actor.EquipItem / EquipItemEx / EquipItemById are member natives of Actor (the frame's
        // self is the actor) and all three take the item as argument 0. Argument 0 is the Variable right after
        // the frame (StackFrame is 0x40, args follow), unless the frame's page ends before it: then the engine's
        // GetStackFrameVariable (AE 104484 / SE 97746) would continue in the next page, and this returns false
        // (not matched) instead of calling it. Layout verified on the disassembly of all three runtimes
        // (agentlog apmf-papyrus-attribution-step2.md): pages = Stack+0x18 BSTSmallArray<MemoryPageData, 3>
        // (16-byte entries, page pointer first, count at +0x50), MemoryPage = { u32 pageSize, buf at +4 }, the
        // variable address = frame + 0x40 + index * 0x10 while it fits in the frame's page (AE 104494 / SE
        // 97755 / 1.7.104 0x14A04E0 identical). Form handles only: low 32 bits = the FormID. This thread only.
        bool NativeTarget(const Running& a_cur, RE::FormID& a_actor, RE::FormID& a_item) {
            a_actor = a_item = 0;
            if (!a_cur.stack || !a_cur.vm) return false;
            const Frame* top = a_cur.stack->top;
            if (!top || top->owningFunction.get() != a_cur.fn) return false;
            auto* policy = a_cur.vm->handlePolicy;
            if (!policy) return false;
            const auto formOf = [&](const RE::BSScript::Variable& a_v, RE::FormID& a_out) {
                if (!a_v.IsObject()) return false;
                const auto obj = a_v.GetObject();
                if (!obj) return false;
                const RE::VMHandle h = obj->handle;
                if (h == policy->EmptyHandle() || (h >> 48) != 0) return false;
                a_out = static_cast<RE::FormID>(h & 0xFFFFFFFFull);
                return a_out != 0;
            };
            if (!formOf(top->self, a_actor)) return false;
            const auto frameAt = reinterpret_cast<std::uintptr_t>(top);
            const auto argAt   = frameAt + sizeof(Frame);
            for (std::uint32_t i = 0; i < a_cur.stack->pages.size(); ++i) {
                const auto* page = a_cur.stack->pages[i].page.get();
                if (!page) continue;
                const auto head = reinterpret_cast<std::uintptr_t>(page) + 4;
                const auto tail = head + page->pageSize;
                if (frameAt < head || frameAt >= tail) continue;
                if (argAt + sizeof(RE::BSScript::Variable) > tail) return false;   // argument 0 is in the next page
                return formOf(*reinterpret_cast<const RE::BSScript::Variable*>(argAt), a_item);
            }
            return false;
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
                        a_out.kind = "effect";
                        // An ActiveEffect is not refcounted and the main thread dispels it: its pointer is read
                        // only ON the main thread (a native that equips or acts runs there), never off it.
                        if (REX::W32::GetCurrentThreadId() != g_mainTid.load(std::memory_order_relaxed)) {
                            a_out.rule = "an effect script off the main thread: not read";
                            return true;
                        }
                        auto* ae = static_cast<RE::ActiveEffect*>(policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, h));
                        const RE::EffectSetting* mgef = ae ? ae->GetBaseObject() : nullptr;
                        a_out.kind  = "effect";
                        a_out.owner = mgef ? mgef->GetFormID() : 0;
                        FilesOf(mgef, a_out);
                        OwnerOf(mgef, true, a_out);
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
                ReadFormLocked(formId, alias, a_out);
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

        // ---- STEP 2: the "native must be the one that acts" rule (APMF-B210; header "WHEN") ----
        // The natives that equip an item on an actor: the engine's Actor.EquipItem (AE 54661 / SE 53861,
        // which calls EquipObject synchronously) and SKSE's Actor.EquipItemEx / Actor.EquipItemById.
        constexpr std::string_view kEquipNatives[] = { "Actor.EquipItem", "Actor.EquipItemEx", "Actor.EquipItemById" };
        bool IEq(std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }
        bool IsEquipNative(std::string_view a_name) {
            for (const auto n : kEquipNatives)
                if (IEq(a_name, n)) return true;
            return false;
        }
        bool IsEquipFacet(apmf::sources::Facet f) {
            return f == apmf::sources::Facet::kEquipWeapon || f == apmf::sources::Facet::kEquipArmor;
        }

        // One "ranked as" line per (facet, source) at most every 10 s, and one "stays DirectCalls" line per
        // (facet, reason) at most every 10 s. Leaf mutex, bounded.
        std::mutex                                       g_rankLogMx;
        std::unordered_map<std::uint64_t, std::uint64_t> g_rankLogMs;
        bool RankLineDue(std::uint64_t a_key) {
            const std::uint64_t now = apmf::clock::MonotonicMs();
            std::scoped_lock    lock(g_rankLogMx);
            if (g_rankLogMs.size() >= 1024) g_rankLogMs.clear();
            auto [it, fresh] = g_rankLogMs.try_emplace(a_key, now);
            if (fresh) return true;
            if (now - it->second < 10000) return false;
            it->second = now;
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
            g_rank.store(GetPrivateProfileIntA("Harbinger", "bPapyrusAttribution", 1, kIni) != 0);
            g_probe.store(GetPrivateProfileIntA("Probe", "bPapyrusCaller", 0, kIni) != 0);
            g_ownerDefining.store(GetPrivateProfileIntA("Harbinger", "iPapyrusOwnerFile", 0, kIni) == 1);
            if (!g_rank.load() && !g_probe.load()) {
                g_refused.store(true);
                spdlog::info("[apmf][papyrus-attr] [Harbinger] bPapyrusAttribution=0 and [Probe] bPapyrusCaller=0: caller "
                             "seat off (no vtable written). Papyrus calls rank as DirectCalls.");
                return false;
            }
            spdlog::info("[apmf][papyrus-attr] [Harbinger] bPapyrusAttribution={} ({}), iPapyrusOwnerFile={} ({}), "
                         "[Probe] bPapyrusCaller={}.",
                         g_rank.load() ? 1 : 0,
                         g_rank.load() ? "a Papyrus call ranks as the plugin that owns its script"
                                       : "probe only: Papyrus calls still rank as DirectCalls",
                         g_ownerDefining.load() ? 1 : 0,
                         g_ownerDefining.load() ? "the defining plugin" : "the winning override",
                         g_probe.load() ? 1 : 0);
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
        g_mainTid.store(REX::W32::GetCurrentThreadId(), std::memory_order_relaxed);   // MAIN thread (header)
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

    apmf::sources::SourceId ScriptSource(apmf::sources::Facet a_facet, std::uintptr_t a_callerAddr, RE::FormID a_actor,
                                         RE::FormID a_item) noexcept {
        namespace src = apmf::sources;
        try {
            if (!g_rank.load(std::memory_order_relaxed) || !g_installed.load(std::memory_order_acquire)) return src::kNoSource;
            const Running cur = t_cur;
            if (!cur.fn) return src::kNoSource;   // no Papyrus native on this thread: not a script's call
            const auto facet = static_cast<std::uint64_t>(a_facet);
            const auto stays = [&](const char* a_why, std::uint64_t a_reason, const Info* a_inf) {
                if (RankLineDue((facet << 56) | (0xFFull << 48) | a_reason))
                    spdlog::info("[apmf][papyrus-attr] {} call while native {} runs stays DirectCalls: {}{}. Logged at "
                                 "most once per 10 s per facet and reason.",
                                 src::FacetName(a_facet), FnName(cur.fn), a_why,
                                 a_inf ? fmt::format(" (caller={} bound={} {} owner=0x{} winner={} definer={} rule={})",
                                                     a_inf->caller, a_inf->bound.empty() ? "-" : a_inf->bound, a_inf->kind,
                                                     apmf::log::Hex(a_inf->owner), a_inf->winner.empty() ? "-" : a_inf->winner,
                                                     a_inf->definer.empty() ? "-" : a_inf->definer, a_inf->rule)
                                       : std::string());
                return src::kNoSource;
            };
            // The native must be the one that acts (APMF-B210).
            if (IsEquipFacet(a_facet)) {
                if (!IsEquipNative(FnName(cur.fn))) return stays("the running native is not an equip native", 1, nullptr);
                // ... and this equip IS its own request (2026-11-01 review SEV-3): an equip a DLL makes inside it
                // (a TESEquipEvent sink equipping on the same or another actor) is that DLL's, not the script's.
                RE::FormID na = 0, ni = 0;
                if (!NativeTarget(cur, na, ni)) return stays("the native's own actor and item could not be read", 6, nullptr);
                if (na != a_actor || ni != a_item)
                    return stays("this equip is not the native's own request (another actor or item)", 7, nullptr);
            } else {
                std::uintptr_t lo = 0, hi = 0;
                const auto     vt = *reinterpret_cast<const std::uintptr_t*>(cur.fn);
                if (!a_callerAddr || !src::ModuleRange(a_callerAddr, lo, hi) || vt < lo || vt >= hi)
                    return stays("the running native is not in the caller's module", 2, nullptr);
            }
            Info inf;
            if (!Describe(inf)) return src::kNoSource;
            if (!inf.topIsNative) return stays("its stack's top frame is not the native (layout self-check)", 3, &inf);
            if (inf.file.empty()) return stays("no script owner with a file", 4, &inf);
            const auto id = src::Register(inf.file, src::Kind::kPlugin);
            if (id == src::kNoSource) return stays("the source table is full", 5, &inf);
            if (RankLineDue((facet << 56) | static_cast<std::uint64_t>(static_cast<std::uint32_t>(id))))
                spdlog::info("[apmf][papyrus-attr] {} call ranked as '{}' (priority {}): native={} caller={} bound={} "
                             "(+{} frames) {} owner=0x{} winner={} definer={} rule={}. Logged at most once per 10 s per "
                             "facet and plugin.",
                             src::FacetName(a_facet), inf.file, src::Priority(id, a_facet), inf.native, inf.caller,
                             inf.bound.empty() ? "-" : inf.bound, inf.hops, inf.kind, apmf::log::Hex(inf.owner),
                             inf.winner.empty() ? "-" : inf.winner, inf.definer.empty() ? "-" : inf.definer, inf.rule);
            return id;
        } catch (...) {
            return src::kNoSource;
        }
    }

    void ProbeEquip(RE::FormID a_actor, RE::FormID a_item, const char* a_path, bool a_scriptPath,
                    const char* a_rankedAs) noexcept {
        try {
            if (!g_probe.load(std::memory_order_relaxed) || !g_installed.load(std::memory_order_acquire)) return;
            Info inf;
            const bool running = Describe(inf);
            if (!running && !a_scriptPath) return;   // not Papyrus by either reading: nothing to say
            if (!Admit(a_actor, a_item)) return;
            const char* path   = a_path ? a_path : "?";
            const char* ranked = a_rankedAs ? a_rankedAs : "?";
            if (!running) {
                spdlog::warn("[apmf][papyrus-attr] PROBE equip actor=0x{} item=0x{} path={} ranked-as={} | NO Papyrus "
                             "native is running on this thread ({}): the native that asked is not seated, or its equip "
                             "ran later (a queued apply or a delay functor). Probe line only.",
                             apmf::log::Hex(a_actor), apmf::log::Hex(a_item), path, ranked, REX::W32::GetCurrentThreadId());
                return;
            }
            spdlog::info("[apmf][papyrus-attr] PROBE equip actor=0x{} item=0x{} path={} ranked-as={} | native={} "
                         "top-frame={} caller={} bound={} (+{} frames) {} owner=0x{} definer={} winner={} -> script "
                         "file '{}' ({}). Probe line only: the ranking is the sink's ranked-as.",
                         apmf::log::Hex(a_actor), apmf::log::Hex(a_item), path, ranked, inf.native,
                         inf.topIsNative ? "native" : "MISMATCH", inf.caller, inf.bound.empty() ? "-" : inf.bound, inf.hops,
                         inf.kind, apmf::log::Hex(inf.owner), inf.definer.empty() ? "-" : inf.definer,
                         inf.winner.empty() ? "-" : inf.winner, inf.file.empty() ? "?" : inf.file, inf.rule);
        } catch (...) {
        }
    }

}
