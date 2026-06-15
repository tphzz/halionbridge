#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <juce_core/juce_core.h>

namespace halionbridge::detail
{

class ChildProcessOutputBuffer
{
  public:
    std::vector<std::string> append(std::string_view bytes);
    std::optional<std::string> flush();

  private:
    std::string pendingBytes;
};

void forwardChildOutput(juce::ChildProcess& process, ChildProcessOutputBuffer& output);
void flushChildOutput(ChildProcessOutputBuffer& output);
void forwardAvailableChildOutputToConsole(juce::ChildProcess& process, ChildProcessOutputBuffer& output);
void forwardChildOutputToConsole(juce::ChildProcess& process, ChildProcessOutputBuffer& output);
void flushChildOutputToConsole(ChildProcessOutputBuffer& output);

} // namespace halionbridge::detail
