#include "PCH.h"
#include "core/Log.h"

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

}
