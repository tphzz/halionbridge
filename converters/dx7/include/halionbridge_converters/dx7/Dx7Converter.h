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
    const ConverterRunContext* context = nullptr;
    bool recursive = false;
    bool overwrite = false;
    bool strictParameters = false;
};

struct ConversionResult
{
    bool succeeded = false;
    std::filesystem::path buildFile;
    std::vector<std::filesystem::path> generatedLuaFiles;
    std::vector<std::filesystem::path> generatedFiles;
    std::vector<Diagnostic> diagnostics;
    int syxFilesConverted = 0;
    int messagesConverted = 0;
    int voicesConverted = 0;
};

ConversionResult convertSource(const ConversionOptions& options);
void registerConverter(ConverterRegistry& registry);

} // namespace halionbridge::converters::dx7
