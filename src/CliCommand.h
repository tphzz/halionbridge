#pragma once

#include "halionbridge/Bridge.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

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
    vstPresetMetadata,
    buildWorker,
    scanPluginWorker,
    unknown,
};

enum class CliParseErrorKind
{
    none,
    syntax,
    validation,
};

enum class CliDiagnosticLevel
{
    info,
    warning,
    error,
};

struct CliDiagnostic
{
    CliDiagnosticLevel level = CliDiagnosticLevel::error;
    std::string message;
};

struct BuildOptionsParseResult
{
    std::optional<AppOptions> options;
    CliParseErrorKind errorKind = CliParseErrorKind::none;
    std::vector<CliDiagnostic> diagnostics;
};

struct VstPresetRemapOptionsParseResult
{
    std::optional<VstPresetRemapOptions> options;
    CliParseErrorKind errorKind = CliParseErrorKind::none;
    std::vector<CliDiagnostic> diagnostics;
};

CliCommandKind classifyCliCommand(std::span<const std::string> args) noexcept;
BuildOptionsParseResult parseBuildOptionsDetailed(std::span<const std::string> args);
std::optional<AppOptions> parseBuildOptions(std::span<const std::string> args);
VstPresetRemapOptionsParseResult parseVstPresetRemapOptionsDetailed(std::span<const std::string> args);
std::optional<VstPresetRemapOptions> parseVstPresetRemapOptions(std::span<const std::string> args);
std::optional<AppOptions> parseBuildWorkerOptions(std::span<const std::string> args);

} // namespace halionbridge::detail
