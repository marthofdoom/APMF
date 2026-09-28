#include "PCH.h"
#include "core/Log.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace apmf::log {

    namespace {
        // LOG ROTATION (2026-09-28, mirrors MFO's plugin.cpp RotateLog): keep five
        // generations, APMF.log -> .1 -> .2 -> .3 -> .4 -> .5, the oldest dropped.
        // The file used to be truncated at every launch, so a crash or freeze
        // session's log was lost to the relaunch that followed it. Runs BEFORE the
        // sink truncates the live file, on the thread that opens it, and with the
        // same path resolution as the sink (rotate wherever it is about to open).
        // Oldest first: .4 -> .5 replaces .5 (std::filesystem::rename replaces an
        // existing target on Windows, MoveFileEx REPLACE_EXISTING, and USVFS
        // redirects it like the write), then .3 -> .4, ..., then the live file -> .1.
        // A failed step is recorded and skipped, never fatal: the sink still opens.
        // `live` is 1 rotated, 0 nothing to rotate, -1 the live rename failed.
        // Failures are reported after the logger is up, so no line is lost.
        constexpr int kLogGenerations = 5;
        struct LogRotation {
            int                      live = 0;
            std::vector<std::string> failed;
        };
        LogRotation RotateLog(const std::filesystem::path& a_live) {
            LogRotation     r;
            std::error_code ec;
            auto gen = [&](int n) {
                std::filesystem::path p = a_live;
                p += "." + std::to_string(n);
                return p;
            };
            for (int n = kLogGenerations - 1; n >= 1; --n) {
                const auto from = gen(n);
                ec.clear();
                if (!std::filesystem::exists(from, ec) || ec) continue;
                std::filesystem::rename(from, gen(n + 1), ec);
                if (ec) r.failed.push_back(from.filename().string() + ": " + ec.message());
            }
            ec.clear();
            if (!std::filesystem::exists(a_live, ec) || ec) return r;
            std::filesystem::rename(a_live, gen(1), ec);
            r.live = ec ? -1 : 1;
            return r;
        }
    }

    void Setup() {
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> sink;
        LogRotation           rotated;
        std::filesystem::path logPath;
        try {
            logPath = "Data/SKSE/Plugins/APMF.log";
            rotated = RotateLog(logPath);
            sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        } catch (const spdlog::spdlog_ex&) {
            if (auto dir = SKSE::log::log_directory()) {
                try {
                    logPath = *dir / "APMF.log";
                    rotated = RotateLog(logPath);
                    sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
                } catch (const spdlog::spdlog_ex&) { return; }
            } else { return; }
        }
        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);   // flush every line so a CTD keeps the trail
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");

        for (const auto& f : rotated.failed)
            spdlog::warn("[startup] could not shift an older APMF.log generation ({}) -- skipped", f);
        if (rotated.live > 0)
            spdlog::info("[startup] rotated the previous APMF.log to APMF.log.1, keeping {} generations ({})",
                         kLogGenerations, logPath.string());
        else if (rotated.live < 0)
            spdlog::warn("[startup] could not rotate the previous APMF.log to APMF.log.1 ({}) -- it was truncated",
                         logPath.string());
    }

    std::string Hex(std::uint64_t value, int width) {
        static constexpr char kDigits[] = "0123456789ABCDEF";
        char buf[16];
        int  pos = 16;
        do { buf[--pos] = kDigits[value & 0xF]; value >>= 4; } while (value && pos > 0);
        while ((16 - pos) < width && pos > 0) buf[--pos] = '0';
        return std::string(buf + pos, buf + 16);
    }

}
