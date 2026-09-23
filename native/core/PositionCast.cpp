#include "PCH.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/PositionCast.h"

#include <algorithm>
#include <cmath>

// See core/PositionCast.h for the mechanism, the evidence and the doctrine.

// Win32 INI read for [PositionCast] -- the same hand-declared extern every other
// INI-gated file in this project uses (PCH does not pull in <Windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall GetPrivateProfileIntA(
    const char* a_appName, const char* a_keyName, long a_default, const char* a_fileName);

namespace apmf::poscast {

    namespace {

        using apmf::log::Hex;

        // Skyrim.esm XMarker (STAT). The marker vanilla itself places for "a spot".
        constexpr RE::FormID kXMarkerBase = 0x0000003B;

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_reason{ "not installed yet (before kDataLoaded)" };
        RE::TESBoundObject*      g_markerBase = nullptr;   // written once in Install (main thread)

        // MAIN THREAD ONLY: every marker APMF placed and has not deleted yet.
        struct LiveMarker {
            RE::ObjectRefHandle handle{};
            RE::FormID          formID  = 0;
            APMF_API::Handle    request = APMF_API::kInvalidHandle;
            RE::FormID          actor   = 0;
        };
        std::vector<LiveMarker> g_live;

        const char* SpellTypeName(RE::MagicSystem::SpellType a_type) {
            switch (a_type) {
            case RE::MagicSystem::SpellType::kDisease:   return "disease";
            case RE::MagicSystem::SpellType::kAbility:   return "ability";
            case RE::MagicSystem::SpellType::kAddiction: return "addiction";
            default:                                     return "other";
            }
        }

        const char* DeliveryName(RE::MagicSystem::Delivery a_delivery) {
            switch (a_delivery) {
            case RE::MagicSystem::Delivery::kSelf:           return "Self";
            case RE::MagicSystem::Delivery::kTouch:          return "Touch";
            case RE::MagicSystem::Delivery::kAimed:          return "Aimed";
            case RE::MagicSystem::Delivery::kTargetActor:    return "Target Actor";
            case RE::MagicSystem::Delivery::kTargetLocation: return "Target Location";
            default:                                         return "unknown";
            }
        }

        // Returns nullptr when `a_spell` may be position-cast, else the refusal reason.
        // `a_detail` receives a short fact for the log (the delivery name, the type...).
        const char* Ineligible(RE::SpellItem* a_spell, std::string& a_detail) {
            const auto type = a_spell->GetSpellType();
            if (type == RE::MagicSystem::SpellType::kDisease || type == RE::MagicSystem::SpellType::kAbility ||
                type == RE::MagicSystem::SpellType::kAddiction) {
                a_detail = SpellTypeName(type);
                return "spell type refused (Papyrus RemoteCast refuses disease, ability and addiction too)";
            }
            const auto delivery = a_spell->GetDelivery();
            if (delivery != RE::MagicSystem::Delivery::kTargetLocation) {
                a_detail = DeliveryName(delivery);
                return "delivery is not Target Location (only a Target Location spell lands at a point; "
                       "use an ordinary kIntent_Cast with an actor target for this one)";
            }
            const auto casting = a_spell->GetCastingType();
            if (casting != RE::MagicSystem::CastingType::kFireAndForget) {
                a_detail = casting == RE::MagicSystem::CastingType::kConcentration ? "concentration" : "not fire-and-forget";
                return "casting type refused (a position cast is one instant cast; a held stream needs a "
                       "caster that stays)";
            }
            for (auto* effect : a_spell->effects) {
                if (effect && effect->baseEffect &&
                    effect->baseEffect->GetArchetype() == RE::EffectSetting::Archetype::kSummonCreature) {
                    a_detail = std::format("summon effect 0x{}", Hex(effect->baseEffect->GetFormID()));
                    return "SUMMON refused: the engine applies a summon effect only to the actor that cast "
                           "it, so a marker can never summon (cast core summon gate, both runtimes). An "
                           "NPC's own summon already lands in front of it -- the engine's "
                           "SummonCreatureEffect picks that spot";
                }
            }
            return nullptr;
        }

        // MAIN THREAD. Disable + SetDelete(true) the tracked marker behind `a_handle`
        // and drop it from the table. Re-checked first: the handle must still resolve
        // (its age bits make a recycled slot fail here), to the same FormID, on the
        // XMarker base, and not already deleted. Anything else is logged and only the
        // table entry is dropped -- APMF never deletes a reference it cannot prove is
        // its own.
        void Retire(RE::ObjectRefHandle::native_handle_type a_handle, const char* a_when) {
            const auto it = std::find_if(g_live.begin(), g_live.end(), [&](const LiveMarker& m) {
                return m.handle.native_handle() == a_handle;
            });
            if (it == g_live.end()) {
                // ResetAll dropped it at a world boundary. Nothing to do, and nothing
                // of the new world is touched.
                spdlog::info("[poscast] marker handle 0x{} not tracked any more ({}) -- nothing deleted.",
                             Hex(a_handle), a_when);
                return;
            }
            const LiveMarker m = *it;
            g_live.erase(it);

            auto  refPtr = m.handle.get();
            auto* ref    = refPtr.get();
            if (!ref) {
                spdlog::warn("[poscast] request {} marker 0x{} ({}): handle no longer resolves -- already gone, "
                             "nothing deleted.", m.request, Hex(m.formID), a_when);
                return;
            }
            if (ref->GetFormID() != m.formID || ref->GetBaseObject() != g_markerBase) {
                spdlog::error("[poscast] request {} marker 0x{} ({}): handle now resolves to 0x{} (base 0x{}) -- "
                              "NOT ours, left untouched.", m.request, Hex(m.formID), a_when,
                              Hex(ref->GetFormID()),
                              Hex(ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0));
                return;
            }
            if (ref->IsDeleted()) {
                spdlog::info("[poscast] request {} marker 0x{} ({}): already deleted.", m.request, Hex(m.formID),
                             a_when);
                return;
            }
            if (!ref->IsDisabled()) ref->Disable();   // TESObjectREFR vfunc 0x89 (both runtimes, verified)
            ref->SetDelete(true);                     // TESObjectREFR vfunc 0x23 (both runtimes, verified)
            spdlog::info("[poscast] request {} marker 0x{} deleted ({}). {} marker(s) still live.", m.request,
                         Hex(m.formID), a_when, g_live.size());
        }

        // MAIN THREAD (runs from apmf::mainthread::Pump). The whole delivery.
        void Deliver(APMF_API::Handle a_request, RE::FormID a_actor, RE::FormID a_spell, RE::NiPoint3 a_point) {
            const auto refuse = [&](std::string_view a_why) {
                spdlog::warn("[poscast] request {} REFUSED (actor 0x{}, spell 0x{}, point {:.0f},{:.0f},{:.0f}): {}",
                             a_request, Hex(a_actor), Hex(a_spell), a_point.x, a_point.y, a_point.z, a_why);
            };

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            if (!actor) return refuse("the actor FormID is not a loaded actor");
            if (actor->IsDead()) return refuse("the actor is dead");
            if (!actor->Is3DLoaded()) return refuse("the actor has no 3D loaded");
            auto* cell = actor->GetParentCell();
            if (!cell || !cell->IsAttached()) return refuse("the actor's cell is not attached");

            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spell);
            if (!spell) return refuse("param.form is not a SpellItem");
            std::string detail;
            if (const char* why = Ineligible(spell, detail)) {
                return refuse(std::format("{} [{}]", why, detail));
            }

            if (g_live.size() >= kMaxLiveMarkers) {
                return refuse(std::format("{} markers are already live (the cap); each lives one frame, so this "
                                          "is a burst of more than {} position casts in one frame",
                                          g_live.size(), kMaxLiveMarkers));
            }

            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh || !g_markerBase) return refuse("TESDataHandler or the XMarker base is unavailable");

            // The same engine call CommonLib's PlaceObjectAtMe wraps (TESDataHandler
            // 13625 / 13723), with the point as the location, the actor's cell and
            // worldspace, and forcePersist = false.
            const RE::ObjectRefHandle handle = dh->CreateReferenceAtLocation(
                g_markerBase, a_point, RE::NiPoint3{}, cell, actor->GetWorldspace(), nullptr, nullptr,
                RE::ObjectRefHandle(), false, true);
            auto  markerPtr = handle.get();
            auto* marker    = markerPtr.get();
            if (!marker) return refuse("the engine placed no marker (CreateReferenceAtLocation returned no reference)");

            // Tracked from this line on, so every exit below deletes it.
            g_live.push_back(LiveMarker{ handle, marker->GetFormID(), a_request, a_actor });

            auto* mcell = marker->GetParentCell();
            if (!mcell || !mcell->IsAttached()) {
                Retire(handle.native_handle(), "refused before the cast");
                return refuse(std::format("the marker 0x{} is not in an attached cell (Papyrus RemoteCast refuses "
                                          "the same)", Hex(marker->GetFormID())));
            }

            auto* caster = marker->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
            if (!caster) {
                Retire(handle.native_handle(), "refused before the cast");
                return refuse(std::format("the marker 0x{} has no instant magic caster", Hex(marker->GetFormID())));
            }

            // Papyrus Spell.RemoteCast's own sequence (AE 55744 / SE 55168).
            caster->InterruptCast(false);
            caster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, actor);

            spdlog::info("[poscast] request {} DELIVERED: spell 0x{} ({}) cast from marker 0x{} at {:.0f},{:.0f},{:.0f} "
                         "(cell 0x{}), blamed on actor 0x{}. Marker deleted next frame.",
                         a_request, Hex(a_spell), spell->GetName() ? spell->GetName() : "?", Hex(marker->GetFormID()), a_point.x, a_point.y,
                         a_point.z, Hex(mcell->GetFormID()), Hex(a_actor));

            // One hop later: the pump runs this on the NEXT frame (a Post made during
            // Pump waits for the next Pump), after the cast has fully returned.
            const auto native = handle.native_handle();
            apmf::mainthread::Post([native] { Retire(native, "one frame after delivery"); });
        }

    }

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;

        if (REL::Module::IsVR()) {
            g_reason.store("VR runtime (the cast-core evidence was read on 1.6.1170 and 1.5.97 only)");
            spdlog::warn("[poscast] NOT installed -- {}.", g_reason.load());
            return;
        }
        // CLAUDE.md rule 11: the exact two runtimes whose cast core, NonActorMagicCaster,
        // RemoteCast, Disable and SetDelete were read (Docs/ADDRESS-TABLE-2026-09-15.md,
        // ADDENDUM 2026-09-23). Never a family test.
        const auto game = REL::Module::get().version();
        if (game != REL::Version{ 1, 6, 1170, 0 } && game != REL::Version{ 1, 5, 97, 0 }) {
            g_reason.store("runtime not verified for the position cast (only 1.6.1170 and 1.5.97 are)");
            spdlog::warn("[poscast] NOT installed -- {} (running {}).", g_reason.load(), game.string("."));
            return;
        }
        if (GetPrivateProfileIntA("PositionCast", "bPositionCast", 1, "Data/SKSE/Plugins/APMF.ini") == 0) {
            g_reason.store("[PositionCast] bPositionCast=0 in Data/SKSE/Plugins/APMF.ini");
            spdlog::info("[poscast] NOT installed -- {}.", g_reason.load());
            return;
        }
        auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(kXMarkerBase);
        if (!base || base->GetFormType() != RE::FormType::Static) {
            g_reason.store("Skyrim.esm XMarker (0x3B) did not resolve to a STAT");
            spdlog::error("[poscast] NOT installed -- {}.", g_reason.load());
            return;
        }
        g_markerBase = base;
        g_reason.store("");
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[poscast] installed on {}: a kIntent_Cast RequestEx with kCastFlag_AtPosition casts a Target "
                     "Location spell FROM an XMarker at the client's point, blamed on the client's actor "
                     "(Papyrus RemoteCast's sequence). Summons and every other delivery are refused by name.",
                     game.string("."));
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_reason.load(); }

    bool Enqueue(APMF_API::Handle a_handle, RE::FormID a_actor, const APMF_API::APMF_Param& a_param) {
        const std::uint32_t flags = static_cast<std::uint32_t>(a_param.ival);
        if (!Installed()) {
            spdlog::warn("[poscast] request REFUSED (actor 0x{}): position cast not installed -- {}.", Hex(a_actor),
                         NotInstalledReason());
            return false;
        }
        if (const std::uint32_t extra = flags & ~static_cast<std::uint32_t>(APMF_API::kCastFlag_AtPosition)) {
            spdlog::warn("[poscast] request REFUSED (actor 0x{}): other cast flags set alongside kCastFlag_AtPosition "
                         "(0x{}). A position cast has no hand, no stream and no stop percent; send the flag alone.",
                         Hex(a_actor), Hex(extra));
            return false;
        }
        if (a_actor == 0 || a_param.form == 0) {
            spdlog::warn("[poscast] request REFUSED: actor 0x{} / spell 0x{} -- both are required.", Hex(a_actor),
                         Hex(a_param.form));
            return false;
        }
        if (!std::isfinite(a_param.posX) || !std::isfinite(a_param.posY) || !std::isfinite(a_param.posZ)) {
            spdlog::warn("[poscast] request REFUSED (actor 0x{}): param.pos is not a finite point.", Hex(a_actor));
            return false;
        }
        const RE::NiPoint3 point{ a_param.posX, a_param.posY, a_param.posZ };
        const RE::FormID   spell = a_param.form;
        apmf::mainthread::Post([a_handle, a_actor, spell, point] { Deliver(a_handle, a_actor, spell, point); });
        spdlog::info("[poscast] request {} queued: actor 0x{} spell 0x{} at {:.0f},{:.0f},{:.0f}.", a_handle,
                     Hex(a_actor), Hex(spell), point.x, point.y, point.z);
        return true;
    }

    void ResetAll(const char* a_why) {
        if (!g_live.empty()) {
            spdlog::warn("[poscast] {} -- forgetting {} live marker(s) WITHOUT deleting them (they belong to the world "
                         "being replaced; a save taken in that frame keeps them as inert XMarkers).",
                         a_why, g_live.size());
        }
        g_live.clear();
    }

}
