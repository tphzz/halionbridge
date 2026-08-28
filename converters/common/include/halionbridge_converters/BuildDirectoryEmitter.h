#pragma once

#include "halionbridge_converters/Converter.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace halionbridge::converters
{

enum class GeneratedLuaFileRole
{
    buildEntrypoint,
    helperModule
};

struct GeneratedLuaScript
{
    std::string moduleName;
    std::string fileName;
    std::string luaSource;
    GeneratedLuaFileRole role = GeneratedLuaFileRole::buildEntrypoint;
};

struct GeneratedBuildFile
{
    std::filesystem::path relativePath;
    std::vector<std::byte> data;
};

struct BuildDirectoryRequest
{
    std::filesystem::path outputDirectory;
    bool overwrite = false;
    std::vector<GeneratedLuaScript> scripts;
    std::vector<GeneratedBuildFile> files;
};

struct BuildDirectoryResult
{
    bool succeeded = false;
    std::filesystem::path buildFile;
    std::vector<std::filesystem::path> generatedLuaFiles;
    std::vector<std::filesystem::path> generatedFiles;
    std::vector<Diagnostic> diagnostics;
};

std::string luaQuotedString(std::string_view text);
BuildDirectoryResult writeBuildDirectory(const BuildDirectoryRequest& request);

} // namespace halionbridge::converters
