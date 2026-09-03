#pragma once

#include "halionbridge_converters/Converter.h"

#include <filesystem>
#include <vector>

namespace halionbridge::converters::dx7
{

struct ConversionOptions
{
    std::filesystem::path sourcePath;
    std::filesystem::path outputDirectory;
    PresetOutputType presetOutputType = PresetOutputType::program;
    PresetTarget presetTarget = PresetTarget::halion;
    const ConverterRunContext* context = nullptr;
    bool recursive = false;
    bool overwrite = false;
    bool strictParameters = false;
    bool continueOnError = false;
};

struct ConversionResult
{
    bool succeeded = false;
    std::filesystem::path buildFile;
    std::vector<std::filesystem::path> generatedLuaFiles;
    std::vector<std::filesystem::path> generatedFiles;
    std::vector<Diagnostic> diagnostics;
    int syxFilesScanned = 0;
    int syxFilesConverted = 0;
    int syxFilesPartial = 0;
    int syxFilesSkipped = 0;
    int messagesConverted = 0;
    int bankMessagesConverted = 0;
    int singleMessagesConverted = 0;
    int messagesSkipped = 0;
    int fragmentsSkipped = 0;
    int voicesConverted = 0;
    int normalizationsApplied = 0;
    int voicesNormalized = 0;
};

ConversionResult convertSource(const ConversionOptions& options);
void registerConverter(ConverterRegistry& registry);

} // namespace halionbridge::converters::dx7
