#include "PCH.h"
#include "core/HarbingerMenu.h"
#include "core/Clock.h"
#include "core/Log.h"
#include "core/Sources.h"

#include <cstdarg>

// See core/HarbingerMenu.h. Every signature below is copied from SMF's consumer header
// (resources/SKSEMenuFramework.h @ dc91824: AddSectionItem ~l.149, igTextUnformatted l.4475,
// igTextColoredV l.4493, igSeparator l.4365, igSameLine l.4370, igSliderInt l.4744,
// igBeginTable l.5204, igEndTable l.5209, igTableNextRow l.5214, igTableNextColumn l.5219,
// igTableSetupColumn l.5229, igTableHeadersRow l.5244, igBeginTabBar l.5334, igEndTabBar
// l.5339, igBeginTabItem l.5344, igEndTabItem l.5349, igPushID_Str l.4436, igPopID l.4455,
// igSetNextItemWidth l.4257), never written from memory.

namespace apmf::harbingermenu {

    namespace {

        using namespace apmf::sources;

        struct ImVec2 { float x, y; };
        struct ImVec4 { float x, y, z, w; };
        using RenderFn = void(__stdcall*)();

        // ImGui flag values from the same header (l.1100-1125, 627, 1065, 1142-1143).
        constexpr int kTableRowBg        = 1 << 6;
        constexpr int kTableBorders      = (1 << 7) | (1 << 8) | (1 << 9) | (1 << 10);
        constexpr int kTableSizingStretch = 3 << 13;
        constexpr int kTabBarScroll      = 1 << 7;
        constexpr int kSliderAlwaysClamp = 1 << 4;

        struct Api {
            void (*AddSectionItem)(const char*, RenderFn)                       = nullptr;
            void (*TextUnformatted)(const char*, const char*)                   = nullptr;
            void (*TextColoredV)(const ImVec4, const char*, va_list)            = nullptr;
            void (*Separator)()                                                 = nullptr;
            void (*SameLine)(float, float)                                      = nullptr;
            bool (*SliderInt)(const char*, int*, int, int, const char*, int)    = nullptr;
            bool (*BeginTable)(const char*, int, int, const ImVec2, float)      = nullptr;
            void (*EndTable)()                                                  = nullptr;
            void (*TableNextRow)(int, float)                                    = nullptr;
            bool (*TableNextColumn)()                                           = nullptr;
            void (*TableSetupColumn)(const char*, int, float, unsigned int)     = nullptr;
            void (*TableHeadersRow)()                                           = nullptr;
            bool (*BeginTabBar)(const char*, int)                               = nullptr;
            void (*EndTabBar)()                                                 = nullptr;
            bool (*BeginTabItem)(const char*, bool*, int)                       = nullptr;
            void (*EndTabItem)()                                                = nullptr;
            void (*PushID)(const char*)                                         = nullptr;
            void (*PopID)()                                                     = nullptr;
            void (*SetNextItemWidth)(float)                                     = nullptr;
        };
        Api g;

        constexpr ImVec4 kWarn  { 1.00f, 0.62f, 0.20f, 1.0f };
        constexpr ImVec4 kGrey  { 0.55f, 0.55f, 0.55f, 1.0f };
        constexpr ImVec4 kGood  { 0.45f, 0.85f, 0.45f, 1.0f };
        constexpr ImVec4 kHead  { 0.80f, 0.80f, 1.00f, 1.0f };

        // The 1a facets get the master table; the others appear on source tabs once involved.
        constexpr Facet kMasterFacets[] = { Facet::kTarget, Facet::kDetection, Facet::kPackage, Facet::kEquip };

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
            if (d < 60) return fmt::format("{} s", d);
            if (d < 3600) return fmt::format("{} min", d / 60);
            return fmt::format("{} h", d / 3600);
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

        void FacetsTab() {
            for (const Facet f : kMasterFacets) {
                const SourceId win = Winner(f);
                Colored(kHead, "%s", FacetName(f));
                g.SameLine(0.0f, -1.0f);
                Text(win != kNoSource ? fmt::format("-- winner: {} ({})", Name(win), Priority(win, f))
                                      : std::string("-- no source seen yet"));
                if (!g.BeginTable(fmt::format("tbl{}", static_cast<int>(f)).c_str(), 6,
                                  kTableRowBg | kTableBorders | kTableSizingStretch, ImVec2{ 0.0f, 0.0f }, 0.0f))
                    continue;
                g.TableSetupColumn("Source", 0, 0.0f, 0);
                g.TableSetupColumn("Kind", 0, 0.0f, 0);
                g.TableSetupColumn("Hits", 0, 0.0f, 0);
                g.TableSetupColumn("Overruled / stood", 0, 0.0f, 0);
                g.TableSetupColumn("Seen", 0, 0.0f, 0);
                g.TableSetupColumn("Priority (0 = off)", 0, 0.0f, 0);
                g.TableHeadersRow();
                const auto n = Count();
                for (std::uint32_t i = 0; i < n; ++i) {
                    const SourceId id = static_cast<SourceId>(i);
                    if (!Involved(id, f)) continue;
                    const bool idle = Idle(id);
                    g.TableNextRow(0, 0.0f);
                    g.TableNextColumn();
                    Colored(idle ? kGrey : (id == kUnattributedSource ? kWarn : (id == win ? kGood : ImVec4{ 1, 1, 1, 1 })),
                            fmt::format("{}{}", Name(id), idle ? " (idle)" : ""));
                    g.TableNextColumn();
                    Text(KindName(KindOf(id)));
                    g.TableNextColumn();
                    Text(std::to_string(Hits(id, f)));
                    g.TableNextColumn();
                    Text(fmt::format("{} / {}", Overruled(id, f), Stood(id, f)));
                    g.TableNextColumn();
                    Text(Age(LastSeenMs(id, f)));
                    g.TableNextColumn();
                    if (id == kUnattributedSource) Colored(kWarn, "always allowed");
                    else Slider(id, f);
                }
                g.EndTable();
                g.Separator();
            }
        }

        void SourceTab(SourceId id) {
            Text(fmt::format("{} -- {}", Name(id), KindName(KindOf(id))));
            if (Idle(id)) Colored(kGrey, "Idle: not seen during the last 3 game loads. Its priorities are kept.");
            if (!g.BeginTable("src", 5, kTableRowBg | kTableBorders | kTableSizingStretch, ImVec2{ 0.0f, 0.0f }, 0.0f))
                return;
            g.TableSetupColumn("Facet", 0, 0.0f, 0);
            g.TableSetupColumn("Hits", 0, 0.0f, 0);
            g.TableSetupColumn("Overruled / stood", 0, 0.0f, 0);
            g.TableSetupColumn("Seen", 0, 0.0f, 0);
            g.TableSetupColumn("Priority (0 = off)", 0, 0.0f, 0);
            g.TableHeadersRow();
            for (int fi = 0; fi < kFacetCount; ++fi) {
                const Facet f = static_cast<Facet>(fi);
                if (!Involved(id, f)) continue;
                g.TableNextRow(0, 0.0f);
                g.TableNextColumn();
                Text(FacetName(f));
                g.TableNextColumn();
                Text(std::to_string(Hits(id, f)));
                g.TableNextColumn();
                Text(fmt::format("{} / {}", Overruled(id, f), Stood(id, f)));
                g.TableNextColumn();
                Text(Age(LastSeenMs(id, f)));
                g.TableNextColumn();
                if (id == kUnattributedSource) Colored(kWarn, "always allowed");
                else Slider(id, f);
            }
            g.EndTable();
        }

        void SeatsTab() {
            const auto snap = SeatSnapshot();
            Text(fmt::format("{} Harbinger seat(s) installed; {} involve another mod.", SeatCount(), snap->size()));
            if (snap->empty()) return;
            if (!g.BeginTable("seats", 4, kTableRowBg | kTableBorders | kTableSizingStretch, ImVec2{ 0.0f, 0.0f }, 0.0f))
                return;
            g.TableSetupColumn("Seat", 0, 0.0f, 0);
            g.TableSetupColumn("Facet", 0, 0.0f, 0);
            g.TableSetupColumn("Inside Harbinger (earlier)", 0, 0.0f, 0);
            g.TableSetupColumn("On top (later)", 0, 0.0f, 0);
            g.TableHeadersRow();
            for (const auto& s : *snap) {
                g.TableNextRow(0, 0.0f);
                g.TableNextColumn();
                Text(s.seat);
                g.TableNextColumn();
                Text(FacetName(s.facet));
                g.TableNextColumn();
                Text(s.prev != kNoSource ? std::string(Name(s.prev)) : std::string("-"));
                g.TableNextColumn();
                if (s.lostOutermost) Colored(kWarn, fmt::format("{} -- Harbinger NOT outermost", s.late != kNoSource ? Name(s.late) : "?"));
                else if (s.rehooks) Text(fmt::format("{} (Harbinger re-hooked on top x{})", s.late != kNoSource ? Name(s.late) : "?", s.rehooks));
                else Text("-");
            }
            g.EndTable();
        }

        void __stdcall Render() {
            try {
                Colored(kHead, "Harbinger -- who drives each AI facet, and who wins.");
                Text("A mod's claim beats an outside writer only when its priority is higher. 0 = off. "
                     "Changes apply at once and are saved to Data/SKSE/Plugins/APMF_Sources.ini.");
                const auto alerts = RecentAlerts();
                if (!alerts.empty()) {
                    g.Separator();
                    Colored(kWarn, "ALERT: %llu write(s) Harbinger could not attribute. They were ALLOWED, never denied.",
                            static_cast<unsigned long long>(AlertCount()));
                    for (std::size_t i = 0; i < alerts.size() && i < 8; ++i) {
                        const auto& a = alerts[i];
                        Colored(kWarn, fmt::format("  {} ago  {}  actor 0x{}  {}", Age(a.ms), FacetName(a.facet),
                                                   apmf::log::Hex(a.actor), a.detail));
                    }
                }
                g.Separator();
                if (!g.BeginTabBar("harbinger", kTabBarScroll)) return;
                if (g.BeginTabItem("Facets", nullptr, 0)) {
                    FacetsTab();
                    g.EndTabItem();
                }
                const auto n = Count();
                for (std::uint32_t i = 0; i < n; ++i) {
                    const SourceId id = static_cast<SourceId>(i);
                    bool any = false;
                    for (int fi = 0; fi < kFacetCount && !any; ++fi) any = Involved(id, static_cast<Facet>(fi));
                    if (!any) continue;
                    g.PushID(fmt::format("srctab{}", id).c_str());
                    if (g.BeginTabItem(std::string(Name(id)).c_str(), nullptr, 0)) {
                        SourceTab(id);
                        g.EndTabItem();
                    }
                    g.PopID();
                }
                if (g.BeginTabItem("Seats", nullptr, 0)) {
                    SeatsTab();
                    g.EndTabItem();
                }
                g.EndTabBar();
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
        Resolve(m, g.SliderInt, "igSliderInt", missing);
        Resolve(m, g.BeginTable, "igBeginTable", missing);
        Resolve(m, g.EndTable, "igEndTable", missing);
        Resolve(m, g.TableNextRow, "igTableNextRow", missing);
        Resolve(m, g.TableNextColumn, "igTableNextColumn", missing);
        Resolve(m, g.TableSetupColumn, "igTableSetupColumn", missing);
        Resolve(m, g.TableHeadersRow, "igTableHeadersRow", missing);
        Resolve(m, g.BeginTabBar, "igBeginTabBar", missing);
        Resolve(m, g.EndTabBar, "igEndTabBar", missing);
        Resolve(m, g.BeginTabItem, "igBeginTabItem", missing);
        Resolve(m, g.EndTabItem, "igEndTabItem", missing);
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
