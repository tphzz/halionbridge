#include "ChildProcessOutput.h"

#include "Log.h"

#include <optional>

namespace halionbridge::detail
{
namespace
{

std::string decodeUtf8Line(std::string_view bytes)
{
    return juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())).toStdString();
}

void logLine(const std::string& line)
{
    if (!line.empty())
        log::debug("{}", line);
}

struct ParsedLogLine
{
    log::Level level = log::Level::info;
    std::string message;
};

std::optional<ParsedLogLine> parseChildLogLine(const std::string& line)
{
    if (line.empty() || line.front() != '[')
        return std::nullopt;

    const auto timestampEnd = line.find("] [");
    if (timestampEnd == std::string::npos)
        return std::nullopt;

    const auto levelStart = timestampEnd + 3;
    const auto levelEnd = line.find("] ", levelStart);
    if (levelEnd == std::string::npos)
        return std::nullopt;

    const auto parsedLevel = log::parseLevel(std::string_view(line).substr(levelStart, levelEnd - levelStart));
    if (!parsedLevel.valid)
        return std::nullopt;

    return ParsedLogLine{parsedLevel.level, line.substr(levelEnd + 2)};
}

void forwardConsoleLine(const std::string& line)
{
    if (line.empty())
        return;

    const auto parsed = parseChildLogLine(line);
    const auto level = parsed ? parsed->level : log::Level::info;
    const auto& message = parsed ? parsed->message : line;

    switch (level)
    {
    case log::Level::trace:
        log::trace("{}", message);
        break;
    case log::Level::debug:
        log::debug("{}", message);
        break;
    case log::Level::info:
        log::info("{}", message);
        break;
    case log::Level::warn:
        log::warn("{}", message);
        break;
    case log::Level::error:
        log::error("{}", message);
        break;
    case log::Level::off:
        break;
    }
}

bool forwardChildOutputToConsole(juce::ChildProcess& process, ChildProcessOutputBuffer& output, const int bufferSize)
{
    std::vector<char> buffer(static_cast<size_t>(bufferSize));
    const auto bytesRead = process.readProcessOutput(buffer.data(), static_cast<int>(buffer.size()));
    if (bytesRead <= 0)
        return false;

    for (const auto& line : output.append(std::string_view(buffer.data(), static_cast<size_t>(bytesRead))))
        forwardConsoleLine(line);

    return true;
}

} // namespace

std::vector<std::string> ChildProcessOutputBuffer::append(std::string_view bytes)
{
    pendingBytes.append(bytes.data(), bytes.size());

    auto lines = std::vector<std::string>();
    for (;;)
    {
        const auto newline = pendingBytes.find('\n');
        if (newline == std::string::npos)
            break;

        auto lineBytes = pendingBytes.substr(0, newline);
        pendingBytes.erase(0, newline + 1);

        if (!lineBytes.empty() && lineBytes.back() == '\r')
            lineBytes.pop_back();

        lines.push_back(decodeUtf8Line(lineBytes));
    }

    return lines;
}

std::optional<std::string> ChildProcessOutputBuffer::flush()
{
    if (pendingBytes.empty())
        return std::nullopt;

    if (!pendingBytes.empty() && pendingBytes.back() == '\r')
        pendingBytes.pop_back();

    auto line = decodeUtf8Line(pendingBytes);
    pendingBytes.clear();
    return line;
}

void forwardChildOutput(juce::ChildProcess& process, ChildProcessOutputBuffer& output)
{
    char buffer[1024]{};

    for (;;)
    {
        const auto bytesRead = process.readProcessOutput(buffer, static_cast<int>(sizeof(buffer)));
        if (bytesRead <= 0)
            break;

        for (const auto& line : output.append(std::string_view(buffer, static_cast<size_t>(bytesRead))))
            logLine(line);
    }
}

void flushChildOutput(ChildProcessOutputBuffer& output)
{
    if (auto line = output.flush())
        logLine(*line);
}

void forwardChildOutputToConsole(juce::ChildProcess& process, ChildProcessOutputBuffer& output)
{
    while (process.isRunning())
        forwardChildOutputToConsole(process, output, 1);

    while (forwardChildOutputToConsole(process, output, 1024))
    {
    }
}

void flushChildOutputToConsole(ChildProcessOutputBuffer& output)
{
    if (auto line = output.flush())
        forwardConsoleLine(*line);
}

} // namespace halionbridge::detail
