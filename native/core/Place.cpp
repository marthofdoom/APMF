#include "PCH.h"
#include "core/Place.h"

#include "core/Allowance.h"   // SeatVerified(): the mit-3.7 F1 self-check gate; RuntimeSupported()
#include "core/Clock.h"
#include "core/ControlMap.h"  // TryGetOwningClaimBasis: the ch.19 / ch.1 owner (lock-free RCU read)
#include "core/Log.h"
#include "core/MainThread.h"

#include <cmath>
#include <cstring>

// ABI v28 PLACE. See core/Place.h for the engine road and the shape (a one-shot, never a claim).
namespace apmf::place {

    namespace {
        using apmf::log::Hex;
        using APMF_API::Handle;

        std::atomic<bool>        g_installed{ false };
        std::atomic<const char*> g_reason{ "not installed yet (before kDataLoaded)" };

        // A world generation: ResetAll bumps it, and a move posted under an older one drops itself, so no
        // placement asked for in one world runs in the next (the Discard() in plugin.cpp drops it too).
        std::atomic<std::uint32_t> g_gen{ 1 };

        // Per-handle records. Leaf lock: nothing is called while it is held (no game call, no log).
        struct Rec {
            APMF_API::APMF_PlaceStateInfo info{};   // size/handle/msSinceChange are filled at the read
            std::uint64_t                 changedMs = 0;
        };
        constexpr std::size_t           kMaxRecords = 128;
        std::mutex                      g_mx;
        std::unordered_map<Handle, Rec> g_recs;
        std::uint32_t                   g_seq = 0;   // under g_mx

        bool Terminal(std::uint32_t s) {
            return s == APMF_API::kPlaceState_Placed || s == APMF_API::kPlaceState_Refused ||
                   s == APMF_API::kPlaceState_Failed;
        }

        // Under g_mx: room for one more record. Drops finished records past the grace, then the oldest
        // finished one. A table full of QUEUED placements refuses the new one (false).
        bool MakeRoomLocked(std::uint64_t a_now) {
            for (auto it = g_recs.begin(); it != g_recs.end();) {
                if (Terminal(it->second.info.state) && a_now - it->second.changedMs > APMF_API::kPlaceStateGraceMs)
                    it = g_recs.erase(it);
                else
                    ++it;
            }
            if (g_recs.size() < kMaxRecords) return true;
            auto oldest = g_recs.end();
            for (auto it = g_recs.begin(); it != g_recs.end(); ++it) {
                if (!Terminal(it->second.info.state)) continue;
                if (oldest == g_recs.end() || it->second.changedMs < oldest->second.changedMs) oldest = it;
            }
            if (oldest == g_recs.end()) return false;
            g_recs.erase(oldest);
            return true;
        }

        // Game thread: finish a record (Refused / Placed / Failed). Returns false when the record is gone
        // (a reset since the post): the caller then logs nothing more.
        bool Finish(Handle a_handle, std::uint32_t a_state, std::uint32_t a_why, Handle a_blocker,
                    RE::FormID a_cell, const RE::NiPoint3* a_point, float a_yaw, std::uint32_t a_info) {
            std::scoped_lock lock(g_mx);
            const auto it = g_recs.find(a_handle);
            if (it == g_recs.end()) return false;
            auto& i   = it->second.info;
            i.state   = a_state;
            i.why     = a_why;
            i.blocker = a_blocker;
            i.cell    = a_cell;
            if (a_point) {
                i.x   = a_point->x;
                i.y   = a_point->y;
                i.z   = a_point->z;
                i.yaw = a_yaw;
            }
            i.info |= a_info;
            i.seq                = ++g_seq;
            it->second.changedMs = apmf::clock::MonotonicMs();
            return true;
        }

        const char* WhyText(std::uint32_t a_why) {
            switch (a_why) {
            case APMF_API::kPlaceWhy_ActorGone:     return "the actor FormID is not an actor in memory";
            case APMF_API::kPlaceWhy_DestGone:      return "the destination FormID resolves to nothing";
            case APMF_API::kPlaceWhy_DestInvalid:   return "the destination is not a usable reference or cell";
            case APMF_API::kPlaceWhy_DestExterior:  return "the destination is an EXTERIOR cell (name a reference in "
                                                           "that worldspace and an offset instead)";
            case APMF_API::kPlaceWhy_ActorLoaded:   return "kPlace_OnlyIfUnloaded and the actor is loaded";
            case APMF_API::kPlaceWhy_PlayerPresent: return "kPlace_RefuseIfPlayerPresent and the player is in the "
                                                           "actor's cell or the destination cell";
            case APMF_API::kPlaceWhy_TravelOwned:   return "another client's kIntent_Travel (ch.19) claim owns the actor";
            case APMF_API::kPlaceWhy_HoldOwned:     return "another client's kIntent_MovementBlock (ch.1) claim owns "
                                                           "the actor";
            case APMF_API::kPlaceWhy_NotMoved:      return "the engine move returned but the actor's position did not "
                                                           "read back at the point";
            case APMF_API::kPlaceWhy_Error:         return "an exception inside Harbinger's game-thread step";
            default:                                return "unknown";
            }
        }

        // TESObjectREFR::MoveTo_Impl (private in the fork). Signature read off all three builds: rcx this,
        // rdx &handle (read on the player branch only), r8 cell, r9 worldspace, [rsp+0x28] &point,
        // [rsp+0x30] &rotation (a Character reads rotation Z only). Verified row Place.TESObjectREFR.MoveTo.
        using MoveToImpl_t = void (*)(RE::TESObjectREFR*, const RE::ObjectRefHandle&, RE::TESObjectCELL*,
                                      RE::TESWorldSpace*, const RE::NiPoint3&, const RE::NiPoint3&);

        // GAME THREAD (apmf::mainthread::Pump). The whole placement after the call's checks.
        void Run(Handle a_handle, RE::FormID a_actor, RE::FormID a_dest, RE::NiPoint3 a_offset, float a_yaw,
                 std::uint32_t a_flags, apmf::sources::SourceId a_client) {
            const auto refuse = [&](std::uint32_t a_why, Handle a_blocker, const std::string& a_detail) {
                if (!Finish(a_handle, APMF_API::kPlaceState_Refused, a_why, a_blocker, 0, nullptr, 0.0f, 0)) return;
                spdlog::warn("[place] {} REFUSED (actor 0x{}, dest 0x{}): {}{}", a_handle, Hex(a_actor), Hex(a_dest),
                             WhyText(a_why), a_detail);
            };

            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actor);
            if (!actor) return refuse(APMF_API::kPlaceWhy_ActorGone, 0, "");
            if (actor->IsPlayerRef()) return refuse(APMF_API::kPlaceWhy_ActorGone, 0, " (the player is never placed)");

            // The destination: a REFERENCE (the actor lands at its position + the offset, in its cell and
            // worldspace: Papyrus MoveTo(akTarget, x, y, z)) or an INTERIOR CELL (the point in it).
            auto* destForm = RE::TESForm::LookupByID(a_dest);
            if (!destForm) return refuse(APMF_API::kPlaceWhy_DestGone, 0, "");
            RE::TESObjectCELL*  cell  = nullptr;
            RE::TESWorldSpace*  ws    = nullptr;
            RE::NiPoint3        point = a_offset;
            std::uint32_t       info  = 0;
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_dest)) {
                if (ref->GetFormID() == a_actor) return refuse(APMF_API::kPlaceWhy_DestInvalid, 0, " (the actor itself)");
                if (ref->IsDeleted()) return refuse(APMF_API::kPlaceWhy_DestInvalid, 0, " (a deleted reference)");
                cell = ref->GetParentCell();
                if (!cell) return refuse(APMF_API::kPlaceWhy_DestInvalid, 0, " (the reference is in no cell)");
                ws    = ref->GetWorldspace();   // parentCell is set, so this reads the cell only (no vfunc)
                point = ref->GetPosition() + a_offset;
            } else if (auto* c = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_dest)) {
                if (!c->IsInteriorCell()) return refuse(APMF_API::kPlaceWhy_DestExterior, 0, "");
                cell = c;
                info |= APMF_API::kPlaceInfo_FromCell;
            } else {
                return refuse(APMF_API::kPlaceWhy_DestInvalid, 0,
                              std::format(" (form type {}, neither a reference nor a cell)",
                                          static_cast<int>(destForm->GetFormType())));
            }

            // LOADED = the actor has 3D, or stands in an attached INTERIOR cell. Outdoors only the 3D counts: an
            // unloaded persistent actor's parent cell can be the worldspace's persistent cell, which is not a
            // place the player can see, and its attach state is not one this file reads anything into.
            auto* fromCell = actor->GetParentCell();
            const bool loaded =
                actor->Is3DLoaded() || (fromCell && fromCell->IsInteriorCell() && fromCell->IsAttached());
            if ((a_flags & APMF_API::kPlace_OnlyIfUnloaded) && loaded)
                return refuse(APMF_API::kPlaceWhy_ActorLoaded, 0, "");
            if (a_flags & APMF_API::kPlace_RefuseIfPlayerPresent) {
                auto* pc    = RE::PlayerCharacter::GetSingleton();
                auto* pcell = pc ? pc->GetParentCell() : nullptr;
                if (pcell && (pcell == fromCell || pcell == cell))
                    return refuse(APMF_API::kPlaceWhy_PlayerPresent, 0,
                                  std::format(" (player cell 0x{})", Hex(pcell->GetFormID())));
            }

            // ARBITRATION: the winning ch.19 travel / ch.1 hold claim on the actor must be this client's own.
            // A claim no client can be named for (an internal one, or a caller Harbinger could not attribute)
            // counts as another client's: refused, never guessed.
            struct Owned { APMF_API::Intent intent; std::uint32_t why; const char* name; };
            constexpr Owned kOwned[] = {
                { APMF_API::kIntent_Travel, APMF_API::kPlaceWhy_TravelOwned, "ch.19 travel" },
                { APMF_API::kIntent_MovementBlock, APMF_API::kPlaceWhy_HoldOwned, "ch.1 hold" },
            };
            std::string ownNote;
            for (const auto& o : kOwned) {
                APMF_API::APMF_Param p{};
                float                b     = 0.0f;
                Handle               owner = APMF_API::kInvalidHandle;
                if (!apmf::ControlMap::Get().TryGetOwningClaimBasis(a_actor, o.intent, p, b, &owner) ||
                    owner == APMF_API::kInvalidHandle)
                    continue;
                const auto oc = apmf::sources::ClientForHandle(owner);
                if (oc == apmf::sources::kNoSource || a_client == apmf::sources::kNoSource || oc != a_client) {
                    return refuse(o.why, owner,
                                  std::format(" (h={} from '{}', this request from '{}')", owner,
                                              oc == apmf::sources::kNoSource ? "no client" : apmf::sources::Name(oc),
                                              a_client == apmf::sources::kNoSource ? "no client"
                                                                                   : apmf::sources::Name(a_client)));
                }
                ownNote += std::format(" your {} claim h={} stands;", o.name, owner);
            }

            // Rotation: the actor's own, or the declared yaw. X / Y zeroed as Papyrus MoveTo's task does for a
            // Character (the engine's Character branch reads Z only).
            RE::NiPoint3 rot = actor->GetAngle();
            rot.x = 0.0f;
            rot.y = 0.0f;
            if (a_flags & APMF_API::kPlace_SetYaw) rot.z = a_yaw;
            if (loaded) info |= APMF_API::kPlaceInfo_WasLoaded;

            static REL::Relocation<MoveToImpl_t> s_moveTo{ RELOCATION_ID(56227, 56626) };
            s_moveTo(actor, RE::ObjectRefHandle{}, cell, ws, point, rot);

            // Read back. A position that did not take is a FAILURE, said so, never reported as placed.
            const RE::NiPoint3 after   = actor->GetPosition();
            auto*              nowCell = actor->GetParentCell();
            const float        dist    = after.GetDistance(point);
            if (nowCell != cell) info |= APMF_API::kPlaceInfo_CellDiffers;
            const RE::FormID nowCellId = nowCell ? nowCell->GetFormID() : 0;
            if (dist > 1.0f) {
                if (!Finish(a_handle, APMF_API::kPlaceState_Failed, APMF_API::kPlaceWhy_NotMoved, 0, nowCellId,
                            &point, rot.z, info))
                    return;
                spdlog::error("[place] {} FAILED (actor 0x{}, dest 0x{}): {} -- sent to {:.0f},{:.0f},{:.0f} in cell "
                              "0x{}, reads back {:.0f},{:.0f},{:.0f} in cell 0x{} ({:.0f}u off; was {}).",
                              a_handle, Hex(a_actor), Hex(a_dest), WhyText(APMF_API::kPlaceWhy_NotMoved), point.x,
                              point.y, point.z, Hex(cell->GetFormID()), after.x, after.y, after.z, Hex(nowCellId), dist,
                              loaded ? "loaded" : "unloaded");
                return;
            }
            if (!Finish(a_handle, APMF_API::kPlaceState_Placed, APMF_API::kPlaceWhy_None, 0, nowCellId, &point, rot.z,
                        info))
                return;
            spdlog::info("[place] {} PLACED actor 0x{} at {:.0f},{:.0f},{:.0f} yaw {:.2f} in cell 0x{}{} (dest 0x{}, "
                         "{}; was {} in cell 0x{}).{}",
                         a_handle, Hex(a_actor), after.x, after.y, after.z, rot.z, Hex(nowCellId),
                         (info & APMF_API::kPlaceInfo_CellDiffers)
                             ? std::format(" -- NOT the cell handed to the engine (0x{})", Hex(cell->GetFormID()))
                             : std::string{},
                         Hex(a_dest), (info & APMF_API::kPlaceInfo_FromCell) ? "a cell" : "a reference",
                         loaded ? "loaded" : "unloaded", Hex(fromCell ? fromCell->GetFormID() : 0), ownNote);
        }

        void Deliver(Handle a_handle, RE::FormID a_actor, RE::FormID a_dest, RE::NiPoint3 a_offset, float a_yaw,
                     std::uint32_t a_flags, apmf::sources::SourceId a_client, std::uint32_t a_gen) {
            if (g_gen.load(std::memory_order_acquire) != a_gen) {
                spdlog::info("[place] {} dropped unrun: the world changed since the request (load / new game).",
                             a_handle);
                return;
            }
            try {
                Run(a_handle, a_actor, a_dest, a_offset, a_yaw, a_flags, a_client);
            } catch (...) {
                if (Finish(a_handle, APMF_API::kPlaceState_Failed, APMF_API::kPlaceWhy_Error, 0, 0, nullptr, 0.0f, 0))
                    spdlog::error("[place] {} FAILED (actor 0x{}, dest 0x{}): {}.", a_handle, Hex(a_actor), Hex(a_dest),
                                  WhyText(APMF_API::kPlaceWhy_Error));
            }
        }
    }

    void Install() {
        if (g_installed.load(std::memory_order_relaxed)) return;
        if (REL::Module::IsVR()) {
            g_reason.store("VR runtime (MoveTo_Impl was read on 1.6.1170, 1.5.97 and 1.7.104 only)");
            spdlog::warn("[place] NOT installed -- {}.", g_reason.load());
            return;
        }
        // CLAUDE.md rule 11: the exact builds whose MoveTo_Impl body was read (actor branch identical on all
        // three: StopInteractingQuick then the actor relocation; the handle read on the player branch only).
        // Never a family test.
        if (!apmf::allowance::RuntimeSupported()) {
            g_reason.store("runtime not verified for placement (only 1.6.1170, 1.5.97 and 1.7.104 are)");
            spdlog::warn("[place] NOT installed -- {} (running {}).", g_reason.load(),
                         REL::Module::get().version().string("."));
            return;
        }
        if (!apmf::allowance::SeatVerified(REL::Relocation<std::uintptr_t>{ RELOCATION_ID(56227, 56626) }.address(),
                                           "Place.TESObjectREFR.MoveTo")) {
            g_reason.store("the mit-3.7 self-check refused MoveTo_Impl (row Place.TESObjectREFR.MoveTo)");
            spdlog::error("[place] NOT installed -- {}.", g_reason.load());
            return;
        }
        g_reason.store("installed");
        g_installed.store(true, std::memory_order_release);
        spdlog::info("[place] installed (kIntent_Place, ABI v28): one MoveTo_Impl per request, game thread.");
    }

    bool Installed() { return g_installed.load(std::memory_order_acquire); }

    const char* NotInstalledReason() { return g_reason.load(); }

    bool Enqueue(Handle a_handle, RE::FormID a_actor, const APMF_API::APMF_Param* a_param,
                 apmf::sources::SourceId a_client) {
        if (!Installed()) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): placement not installed -- {}.", Hex(a_actor),
                         NotInstalledReason());
            return false;
        }
        if (!a_param) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): no param. kIntent_Place needs RequestEx with "
                         "param.form = the destination.", Hex(a_actor));
            return false;
        }
        const auto flags = static_cast<std::uint32_t>(a_param->ival);
        if (const std::uint32_t extra = flags & ~static_cast<std::uint32_t>(APMF_API::kPlace_All)) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): unknown PlaceFlags bit(s) 0x{}.", Hex(a_actor),
                         Hex(extra));
            return false;
        }
        if (a_actor == 0 || a_actor == 0x14) {
            spdlog::warn("[place] request REFUSED: actor 0x{} -- {}.", Hex(a_actor),
                         a_actor == 0 ? "actor 0" : "the player is never placed");
            return false;
        }
        if (a_param->form == 0) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): param.form (the destination) is 0.", Hex(a_actor));
            return false;
        }
        if (!std::isfinite(a_param->posX) || !std::isfinite(a_param->posY) || !std::isfinite(a_param->posZ)) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): param.pos is not a finite point.", Hex(a_actor));
            return false;
        }
        const bool setYaw = (flags & APMF_API::kPlace_SetYaw) != 0;
        if (setYaw && !std::isfinite(a_param->fval)) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): kPlace_SetYaw with a non-finite param.fval.",
                         Hex(a_actor));
            return false;
        }

        // NO FORM LOOKUP HERE: the caller's thread (the position cast's review R2-1 rule). Everything that
        // needs a form is decided in Run, on the game thread.
        const RE::NiPoint3 offset{ a_param->posX, a_param->posY, a_param->posZ };
        const RE::FormID   dest = a_param->form;
        const float        yaw  = setYaw ? a_param->fval : 0.0f;
        std::uint32_t gen  = 0;
        bool          room = false;
        {
            std::scoped_lock    lock(g_mx);
            gen                     = g_gen.load(std::memory_order_acquire);   // under g_mx: ResetAll bumps it there
            const std::uint64_t now = apmf::clock::MonotonicMs();
            room = MakeRoomLocked(now);
            if (room) {
                Rec r{};
                r.info.state     = APMF_API::kPlaceState_Queued;
                r.info.actor     = a_actor;
                r.info.dest      = dest;
                r.info.flags     = flags;
                r.info.yaw       = yaw;
                r.info.seq       = ++g_seq;
                r.changedMs      = now;
                g_recs[a_handle] = r;
            }
        }
        if (!room) {
            spdlog::warn("[place] request REFUSED (actor 0x{}): {} placements are already queued.", Hex(a_actor),
                         kMaxRecords);
            return false;
        }

        apmf::mainthread::Post([a_handle, a_actor, dest, offset, yaw, flags, a_client, gen] {
            Deliver(a_handle, a_actor, dest, offset, yaw, flags, a_client, gen);
        });
        spdlog::info("[place] {} queued: actor 0x{} -> dest 0x{} offset/point {:.0f},{:.0f},{:.0f}{} flags 0x{} "
                     "(client '{}').",
                     a_handle, Hex(a_actor), Hex(dest), offset.x, offset.y, offset.z,
                     setYaw ? std::format(" yaw {:.2f}", yaw) : std::string{}, Hex(flags),
                     a_client == apmf::sources::kNoSource ? "none" : apmf::sources::Name(a_client));
        return true;
    }

    std::uint32_t GetState(Handle a_handle, APMF_API::APMF_PlaceStateInfo* a_out) {
        APMF_API::APMF_PlaceStateInfo info{};
        info.size   = sizeof(info);
        info.state  = APMF_API::kPlaceState_Unknown;
        info.handle = a_handle;
        if (a_handle != APMF_API::kInvalidHandle) {
            const std::uint64_t now = apmf::clock::MonotonicMs();
            std::scoped_lock    lock(g_mx);
            if (const auto it = g_recs.find(a_handle); it != g_recs.end()) {
                const std::uint64_t age = now - it->second.changedMs;
                if (!(Terminal(it->second.info.state) && age > APMF_API::kPlaceStateGraceMs)) {
                    info               = it->second.info;
                    info.size          = sizeof(info);
                    info.handle        = a_handle;
                    info.msSinceChange = age > 0xFFFFFFFEull ? 0xFFFFFFFEu : static_cast<std::uint32_t>(age);
                }
            }
        }
        if (a_out && a_out->size >= APMF_API::kPlaceStateInfoV28Size) {
            const std::uint32_t callerSize = a_out->size;
            std::memcpy(a_out, &info, APMF_API::kPlaceStateInfoV28Size);
            a_out->size = callerSize;
        }
        return info.state;
    }

    void ResetAll(const char* a_why) {
        std::size_t n = 0;
        {
            std::scoped_lock lock(g_mx);
            g_gen.fetch_add(1, std::memory_order_acq_rel);   // under g_mx, so no record is filed under the old one
            n = g_recs.size();
            g_recs.clear();
        }
        if (n != 0) spdlog::info("[place] {} -- forgot {} placement record(s).", a_why, n);
    }

}
