#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace halionbridge::detail
{

struct VstPresetInspectionFile
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
};

struct VstPresetInspectionCollectionResult
{
    std::vector<VstPresetInspectionFile> files;
    std::vector<std::string> errors;
};

struct VstPresetInspectionRuntimeConfig
{
    std::filesystem::path runtimeRoot;
    std::filesystem::path reportPath;
    std::vector<VstPresetInspectionFile> files;
};

struct VstPresetInspectionReportSummary
{
    int total = 0;
    int inspected = 0;
    int failed = 0;
};

VstPresetInspectionCollectionResult collectVstPresetsForInspection(const std::filesystem::path& inputPath, bool recursive);
std::string createVstPresetInspectionRuntimeModuleText(const VstPresetInspectionRuntimeConfig& config);
std::filesystem::path getDefaultHalionInspectionDirectory();
bool validateVstPresetInspectionOutput(const std::filesystem::path& destination, bool overwrite, std::string& error);
bool readVstPresetInspectionReport(const std::filesystem::path& source, std::string& json, std::string& error);
bool validateVstPresetInspectionReport(const std::string& json, int expectedPresetCount, VstPresetInspectionReportSummary& summary,
                                       std::string& error);
bool publishVstPresetInspectionReport(const std::filesystem::path& source, const std::filesystem::path& destination, bool overwrite,
                                      std::string& error);
bool cleanupVstPresetInspectionDirectory(const std::filesystem::path& directory, const std::filesystem::path& expectedInspectionRoot,
                                         std::string& error);

} // namespace halionbridge::detail
