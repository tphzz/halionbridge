#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace halionbridge::detail
{

struct PresetRemapFile
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct PresetRemapCollectionResult
{
    std::vector<PresetRemapFile> files;
    std::vector<std::string> errors;
};

struct PresetRemapRuntimeConfig
{
    std::filesystem::path runtimeRoot;
    std::vector<std::string> relativePresetPaths;
    std::string oldRoot;
    std::string newRoot;
    std::string pluginCode = "H7";
};

bool hasVstPresetExtension(const std::filesystem::path& path);
std::string normalizePresetRemapRoot(std::string path);
PresetRemapCollectionResult collectPresetRemapFiles(const std::filesystem::path& inputDirectory);
std::string createPresetRemapListText(std::span<const PresetRemapFile> files);
std::string createPresetRemapRuntimeModuleText(const PresetRemapRuntimeConfig& config);
std::filesystem::path getDefaultHalionUserPresetDirectory();
bool isDirectoryEmpty(const std::filesystem::path& directory, std::string& errorMessage);
bool copyPresetRemapFilesToStage(std::span<const PresetRemapFile> files, const std::filesystem::path& stageDirectory,
                                 std::vector<std::string>& errors);
bool copyPresetRemapFilesFromStage(std::span<const PresetRemapFile> files, const std::filesystem::path& stageDirectory,
                                   const std::filesystem::path& outputDirectory, std::vector<std::string>& errors);
bool cleanupPresetRemapStageDirectory(const std::filesystem::path& stageDirectory, const std::filesystem::path& expectedUserPresetRoot,
                                      std::string& errorMessage);

} // namespace halionbridge::detail
