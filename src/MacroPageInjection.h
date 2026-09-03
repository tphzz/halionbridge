#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace halionbridge::detail
{

struct MacroPageInjectionFile
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct MacroPageInjectionCollectionResult
{
    std::vector<MacroPageInjectionFile> files;
    std::vector<std::string> errors;
};

struct MacroPageInjectionRuntimeEntry
{
    std::size_t index = 0;
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct MacroPageInjectionRuntimeConfig
{
    std::filesystem::path donorPreset;
    std::filesystem::path outputDirectory;
    std::filesystem::path receiptPath;
    std::string token;
    bool preflight = false;
    bool validateOnly = false;
    bool failFast = false;
    std::vector<MacroPageInjectionRuntimeEntry> entries;
};

struct MacroPageInjectionReceiptResult
{
    std::vector<std::size_t> completedIndices;
    std::vector<std::pair<std::size_t, std::string>> reportedFailures;
    std::vector<std::string> errors;
};

struct MacroPageInjectionWorkPaths
{
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::filesystem::path completionJournal;
    std::filesystem::path runtime;
    std::filesystem::path presets;
};

struct MacroPageInjectionManifestEntry
{
    std::filesystem::path relativePath;
    std::string sourceSha256;

    bool operator==(const MacroPageInjectionManifestEntry&) const = default;
};

struct MacroPageInjectionManifest
{
    std::filesystem::path inputDirectory;
    std::filesystem::path outputDirectory;
    std::filesystem::path donorPreset;
    std::string donorSha256;
    bool recursive = false;
    std::vector<MacroPageInjectionManifestEntry> files;

    bool operator==(const MacroPageInjectionManifest&) const = default;
};

struct MacroPageInjectionCompletion
{
    std::size_t index = 0;
    std::string sourceSha256;
    std::string outputSha256;
};

struct MacroPageInjectionCompletionReadResult
{
    std::map<std::size_t, MacroPageInjectionCompletion> records;
    std::vector<std::string> errors;
};

MacroPageInjectionCollectionResult collectMacroPageInjectionFiles(const std::filesystem::path& inputDirectory, bool recursive);
std::string createMacroPageInjectionRuntimeModuleText(const MacroPageInjectionRuntimeConfig& config);
MacroPageInjectionReceiptResult parseMacroPageInjectionReceipt(std::string_view text, std::string_view expectedToken,
                                                               std::span<const std::size_t> expectedIndices);
MacroPageInjectionWorkPaths makeMacroPageInjectionWorkPaths(const std::filesystem::path& outputDirectory);
bool writeMacroPageInjectionManifest(const std::filesystem::path& path, const MacroPageInjectionManifest& manifest, std::string& error);
bool readMacroPageInjectionManifest(const std::filesystem::path& path, MacroPageInjectionManifest& manifest, std::string& error);
bool macroPageInjectionManifestsMatch(const MacroPageInjectionManifest& expected, const MacroPageInjectionManifest& actual,
                                      std::string& error);
bool appendMacroPageInjectionCompletion(const std::filesystem::path& path, const MacroPageInjectionCompletion& completion,
                                        std::string& error);
MacroPageInjectionCompletionReadResult readMacroPageInjectionCompletions(const std::filesystem::path& path, std::size_t maximumEntryCount);
bool publishMacroPageInjectionPresets(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& outputDirectory,
                                      std::string& error);
std::optional<std::string> sha256MacroPageInjectionFile(const std::filesystem::path& path, std::string& error);
bool writeMacroPageInjectionRuntimeFile(const std::filesystem::path& path, std::string_view text, std::string& error);
bool readMacroPageInjectionReceiptFile(const std::filesystem::path& path, std::string& text, std::string& error);
bool removeMacroPageInjectionStagedPreset(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& relativePath,
                                          std::string& error);
bool cleanupMacroPageInjectionWorkDirectory(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& outputDirectory,
                                            std::string& error);

} // namespace halionbridge::detail
