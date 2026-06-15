#include "CliCommand.h"

namespace halionbridge::detail
{

CliCommandKind classifyCliCommand(std::span<const std::string> args) noexcept
{
    if (args.empty())
        return CliCommandKind::help;

    const auto& command = args.front();
    if (command == "--help" || command == "-h")
        return CliCommandKind::help;
    if (command == "--version")
        return CliCommandKind::version;
    if (command == "build")
        return CliCommandKind::build;
    if (command == "init")
        return CliCommandKind::init;
    if (command == "convert")
        return CliCommandKind::convert;
    if (command == "remap-vstpresets")
        return CliCommandKind::remapVstPresets;
    if (command == "--halionbridge-build-worker")
        return CliCommandKind::buildWorker;
    if (command == "--halionbridge-scan-plugin")
        return CliCommandKind::scanPluginWorker;

    return CliCommandKind::unknown;
}

} // namespace halionbridge::detail
