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
        std::atomic<bool>        g_markersSupported{ false };   // runtime + base gate, independent of the INI

        // MAIN THREAD ONLY: every marker APMF placed and has not deleted yet.
        struct LiveMarker {
            RE::ObjectRefHandle handle{};
            RE::FormID          formID  = 0;
            APMF_API::Handle    request = APMF_API::kInvalidHandle;
            RE::FormID          actor   = 0;
        };
        std::vector<LiveMarker> g_live;

        // ── THE MARKER LEDGER (co-saved, record kMarkerRecordType). MAIN THREAD ONLY. ──
        // Every XMarker PlaceMarker created and DeleteMarker has not yet deleted, for
        // BOTH callers (position casts and ch.19 position legs). It is what lets a load
        // prove which markers in the loaded world are APMF's. See PositionCast.h.
        struct OwnedMarker {
            RE::FormID          id = 0;
            RE::NiPoint3        pos{};
            RE::ObjectRefHandle handle{};   // empty for a carried entry
        };
        std::vector<OwnedMarker> g_owned;   // placed in THIS world, handle live
        std::vector<OwnedMarker> g_carry;   // read from a co-save, or whose handle went stale: swept at
                                            // the next kPostLoadGame where they resolve

        // Bound g_carry at kMaxCarriedMarkers wherever it grows (review F8b): the oldest
        // records go first, loudly. A dropped record's marker, if it still exists, stays
        // in the save. MAIN THREAD.
        void CapCarried(const char* a_where) {
            if (g_carry.size() <= kMaxCarriedMarkers) return;
            const std::size_t drop = g_carry.size() - kMaxCarriedMarkers;
            spdlog::warn("[marker] {} -- {} carried marker record(s) over the cap of {}; the oldest {} dropped (those "
                         "markers, if they still exist, stay in the save).", a_where, g_carry.size(),
                         kMaxCarriedMarkers, drop);
            g_carry.erase(g_carry.begin(), g_carry.begin() + static_cast<std::ptrdiff_t>(drop));
        }

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

            if (const char* why = DeleteMarker(m.handle, m.formID)) {
                spdlog::warn("[poscast] request {} marker 0x{} ({}): NOT deleted -- {}.", m.request, Hex(m.formID),
                             a_when, why);
                return;
            }
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

            std::string placeWhy;
            const RE::ObjectRefHandle handle = PlaceMarker(actor, a_point, placeWhy);
            auto  markerPtr = handle.get();
            auto* marker    = markerPtr.get();
            if (!marker) return refuse(placeWhy);

            // Tracked from this line on, so every exit below deletes it.
            g_live.push_back(LiveMarker{ handle, marker->GetFormID(), a_request, a_actor });
            auto* mcell = marker->GetParentCell();   // PlaceMarker proved it attached

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

    bool MarkersSupported() { return g_markersSupported.load(std::memory_order_acquire); }

    RE::ObjectRefHandle PlaceMarker(RE::Actor* a_actor, const RE::NiPoint3& a_point, std::string& a_why) {
        if (!MarkersSupported() || !g_markerBase) {
            a_why = "XMarker placement is not available on this runtime (VR, unverified build, or before kDataLoaded)";
            return {};
        }
        if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z)) {
            a_why = "the point is not finite";
            return {};
        }
        auto* actorCell = a_actor ? a_actor->GetParentCell() : nullptr;
        if (!actorCell || !actorCell->IsAttached()) {
            a_why = "the actor's cell is not attached";
            return {};
        }
        // THE CELL THAT CONTAINS THE POINT (review F3, 2026-09-23). An interior has no
        // other cell, so an interior actor's point is placed in its own cell. Outdoors
        // the point may lie in a neighbouring cell, so it is resolved by coordinates
        // with TES::GetCell(const NiPoint3&) (RELOCATION_ID(13177, 13322); SE 0x155090 /
        // AE 0x19e850, identical): with no interior loaded it floors x/y to the 4096u
        // grid and returns the LOADED grid cell there (state 6 or 7), else null. That
        // cell must be an attached exterior cell of the actor's own worldspace, or the
        // request is refused by name. Nothing is guessed.
        RE::TESObjectCELL* cell = actorCell;
        if (actorCell->IsExteriorCell()) {
            auto* tes = RE::TES::GetSingleton();
            RE::TESObjectCELL* at = tes ? tes->GetCell(a_point) : nullptr;
            if (!at) {
                a_why = "no loaded exterior cell contains the point (it is outside the loaded grid)";
                return {};
            }
            if (!at->IsExteriorCell() || at->GetRuntimeData().worldSpace != a_actor->GetWorldspace()) {
                a_why = "the cell at the point is not an exterior cell of the actor's worldspace";
                return {};
            }
            if (!at->IsAttached()) {
                a_why = std::format("the cell 0x{} that contains the point is not attached", Hex(at->GetFormID()));
                return {};
            }
            cell = at;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            a_why = "TESDataHandler is unavailable";
            return {};
        }
        // The same engine call CommonLib's PlaceObjectAtMe wraps (TESDataHandler
        // 13625 / 13723), with the point as the location, the actor's cell and
        // worldspace, and forcePersist = false.
        const RE::ObjectRefHandle handle = dh->CreateReferenceAtLocation(
            g_markerBase, a_point, RE::NiPoint3{}, cell, a_actor->GetWorldspace(), nullptr, nullptr,
            RE::ObjectRefHandle(), false, true);
        auto  ptr    = handle.get();
        auto* marker = ptr.get();
        if (!marker) {
            a_why = "the engine placed no marker (CreateReferenceAtLocation returned no reference)";
            return {};
        }
        auto* mcell = marker->GetParentCell();
        if (!mcell || !mcell->IsAttached()) {
            const RE::FormID id = marker->GetFormID();
            const char* del = DeleteMarker(handle, id);
            a_why = std::format("the marker 0x{} is not in an attached cell (Papyrus RemoteCast refuses the same); "
                                "it was {}", Hex(id), del ? std::format("NOT deleted: {}", del) : "deleted");
            return {};
        }
        // Into the co-saved ledger from this line on: a save taken while it lives
        // records it, and the load of that save deletes it (SweepCarriedMarkers).
        g_owned.push_back(OwnedMarker{ marker->GetFormID(), marker->GetPosition(), handle });
        return handle;
    }

    const char* DeleteMarker(const RE::ObjectRefHandle& a_handle, RE::FormID a_formID) {
        // Take this marker out of the live ledger whatever happens below; the only
        // outcome that keeps it recorded is a stale handle (next comment).
        OwnedMarker entry{};
        bool        tracked = false;
        if (const auto it = std::find_if(g_owned.begin(), g_owned.end(),
                                         [&](const OwnedMarker& m) { return m.id == a_formID; });
            it != g_owned.end()) {
            entry   = *it;
            tracked = true;
            g_owned.erase(it);
        }

        auto  refPtr = a_handle.get();
        auto* ref    = refPtr.get();
        if (!ref) {
            // A stale handle does NOT prove the marker is gone: a temporary reference
            // whose cell detached is unloaded from memory but still lives in the
            // cell's saved data. Keep it recorded (FormID + position) so the next load
            // that finds it deletes it, under the same proofs.
            if (tracked) {
                entry.handle = {};
                g_carry.push_back(entry);
                CapCarried("a stale handle during the session");
            }
            return "the handle no longer resolves (unloaded or gone); kept in the co-saved ledger for the next load";
        }
        if (ref->GetFormID() != a_formID || !g_markerBase || ref->GetBaseObject() != g_markerBase) {
            spdlog::error("[marker] handle for 0x{} now resolves to 0x{} (base 0x{}) -- NOT ours, left untouched.",
                          Hex(a_formID), Hex(ref->GetFormID()),
                          Hex(ref->GetBaseObject() ? ref->GetBaseObject()->GetFormID() : 0));
            return "the handle resolves to a reference that is not this XMarker";
        }
        if (ref->IsDeleted()) return "already deleted";
        if (!ref->IsDisabled()) ref->Disable();   // TESObjectREFR vfunc 0x89 (both runtimes, verified)
        ref->SetDelete(true);                     // TESObjectREFR vfunc 0x23 (both runtimes, verified)
        return nullptr;
    }

    // ── Co-save (record kMarkerRecordType, v1) ────────────────────────────────────

    void SaveMarkers(SKSE::SerializationInterface* a_intf) {
        const std::uint32_t count = static_cast<std::uint32_t>(g_owned.size() + g_carry.size());
        if (!a_intf->OpenRecord(kMarkerRecordType, kMarkerRecordVersion)) {
            spdlog::error("[marker] OpenRecord failed -- {} APMF XMarker(s) NOT co-saved; a load of this save cannot "
                          "sweep them.", count);
            return;
        }
        bool ok = a_intf->WriteRecordData(&count, sizeof(count));
        const auto write = [&](const OwnedMarker& m) {
            ok = ok && a_intf->WriteRecordData(&m.id, sizeof(m.id));
            ok = ok && a_intf->WriteRecordData(&m.pos.x, sizeof(m.pos.x));
            ok = ok && a_intf->WriteRecordData(&m.pos.y, sizeof(m.pos.y));
            ok = ok && a_intf->WriteRecordData(&m.pos.z, sizeof(m.pos.z));
        };
        for (const auto& m : g_owned) write(m);
        for (const auto& m : g_carry) write(m);
        if (!ok) spdlog::error("[marker] WriteRecordData failed mid-record -- the marker record may be truncated.");
        else if (count != 0)
            spdlog::info("[marker] co-saved {} APMF XMarker(s) ({} live, {} carried) for the load-time sweep.", count,
                         g_owned.size(), g_carry.size());
    }

    void LoadMarkers(SKSE::SerializationInterface* a_intf, std::uint32_t a_version) {
        if (a_version > kMarkerRecordVersion) {
            spdlog::warn("[marker] co-save record v{} is newer than this build knows (v{}) -- skipped; nothing it "
                         "names will be swept.", a_version, kMarkerRecordVersion);
            return;
        }
        // v1 (the only version): u32 count, then count x { u32 formID, f32 x, f32 y, f32 z }.
        std::uint32_t count = 0;
        if (a_intf->ReadRecordData(&count, sizeof(count)) != sizeof(count)) {
            spdlog::error("[marker] co-save record v{} is truncated: its entry count could not be read -- nothing "
                          "from it will be swept.", a_version);
            return;
        }
        std::size_t read = 0, refused = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            OwnedMarker m{};
            if (a_intf->ReadRecordData(&m.id, sizeof(m.id)) != sizeof(m.id) ||
                a_intf->ReadRecordData(&m.pos.x, sizeof(m.pos.x)) != sizeof(m.pos.x) ||
                a_intf->ReadRecordData(&m.pos.y, sizeof(m.pos.y)) != sizeof(m.pos.y) ||
                a_intf->ReadRecordData(&m.pos.z, sizeof(m.pos.z)) != sizeof(m.pos.z)) {
                spdlog::error("[marker] record truncated after {} of {} entries.", i, count);
                break;
            }
            // Only a runtime-created (0xFF) reference can be one of ours. SKSE passes
            // 0xFF ids through ResolveFormID unchanged; anything else is not ours.
            RE::FormID resolved = 0;
            if (!a_intf->ResolveFormID(m.id, resolved) || (resolved >> 24) != 0xFF) {
                ++refused;
                continue;
            }
            m.id = resolved;
            g_carry.push_back(m);
            ++read;
        }
        spdlog::info("[marker] read {} recorded APMF XMarker(s) from the co-save (record v{}, {} refused as not "
                     "0xFF); they are swept on the first main-thread pump after the load.", read, a_version, refused);
        CapCarried("co-save load");
    }

    void RevertMarkers() {
        if (!g_owned.empty() || !g_carry.empty())
            spdlog::info("[marker] revert -- forgetting {} live and {} carried marker record(s) of the outgoing world "
                         "(its markers were co-saved with it if a save captured them).",
                         g_owned.size(), g_carry.size());
        g_owned.clear();
        g_carry.clear();
    }

    void SweepCarriedMarkers(const char* a_when) {
        if (g_carry.empty()) return;
        std::size_t swept = 0, forgotten = 0, kept = 0;
        std::vector<OwnedMarker> keep;
        for (const auto& m : g_carry) {
            // A marker placed in THIS session can never be a recorded one; if an id
            // somehow matches a live entry, the live entry owns it. Forget the record.
            if (std::any_of(g_owned.begin(), g_owned.end(), [&](const OwnedMarker& o) { return o.id == m.id; })) {
                ++forgotten;
                continue;
            }
            auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(m.id);
            if (!ref) {
                // Not in memory (its cell is not loaded now). Still recorded: kept for
                // the next save and swept by a later load that finds it.
                keep.push_back(m);
                ++kept;
                continue;
            }
            // THE THREE PROOFS: recorded FormID (the lookup), XMarker base, recorded
            // position within 1u. Any failure: forgotten, never touched.
            const bool baseOk = g_markerBase && ref->GetBaseObject() == g_markerBase;
            const bool posOk  = ref->GetPosition().GetDistance(m.pos) <= 1.0f;
            if (!baseOk || !posOk || ref->IsDeleted()) {
                spdlog::info("[marker] {} -- recorded 0x{} {} -- forgotten, NOT touched.", a_when, Hex(m.id),
                             ref->IsDeleted() ? "is already deleted"
                             : !baseOk        ? "is no longer an XMarker"
                                              : "is not at its recorded position");
                ++forgotten;
                continue;
            }
            if (!ref->IsDisabled()) ref->Disable();   // TESObjectREFR vfunc 0x89 (both runtimes, verified)
            ref->SetDelete(true);                     // TESObjectREFR vfunc 0x23 (both runtimes, verified)
            ++swept;
        }
        g_carry = std::move(keep);
        CapCarried(a_when);
        spdlog::info("[marker] {} sweep -- {} APMF XMarker(s) deleted, {} forgotten (failed a proof), {} not loaded "
                     "(kept for the next load).", a_when, swept, forgotten, kept);
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
        auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(kXMarkerBase);
        if (!base || base->GetFormType() != RE::FormType::Static) {
            g_reason.store("Skyrim.esm XMarker (0x3B) did not resolve to a STAT");
            spdlog::error("[poscast] NOT installed -- {}.", g_reason.load());
            return;
        }
        g_markerBase = base;
        // The marker helpers (also ch.19's position legs) need only the runtime and
        // the base. The INI below switches off the position CAST alone.
        g_markersSupported.store(true, std::memory_order_release);
        // DEFAULT OFF (review F1 + marth 2026-09-23): INVARIANTS #0 action (e) is adopted
        // with condition (8), "not an endpoint" -- the actor does not animate, and proper
        // animations are required for ALL actions. A missing key reads 0 = off; only an
        // explicit bPositionCast=1 installs it.
        if (GetPrivateProfileIntA("PositionCast", "bPositionCast", 0, "Data/SKSE/Plugins/APMF.ini") == 0) {
            g_reason.store("[PositionCast] bPositionCast is 0 (the default: a marker cast does not animate the actor, "
                           "and INVARIANTS #0 (e) condition 8 says it is not an endpoint)");
            spdlog::info("[poscast] NOT installed -- {} (the XMarker helpers ch.19 uses stay available).",
                         g_reason.load());
            return;
        }
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
        // STATIC ELIGIBILITY, SYNCHRONOUSLY (review F7b): delivery, casting type, summon
        // effect and spell type are the spell RECORD's own data, fixed after kDataLoaded,
        // so a wrong spell is refused at the call with kInvalidHandle instead of a live
        // handle that later does nothing. TESForm::LookupByID takes the form map under
        // its own read lock (CommonLib TESForm.h), so it is safe from the caller's thread;
        // nothing here writes. Deliver re-checks the same things on the main thread.
        {
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_param.form);
            if (!sp) {
                spdlog::warn("[poscast] request REFUSED (actor 0x{}): param.form 0x{} is not a SpellItem.", Hex(a_actor),
                             Hex(a_param.form));
                return false;
            }
            std::string detail;
            if (const char* why = Ineligible(sp, detail)) {
                spdlog::warn("[poscast] request REFUSED (actor 0x{}, spell 0x{}): {} [{}]", Hex(a_actor),
                             Hex(a_param.form), why, detail);
                return false;
            }
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
                         "being replaced; a save that captured them records them, and its load deletes them).",
                         a_why, g_live.size());
        }
        g_live.clear();
    }

}
