#include "PCH.h"
#include "core/Log.h"

#include <spdlog/fmt/fmt.h>

#include <cstring>

namespace apmf::log {

    void Setup() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        try {
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("Data/SKSE/Plugins/APMF.log", true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((*dir / "APMF.log").string(), true);
                } catch (const spdlog::spdlog_ex&) { return; }
            } else { return; }
        }
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);   // flush every line so a CTD keeps the trail
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
    }

    std::string Hex(std::uint64_t value, int width) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        char buf[16];
        int  pos = 16;
        do { buf[--pos] = kDigits[value & 0xF]; value >>= 4; } while (value && pos > 0);
        while ((16 - pos) < width && pos > 0) buf[--pos] = '0';
        return std::string(buf + pos, buf + 16);
    }

    // ------------------------------------------------------------------ probe

    namespace {

        // fmt's digit tables as they must read in a healthy image.
        constexpr std::string_view kUpperTbl = "0123456789ABCDEF";
        constexpr std::string_view kLowerTbl = "0123456789abcdef";

        // Cached absolute addresses of every .rdata copy of each table, found
        // on the first (presumed-healthy) scan. /GF string pooling may merge
        // our own reference literals with fmt's tables into one copy -- that
        // merged copy IS the one fmt reads, so caching it is exactly right.
        // A content scan cannot locate a table that is ALREADY corrupted; in
        // that case the sentinel's garbled bytes (item 1) are themselves the
        // table dump, and the "load"-phase failure is the verdict.
        constexpr std::size_t kMaxCopies = 4;
        const std::uint8_t*   g_upperTbl[kMaxCopies] = {};
        const std::uint8_t*   g_lowerTbl[kMaxCopies] = {};
        bool                  g_scanned  = false;
        std::uintptr_t        g_modBase  = 0;

        // Render an arbitrary byte string as spaced ASCII-hex pairs through the
        // known-clean manual path (never fmt hex -- INVARIANTS #16).
        std::string Bytes(const std::uint8_t* p, std::size_t n) {
            std::string out;
            for (std::size_t i = 0; i < n; ++i) {
                if (i) out += ' ';
                out += Hex(p[i], 2);
            }
            return out;
        }

        // Locate this DLL's image base: walk pages down from a function inside
        // it until the 'MZ' magic. Every page from .text down to the header is
        // mapped, so the walk stays inside the image.
        std::uintptr_t OwnModuleBase() {
            auto addr = reinterpret_cast<std::uintptr_t>(&Bytes) & ~std::uintptr_t{ 0xFFF };
            for (int i = 0; i < 4096; ++i, addr -= 0x1000) {
                if (*reinterpret_cast<const std::uint16_t*>(addr) == 0x5A4D) return addr;
            }
            return 0;
        }

        // Find .rdata in the in-memory PE and scan it for every copy of the
        // digit tables.
        void ScanOwnRdataForTables() {
            g_scanned = true;
            g_modBase = OwnModuleBase();
            if (!g_modBase) return;
            const auto* base    = reinterpret_cast<const std::uint8_t*>(g_modBase);
            const auto  peOff   = *reinterpret_cast<const std::uint32_t*>(base + 0x3C);
            const auto* pe      = base + peOff;
            const auto  nSec    = *reinterpret_cast<const std::uint16_t*>(pe + 6);
            const auto  optSize = *reinterpret_cast<const std::uint16_t*>(pe + 20);
            const auto* sec     = pe + 24 + optSize;
            for (std::uint16_t i = 0; i < nSec; ++i, sec += 40) {
                if (std::memcmp(sec, ".rdata\0\0", 8) != 0) continue;
                const auto vsize = *reinterpret_cast<const std::uint32_t*>(sec + 8);
                const auto va    = *reinterpret_cast<const std::uint32_t*>(sec + 12);
                const auto* beg  = base + va;
                std::size_t nUp = 0, nLo = 0;
                for (std::uint32_t off = 0; off + 16 <= vsize; ++off) {
                    if (nUp < kMaxCopies && std::memcmp(beg + off, kUpperTbl.data(), 16) == 0) {
                        g_upperTbl[nUp++] = beg + off;
                    }
                    if (nLo < kMaxCopies && std::memcmp(beg + off, kLowerTbl.data(), 16) == 0) {
                        g_lowerTbl[nLo++] = beg + off;
                    }
                }
                return;
            }
        }

        void ReportTable(const char* phase, const char* name,
                         const std::uint8_t* const* copies, std::string_view expect) {
            if (!copies[0]) {
                spdlog::warn("[hexprobe] {}: no {} table found in .rdata at first scan (already "
                             "corrupted before load, or fmt layout changed) -- rely on the "
                             "sentinel bytes above", phase, name);
                return;
            }
            for (std::size_t i = 0; i < kMaxCopies; ++i) {
                const auto* tbl = copies[i];
                if (!tbl) break;
                const auto rva = reinterpret_cast<std::uintptr_t>(tbl) - g_modBase;
                if (std::memcmp(tbl, expect.data(), 16) == 0) {
                    spdlog::info("[hexprobe] {}: {} digit table intact at base+0x{}", phase, name,
                                 Hex(rva, 0));
                } else {
                    spdlog::error("[hexprobe] {}: {} digit table CORRUPTED at base+0x{} "
                                  "(base 0x{}) -- bytes now [{}]",
                                  phase, name, Hex(rva, 0), Hex(g_modBase, 0), Bytes(tbl, 16));
                }
            }
        }

    }

    void HexSelfTest(const char* phase) {
        // 1. Sentinels through fmt's real formatting paths. {:016X} exercises
        //    every digit of the uppercase table, {:x} the lowercase one, {:.2f}
        //    the float writer, {} the decimal writer (control).
        const std::string up  = fmt::format("{:016X}", 0x0123456789ABCDEFull);
        const std::string lo  = fmt::format("{:x}", 0xDEADBEEFu);
        const std::string fl  = fmt::format("{:.2f}", 1234.5);
        const std::string dec = fmt::format("{}", 1234567890u);

        const bool pass = up == "0123456789ABCDEF" && lo == "deadbeef" &&
                          fl == "1234.50" && dec == "1234567890";
        if (pass) {
            spdlog::info("[hexprobe] {}: PASS ({{:016X}}/{{:x}}/{{:.2f}}/{{}} all clean)", phase);
        } else {
            spdlog::error("[hexprobe] {}: FAIL -- upper=[{}] lower=[{}] float=[{}] dec=[{}]",
                          phase,
                          Bytes(reinterpret_cast<const std::uint8_t*>(up.data()), up.size()),
                          Bytes(reinterpret_cast<const std::uint8_t*>(lo.data()), lo.size()),
                          Bytes(reinterpret_cast<const std::uint8_t*>(fl.data()), fl.size()),
                          Bytes(reinterpret_cast<const std::uint8_t*>(dec.data()), dec.size()));
        }

        // 2. Dump the actual in-image tables (addresses cached on first call).
        if (!g_scanned) ScanOwnRdataForTables();
        ReportTable(phase, "upper", g_upperTbl, kUpperTbl);
        ReportTable(phase, "lower", g_lowerTbl, kLowerTbl);
    }

}
