#pragma once

#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <utility>

namespace halionbridge::log
{

enum class Level
{
    trace,
    debug,
    info,
    warn,
    error,
    off
};

struct ParsedLevel
{
    Level level = Level::info;
    bool valid = true;
};

ParsedLevel parseLevel(std::string_view text) noexcept;
std::string_view toString(Level level) noexcept;
void configure(Level level);
void configureFromEnvironment();
void flush();

template <typename... Args> void trace(spdlog::format_string_t<Args...> format, Args&&... args)
{
    spdlog::trace(format, std::forward<Args>(args)...);
    flush();
}

template <typename... Args> void debug(spdlog::format_string_t<Args...> format, Args&&... args)
{
    spdlog::debug(format, std::forward<Args>(args)...);
    flush();
}

template <typename... Args> void info(spdlog::format_string_t<Args...> format, Args&&... args)
{
    spdlog::info(format, std::forward<Args>(args)...);
    flush();
}

template <typename... Args> void warn(spdlog::format_string_t<Args...> format, Args&&... args)
{
    spdlog::warn(format, std::forward<Args>(args)...);
    flush();
}

template <typename... Args> void error(spdlog::format_string_t<Args...> format, Args&&... args)
{
    spdlog::error(format, std::forward<Args>(args)...);
    flush();
}

} // namespace halionbridge::log
