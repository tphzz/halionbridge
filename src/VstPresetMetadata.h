#pragma once

#include "CliCommand.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace halionbridge::detail
{

enum class VstPresetMetadataAction
{
    exportCsv,
    applyCsv,
};

struct VstPresetMetadataOptions
{
    VstPresetMetadataAction action = VstPresetMetadataAction::exportCsv;
    std::filesystem::path inputDirectory;
    std::filesystem::path metadataCsv;
    std::optional<std::filesystem::path> outputDirectory;
    bool recursive = false;
    bool overwrite = false;
};

struct VstPresetMetadataOptionsParseResult
{
    std::optional<VstPresetMetadataOptions> options;
    CliParseErrorKind errorKind = CliParseErrorKind::none;
    std::vector<CliDiagnostic> diagnostics;
};

struct VstPresetMetadataResult
{
    int exitCode = 0;
    std::vector<CliDiagnostic> diagnostics;
};

struct VstPresetMetadataRecord
{
    std::string filenamePath;
    std::string targetPresetName;
    std::unordered_map<std::string, std::string> metadata;
};

VstPresetMetadataOptionsParseResult parseVstPresetMetadataOptionsDetailed(std::span<const std::string> args);
VstPresetMetadataResult runVstPresetMetadataCommand(const VstPresetMetadataOptions& options);

std::vector<std::string> vstPresetMetadataFieldNames();
std::vector<VstPresetMetadataRecord> parseVstPresetMetadataCsv(std::string_view text, std::vector<std::string>& errors);
std::string writeVstPresetMetadataCsv(std::span<const VstPresetMetadataRecord> records);

} // namespace halionbridge::detail
