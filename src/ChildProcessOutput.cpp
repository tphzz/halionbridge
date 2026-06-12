#include "ChildProcessOutput.h"

#include "Log.h"

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

} // namespace halionbridge::detail
