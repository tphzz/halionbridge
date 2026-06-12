#include "Log.h"

#include <juce_core/juce_core.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/sink.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

namespace halionbridge::log
{
namespace
{

constexpr const char* kEnvironmentVariable = "HALIONBRIDGE_LOGLEVEL";
constexpr const char* kPattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";

class LevelRangeSink final : public spdlog::sinks::sink
{
  public:
    LevelRangeSink(spdlog::sink_ptr innerSinkIn, spdlog::level::level_enum minimumLevelIn, spdlog::level::level_enum maximumLevelIn)
        : innerSink(std::move(innerSinkIn)), minimumLevel(minimumLevelIn), maximumLevel(maximumLevelIn)
    {
    }

    void log(const spdlog::details::log_msg& message) override
    {
        if (message.level >= minimumLevel && message.level <= maximumLevel)
            innerSink->log(message);
    }

    void flush() override
    {
        innerSink->flush();
    }

    void set_pattern(const std::string& pattern) override
    {
        innerSink->set_pattern(pattern);
    }

    void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override
    {
        innerSink->set_formatter(std::move(formatter));
    }

  private:
    spdlog::sink_ptr innerSink;
    spdlog::level::level_enum minimumLevel;
    spdlog::level::level_enum maximumLevel;
};

spdlog::level::level_enum toSpdlogLevel(Level level) noexcept
{
    switch (level)
    {
    case Level::trace:
        return spdlog::level::trace;
    case Level::debug:
        return spdlog::level::debug;
    case Level::info:
        return spdlog::level::info;
    case Level::warn:
        return spdlog::level::warn;
    case Level::error:
        return spdlog::level::err;
    case Level::off:
        return spdlog::level::off;
    }

    return spdlog::level::info;
}

std::string normalized(std::string_view text)
{
    auto value = std::string(text);
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

} // namespace

ParsedLevel parseLevel(std::string_view text) noexcept
{
    const auto value = normalized(text);

    if (value.empty() || value == "info")
        return {Level::info, true};

    if (value == "trace")
        return {Level::trace, true};

    if (value == "debug")
        return {Level::debug, true};

    if (value == "warn" || value == "warning")
        return {Level::warn, true};

    if (value == "error" || value == "err")
        return {Level::error, true};

    if (value == "off" || value == "none" || value == "silent")
        return {Level::off, true};

    return {Level::info, false};
}

std::string_view toString(Level level) noexcept
{
    switch (level)
    {
    case Level::trace:
        return "trace";
    case Level::debug:
        return "debug";
    case Level::info:
        return "info";
    case Level::warn:
        return "warn";
    case Level::error:
        return "error";
    case Level::off:
        return "off";
    }

    return "info";
}

void configureFromEnvironment()
{
    auto level = ParsedLevel{};
    constexpr const char* kUnsetSentinel = "\x1fhalionbridge_LOGLEVEL_UNSET\x1f";
    const auto raw = juce::SystemStats::getEnvironmentVariable(kEnvironmentVariable, kUnsetSentinel);

    if (raw != kUnsetSentinel)
        level = parseLevel(raw.toStdString());

    configure(level.level);

    if (!level.valid)
    {
        warn("Invalid {} value. Falling back to info. Supported values: trace, debug, info, warn, error, off.", kEnvironmentVariable);
    }
}

void configure(Level level)
{
    auto stdoutSink = std::make_shared<LevelRangeSink>(std::make_shared<spdlog::sinks::stdout_color_sink_mt>(), spdlog::level::trace,
                                                       spdlog::level::info);
    stdoutSink->set_level(spdlog::level::trace);
    stdoutSink->set_pattern(kPattern);

    auto stderrSink = std::make_shared<LevelRangeSink>(std::make_shared<spdlog::sinks::stderr_color_sink_mt>(), spdlog::level::warn,
                                                       spdlog::level::critical);
    stderrSink->set_level(spdlog::level::warn);
    stderrSink->set_pattern(kPattern);

    std::vector<spdlog::sink_ptr> sinks{stdoutSink, stderrSink};
    auto logger = std::make_shared<spdlog::logger>("halionbridge", sinks.begin(), sinks.end());
    const auto configuredLevel = toSpdlogLevel(level);
    logger->set_level(configuredLevel);
    logger->flush_on(configuredLevel == spdlog::level::off ? spdlog::level::off : configuredLevel);
    spdlog::set_default_logger(std::move(logger));
    spdlog::set_level(configuredLevel);
}

} // namespace halionbridge::log
