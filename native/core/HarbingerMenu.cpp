#include "PCH.h"
#include "core/HarbingerMenu.h"
#include "core/Clock.h"
#include "core/Log.h"
#include "core/Sources.h"

#include <cstdarg>

// See core/HarbingerMenu.h. Every signature below is copied from SMF's consumer header
// (resources/SKSEMenuFramework.h @ dc91824: AddSectionItem ~l.149, igTextUnformatted l.4475,
// igTextColoredV l.4493, igSeparator l.4365, igSameLine l.4370, igSpacing l.4380, igSliderInt l.4744,
// igCheckbox l.4583, igCollapsingHeader_TreeNodeFlags l.4979, igBeginTooltip l.5108, igEndTooltip
// l.5113, igIsItemHovered l.5494, igBeginTable l.5204, igEndTable l.5209, igTableNextRow l.5214,
// igTableNextColumn l.5219, igTableSetupColumn l.5229, igTableHeadersRow l.5244, igPushID_Str l.4436,
// igPopID l.4455, igSetNextItemWidth l.4257), never written from memory.
//
// LAYOUT (marth 2026-10-11, field feedback "It looks very crowded"): one summary line of alerts at
// the top (expand it to read them), a "hide idle" checkbox (UI state only, never saved), then ONE
// collapsible header per facet (collapsed by default, open when that facet has an alert), each a
// table with the same fixed-width columns -- source, kind badge, hits, priority slider -- one compact
// row per source, idle sources greyed and sorted last, every long detail in the row's hover tooltip.
// The census rows stay available in a collapsed header at the bottom. Presentation only: the same
// registry reads, the same slider -> sources::EnqueueSetPriority path.

namespace apmf::harbingermenu {

    namespace {

        using namespace apmf::sources;

        struct ImVec2 { float x, y; };
        struct ImVec4 { float x, y, z, w; };
        using RenderFn = void(__stdcall*)();

        // ImGui flag values from the same header (l.571, 1065, 1106, 1107, 1142-1143).
        constexpr int kTableRowBg         = 1 << 6;
        constexpr int kTableBordersInnerH = 1 << 7;
        constexpr int kColWidthStretch    = 1 << 3;
        constexpr int kColWidthFixed      = 1 << 4;
        constexpr int kSliderAlwaysClamp  = 1 << 4;
        constexpr int kTreeDefaultOpen    = 1 << 5;

        struct Api {
            void (*AddSectionItem)(const char*, RenderFn)                       = nullptr;
            void (*TextUnformatted)(const char*, const char*)                   = nullptr;
            void (*TextColoredV)(const ImVec4, const char*, va_list)            = nullptr;
            void (*Separator)()                                                 = nullptr;
            void (*SameLine)(float, float)                                      = nullptr;
            void (*Spacing)()                                                   = nullptr;
            bool (*SliderInt)(const char*, int*, int, int, const char*, int)    = nullptr;
            bool (*Checkbox)(const char*, bool*)                                = nullptr;
            bool (*CollapsingHeader)(const char*, int)                          = nullptr;
            bool (*BeginTooltip)()                                              = nullptr;
            void (*EndTooltip)()                                                = nullptr;
            bool (*IsItemHovered)(int)                                          = nullptr;
            bool (*BeginTable)(const char*, int, int, const ImVec2, float)      = nullptr;
            void (*EndTable)()                                                  = nullptr;
            void (*TableNextRow)(int, float)                                    = nullptr;
            bool (*TableNextColumn)()                                           = nullptr;
            void (*TableSetupColumn)(const char*, int, float, unsigned int)     = nullptr;
            void (*TableHeadersRow)()                                           = nullptr;
            void (*PushID)(const char*)                                         = nullptr;
            void (*PopID)()                                                     = nullptr;
            void (*SetNextItemWidth)(float)                                     = nullptr;
        };
        Api g;

        constexpr ImVec4 kWarn  { 1.00f, 0.62f, 0.20f, 1.0f };
        constexpr ImVec4 kGrey  { 0.55f, 0.55f, 0.55f, 1.0f };
        constexpr ImVec4 kGood  { 0.45f, 0.85f, 0.45f, 1.0f };
        constexpr ImVec4 kHead  { 0.80f, 0.80f, 1.00f, 1.0f };
        constexpr ImVec4 kWhite { 1.00f, 1.00f, 1.00f, 1.0f };

        // Column widths (pixels), the same in every facet table so the sliders line up.
        constexpr float kKindWidth   = 95.0f;
        constexpr float kHitsWidth   = 70.0f;
        constexpr float kSliderWidth = 190.0f;

        // UI state only: never written to APMF_Sources.ini (SMF has no per-plugin UI-state store).
        bool g_hideIdle = false;

        // Facets in display order, with the header label.
        struct FacetRow { Facet f; const char* label; };
        constexpr FacetRow kFacets[] = {
            { Facet::kTarget,       "Targets" },
            { Facet::kPackage,      "Packages" },
            { Facet::kEquip,        "Equip" },
            { Facet::kDetection,    "Detection" },
            { Facet::kCast,         "Casting" },
            { Facet::kCombatAction, "Combat actions" },
            { Facet::kCombat,       "Combat entry and approach" },
            { Facet::kActorValue,   "Actor values (speed, disposition)" },
            { Facet::kCore,         "Core" },
        };

        void Text(const std::string& s) { g.TextUnformatted(s.c_str(), nullptr); }
        void Colored(const ImVec4& c, const char* fmt, ...) {
            va_list a;
            va_start(a, fmt);
            g.TextColoredV(c, fmt, a);
            va_end(a);
        }
        void Colored(const ImVec4& c, const std::string& s) { Colored(c, "%s", s.c_str()); }

        std::string Age(std::uint64_t lastMs) {
            if (lastMs == 0) return "never";
            const auto d = (apmf::clock::MonotonicMs() - lastMs) / 1000;
            if (d < 60) return fmt::format("{} s ago", d);
            if (d < 3600) return fmt::format("{} min ago", d / 60);
            return fmt::format("{} h ago", d / 3600);
        }

        // The short kind badge.
        std::string Badge(SourceId id) {
            const Kind k = KindOf(id);
            if (IsClient(id) && k != Kind::kClient) return fmt::format("{}+client", KindName(k));
            if (k == Kind::kDirect) return "direct";
            return KindName(k);
        }

        // The facet's current winner: the highest-priority involved, non-idle source with a
        // non-zero priority (ties keep the first registered). kNoSource when none.
        SourceId Winner(Facet f) {
            SourceId best = kNoSource;
            int      bp   = 0;
            const auto n  = Count();
            for (std::uint32_t i = 0; i < n; ++i) {
                const SourceId id = static_cast<SourceId>(i);
                if (id == kUnattributedSource || !Involved(id, f) || Idle(id)) continue;
                const int p = Priority(id, f);
                if (p > bp) { bp = p; best = id; }
            }
            return best;
        }

        void Slider(SourceId id, Facet f) {
            int v = Priority(id, f);
            g.PushID(fmt::format("{}#{}", static_cast<int>(f), id).c_str());
            g.SetNextItemWidth(-1.0f);
            if (g.SliderInt("##prio", &v, kPrioMin, kPrioMax, v == 0 ? "off" : "%d", kSliderAlwaysClamp))
                EnqueueSetPriority(id, f, v);
            g.PopID();
        }

        void Tooltip(SourceId id, Facet f, const std::vector<SeatView>& seats) {
            if (!g.IsItemHovered(0) || !g.BeginTooltip()) return;
            Colored(kHead, std::string(Name(id)));
            Text(fmt::format("Kind: {}", Badge(id)));
            Text(fmt::format("{}: {} hit(s), overruled {}, stood {}", FacetName(f), Hits(id, f), Overruled(id, f),
                             Stood(id, f)));
            Text(fmt::format("Last seen: {}", Age(LastSeenMs(id, f))));
            if (Idle(id)) Colored(kGrey, "Idle: not seen during the last 3 game loads. Its priority is kept.");
            for (const auto& s : seats) {
                if (s.prev == id) Text(fmt::format("Hooks inside Harbinger's seat {} (installed earlier)", s.seat));
                if (s.late == id) {
                    if (s.lostOutermost)
                        Colored(kWarn, fmt::format("Hooked over Harbinger's seat {}: Harbinger NOT outermost", s.seat));
                    else
                        Text(fmt::format("Hooked over Harbinger's seat {} (Harbinger re-hooked on top x{})", s.seat,
                                         s.rehooks));
                }
            }
            if (id == kUnattributedSource) Colored(kWarn, "Writes Harbinger could not name. Always allowed, never denied.");
            g.EndTooltip();
        }

        void FacetGroup(const FacetRow& fr, const std::vector<Alert>& alerts, const std::vector<SeatView>& seats) {
            const Facet f = fr.f;
            // Rows: involved sources, active first, idle last (registration order inside each).
            std::vector<SourceId> active, idle;
            const auto n = Count();
            for (std::uint32_t i = 0; i < n; ++i) {
                const SourceId id = static_cast<SourceId>(i);
                if (!Involved(id, f)) continue;
                (Idle(id) ? idle : active).push_back(id);
            }
            if (active.empty() && idle.empty()) return;
            std::size_t facetAlerts = 0;
            for (const auto& a : alerts) facetAlerts += (a.facet == f) ? 1 : 0;
            const SourceId win = Winner(f);
            const std::string label =
                fmt::format("{}  -  {}  -  {} source(s){}###facet{}", fr.label,
                            win != kNoSource ? fmt::format("winner {} ({})", Name(win), Priority(win, f))
                                             : std::string("no active source"),
                            active.size() + idle.size(),
                            facetAlerts ? fmt::format("  -  {} alert(s)", facetAlerts) : std::string(),
                            static_cast<int>(f));
            if (!g.CollapsingHeader(label.c_str(), facetAlerts ? kTreeDefaultOpen : 0)) return;
            {
                bool ovr = OverrideUnknown(f);
                g.PushID(fmt::format("unk{}", static_cast<int>(f)).c_str());
                if (g.Checkbox("Unknown sources can be overridden", &ovr)) EnqueueSetOverrideUnknown(f, ovr);
                g.PopID();
            }
            if (!g.BeginTable(fmt::format("tbl{}", static_cast<int>(f)).c_str(), 4, kTableRowBg | kTableBordersInnerH,
                              ImVec2{ 0.0f, 0.0f }, 0.0f))
                return;
            g.TableSetupColumn("Source", kColWidthStretch, 0.0f, 0);
            g.TableSetupColumn("Kind", kColWidthFixed, kKindWidth, 0);
            g.TableSetupColumn("Hits", kColWidthFixed, kHitsWidth, 0);
            g.TableSetupColumn("Priority (0 = off)", kColWidthFixed, kSliderWidth, 0);
            g.TableHeadersRow();
            const auto row = [&](SourceId id, bool isIdle) {
                g.TableNextRow(0, 0.0f);
                g.TableNextColumn();
                const ImVec4& c = isIdle ? kGrey : id == kUnattributedSource ? kWarn : id == win ? kGood : kWhite;
                Colored(c, std::string(Name(id)));
                Tooltip(id, f, seats);
                g.TableNextColumn();
                Colored(isIdle ? kGrey : kWhite, Badge(id));
                g.TableNextColumn();
                Colored(isIdle ? kGrey : kWhite, std::to_string(Hits(id, f)));
                g.TableNextColumn();
                if (id == kUnattributedSource) Colored(kWarn, "always allowed");
                else {
                    Slider(id, f);
                    Tooltip(id, f, seats);   // gamepad: the focused slider shows the details (review SEV-3)
                }
            };
            for (const auto id : active) row(id, false);
            if (!g_hideIdle)
                for (const auto id : idle) row(id, true);
            g.EndTable();
            if (g_hideIdle && !idle.empty()) Colored(kGrey, fmt::format("{} idle source(s) hidden", idle.size()));
            g.Spacing();
        }

        void AlertsLine(const std::vector<Alert>& alerts) {
            if (alerts.empty()) {
                Colored(kGood, "No alerts: every write was attributed.");
                return;
            }
            const std::string label = fmt::format(
                "{} alert(s): writes Harbinger could not attribute (allowed, never denied)###alerts", AlertCount());
            if (!g.CollapsingHeader(label.c_str(), 0)) return;
            for (const auto& a : alerts)
                Colored(kWarn, fmt::format("{}  {}  actor 0x{}  {}", Age(a.ms), FacetName(a.facet),
                                           apmf::log::Hex(a.actor), a.detail));
            g.Spacing();
        }

        void CensusDetails(const std::vector<SeatView>& seats) {
            const std::string label = fmt::format("Seat census: {} seat(s), {} involve another mod###census",
                                                  SeatCount(), seats.size());
            if (!g.CollapsingHeader(label.c_str(), 0) || seats.empty()) return;
            for (const auto& s : seats) {
                const std::string inside = s.prev != kNoSource ? std::string(Name(s.prev)) : std::string("-");
                const std::string top    = s.late != kNoSource ? std::string(Name(s.late)) : std::string("-");
                const std::string line   = fmt::format("{} ({}): inside {}, on top {}{}", s.seat, FacetName(s.facet),
                                                       inside, top,
                                                       s.rehooks ? fmt::format(", re-hooked x{}", s.rehooks) : std::string());
                if (s.lostOutermost) Colored(kWarn, line + " -- Harbinger NOT outermost");
                else Text(line);
            }
        }

        // 1b: the SKSE plugins loaded in the process that have not been seen writing any facet yet
        // (Sources::RegisterLoadedPlugins). A plugin leaves this list on its first write.
        void LoadedPlugins() {
            std::vector<SourceId> quiet;
            const auto            n = Count();
            for (std::uint32_t i = 0; i < n; ++i)
                if (KindOf(static_cast<SourceId>(i)) == Kind::kLoaded) quiet.push_back(static_cast<SourceId>(i));
            const std::string label =
                fmt::format("Loaded SKSE plugins not seen writing yet: {}###loaded", quiet.size());
            if (!g.CollapsingHeader(label.c_str(), 0) || quiet.empty()) return;
            Colored(kGrey, "Read from the process at startup. A plugin moves to a facet above when Harbinger first "
                           "sees it write that facet.");
            for (const auto id : quiet) Text(std::string(Name(id)));
        }

        void __stdcall Render() {
            try {
                Colored(kHead, "Harbinger -- who drives each AI facet, and who wins.");
                Colored(kGrey, "A claim beats another mod's change only with a higher priority. 0 = off. Saved to "
                               "APMF_Sources.ini. Hover a source for details.");
                g.Spacing();
                const auto alerts = RecentAlerts();
                const auto seats  = SeatSnapshot();
                AlertsLine(alerts);
                g.Checkbox("Hide idle sources", &g_hideIdle);
                g.Separator();
                for (const auto& fr : kFacets) FacetGroup(fr, alerts, *seats);
                g.Separator();
                CensusDetails(*seats);
                LoadedPlugins();
            } catch (...) {
            }
        }

        template <class T>
        bool Resolve(REX::W32::HMODULE m, T& out, const char* name, std::string& missing) {
            out = reinterpret_cast<T>(REX::W32::GetProcAddress(m, name));
            if (!out) missing += fmt::format("{}{}", missing.empty() ? "" : ", ", name);
            return out != nullptr;
        }

    }

    void Register() {
        static std::atomic<bool> s_once{ false };
        if (s_once.exchange(true)) return;
        auto* m = REX::W32::GetModuleHandleA("SKSEMenuFramework.dll");
        if (!m) {
            spdlog::warn("[harbinger-menu] SKSE Menu Framework is NOT loaded (absent, or no build for this runtime) -- "
                         "the Harbinger page is unavailable. Harbinger runs from Data/SKSE/Plugins/APMF_Sources.ini: "
                         "edit [<Facet>] <Source>=0..100 there (0 = off).");
            return;
        }
        std::string missing;
        Resolve(m, g.AddSectionItem, "AddSectionItem", missing);
        Resolve(m, g.TextUnformatted, "igTextUnformatted", missing);
        Resolve(m, g.TextColoredV, "igTextColoredV", missing);
        Resolve(m, g.Separator, "igSeparator", missing);
        Resolve(m, g.SameLine, "igSameLine", missing);
        Resolve(m, g.Spacing, "igSpacing", missing);
        Resolve(m, g.SliderInt, "igSliderInt", missing);
        Resolve(m, g.Checkbox, "igCheckbox", missing);
        Resolve(m, g.CollapsingHeader, "igCollapsingHeader_TreeNodeFlags", missing);
        Resolve(m, g.BeginTooltip, "igBeginTooltip", missing);
        Resolve(m, g.EndTooltip, "igEndTooltip", missing);
        Resolve(m, g.IsItemHovered, "igIsItemHovered", missing);
        Resolve(m, g.BeginTable, "igBeginTable", missing);
        Resolve(m, g.EndTable, "igEndTable", missing);
        Resolve(m, g.TableNextRow, "igTableNextRow", missing);
        Resolve(m, g.TableNextColumn, "igTableNextColumn", missing);
        Resolve(m, g.TableSetupColumn, "igTableSetupColumn", missing);
        Resolve(m, g.TableHeadersRow, "igTableHeadersRow", missing);
        Resolve(m, g.PushID, "igPushID_Str", missing);
        Resolve(m, g.PopID, "igPopID", missing);
        Resolve(m, g.SetNextItemWidth, "igSetNextItemWidth", missing);
        if (!missing.empty()) {
            g = Api{};
            spdlog::error("[harbinger-menu] SKSE Menu Framework is loaded but does NOT export: {} -- the Harbinger page "
                          "is NOT registered (an unsupported SMF build). Harbinger runs from "
                          "Data/SKSE/Plugins/APMF_Sources.ini.",
                          missing);
            return;
        }
        g.AddSectionItem("Harbinger/Sources", &Render);
        spdlog::info("[harbinger-menu] the Harbinger page is registered on SKSE Menu Framework (Harbinger/Sources).");
    }

}
