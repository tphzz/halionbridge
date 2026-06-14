#pragma once

#include "halionbridge_converters/Converter.h"

#include <filesystem>
#include <optional>

namespace halionbridge::converters::sfz
{

struct ConversionOptions
{
    std::filesystem::path sourceDirectory;
    std::filesystem::path outputDirectory;
    std::optional<std::string> name;
    const ConverterRunContext* context = nullptr;
    bool recursive = false;
    bool overwrite = false;
};

struct ConversionResult
{
    bool succeeded = false;
    std::filesystem::path buildFile;
    std::vector<std::filesystem::path> generatedLuaFiles;
    std::vector<Diagnostic> diagnostics;
    int sfzFilesConverted = 0;
    int regionsConverted = 0;
    int regionsSkipped = 0;
};

ConversionResult convertDirectory(const ConversionOptions& options);
void registerConverter(ConverterRegistry& registry);

} // namespace halionbridge::converters::sfz
