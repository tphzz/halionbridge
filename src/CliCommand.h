#pragma once

#include <span>
#include <string>

namespace halionbridge::detail
{

enum class CliCommandKind
{
    help,
    version,
    build,
    init,
    convert,
    remapVstPresets,
    buildWorker,
    scanPluginWorker,
    unknown,
};

CliCommandKind classifyCliCommand(std::span<const std::string> args) noexcept;

} // namespace halionbridge::detail
