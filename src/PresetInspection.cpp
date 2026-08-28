#include "PresetInspection.h"

#include "PathUtils.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace halionbridge::detail
{
namespace
{

constexpr std::string_view kInspectionDirectoryPrefix = "halionbridge-inspect-";
constexpr std::uintmax_t kMaximumInspectionReportBytes = 64U * 1024U * 1024U;

std::string lowerCaseAscii(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return text;
}

bool hasVstPresetExtension(const std::filesystem::path& path)
{
    return lowerCaseAscii(path.extension().string()) == ".vstpreset";
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const auto& component : path)
    {
        const auto text = component.generic_string();
        if (text.empty() || text == "." || text == "..")
            return false;
    }

    return true;
}

std::filesystem::path comparableAbsolutePath(const std::filesystem::path& path)
{
    auto error = std::error_code{};
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error)
        result = std::filesystem::absolute(path, error);
    if (error)
        result = path;

    result = result.lexically_normal();
#if JUCE_WINDOWS
    return std::filesystem::path(lowerCaseAscii(result.generic_string()));
#else
    return result;
#endif
}

bool isMissingPathError(const std::error_code& error)
{
    if (error == std::errc::no_such_file_or_directory)
        return true;
#if JUCE_WINDOWS
    return error.category() == std::system_category() && (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND);
#else
    return false;
#endif
}

std::string normalizedLuaPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

std::string quoteForLua(std::string text)
{
    auto result = std::string{};
    result.reserve(text.size() + 2);
    result.push_back('"');
    for (const auto value : text)
    {
        switch (value)
        {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\n':
            result += "\\n";
            break;
        default:
            result.push_back(value);
            break;
        }
    }
    result.push_back('"');
    return result;
}

void addInspectionFile(VstPresetInspectionCollectionResult& result, std::set<std::string>& seen, const std::filesystem::path& source,
                       const std::filesystem::path& relative)
{
    if (!isSafeRelativePath(relative))
    {
        result.errors.push_back("Could not derive a safe relative path for " + source.string());
        return;
    }

    const auto key = lowerCaseAscii(relative.generic_string());
    if (!seen.insert(key).second)
    {
        result.errors.push_back("Duplicate relative .vstpreset path after case normalization: " + relative.generic_string());
        return;
    }

    result.files.push_back({source, relative});
}

std::optional<int> integerProperty(const juce::DynamicObject& object, const juce::Identifier& name)
{
    const auto value = object.getProperty(name);
    if (!value.isInt() && !value.isInt64())
        return std::nullopt;

    const auto integer = static_cast<int64_t>(value);
    if (integer < 0 || integer > (std::numeric_limits<int>::max)())
        return std::nullopt;

    return static_cast<int>(integer);
}

} // namespace

VstPresetInspectionCollectionResult collectVstPresetsForInspection(const std::filesystem::path& inputPath, const bool recursive)
{
    auto result = VstPresetInspectionCollectionResult{};
    auto seen = std::set<std::string>{};
    auto error = std::error_code{};
    const auto status = std::filesystem::symlink_status(inputPath, error);
    if (error)
    {
        result.errors.push_back("Could not inspect input path " + inputPath.string() + ": " + error.message());
        return result;
    }

    if (std::filesystem::is_symlink(status))
    {
        result.errors.push_back("Inspection input must not be a symbolic link: " + inputPath.string());
        return result;
    }

    if (std::filesystem::is_regular_file(status))
    {
        if (!hasVstPresetExtension(inputPath))
            result.errors.push_back("Inspection input file must have a .vstpreset extension: " + inputPath.string());
        else
            addInspectionFile(result, seen, inputPath, inputPath.filename());
        return result;
    }

    if (!std::filesystem::is_directory(status))
    {
        result.errors.push_back("Inspection input path is not a file or directory: " + inputPath.string());
        return result;
    }

    const auto considerEntry = [&](const std::filesystem::directory_entry& entry)
    {
        auto entryError = std::error_code{};
        const auto entryStatus = entry.symlink_status(entryError);
        if (entryError || std::filesystem::is_symlink(entryStatus) || !std::filesystem::is_regular_file(entryStatus) ||
            !hasVstPresetExtension(entry.path()))
            return;

        auto relativeError = std::error_code{};
        const auto relative = std::filesystem::relative(entry.path(), inputPath, relativeError);
        if (relativeError)
        {
            result.errors.push_back("Could not derive a relative path for " + entry.path().string() + ": " + relativeError.message());
            return;
        }
        addInspectionFile(result, seen, entry.path(), relative);
    };

    if (recursive)
    {
        for (auto iterator = std::filesystem::recursive_directory_iterator(
                 inputPath, std::filesystem::directory_options::skip_permission_denied, error);
             !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
            considerEntry(*iterator);
    }
    else
    {
        for (auto iterator =
                 std::filesystem::directory_iterator(inputPath, std::filesystem::directory_options::skip_permission_denied, error);
             !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error))
            considerEntry(*iterator);
    }

    if (error)
        result.errors.push_back("Could not scan inspection input directory " + inputPath.string() + ": " + error.message());

    std::ranges::sort(result.files, [](const auto& left, const auto& right)
                      { return lowerCaseAscii(left.relativePath.generic_string()) < lowerCaseAscii(right.relativePath.generic_string()); });

    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("Inspection input contains no .vstpreset files: " + inputPath.string());

    return result;
}

std::string createVstPresetInspectionRuntimeModuleText(const VstPresetInspectionRuntimeConfig& config)
{
    auto text = std::ostringstream{};
    text << "-- Generated by halionbridge.exe for the embedded HALion preset-inspection bootstrap.\n"
         << "HALIONBRIDGE_PRESET_INSPECTION_ROOT = " << quoteForLua(normalizedLuaPath(config.runtimeRoot)) << "\n"
         << "HALIONBRIDGE_PRESET_INSPECTION_REPORT = " << quoteForLua(normalizedLuaPath(config.reportPath)) << "\n"
         << "HALIONBRIDGE_PRESET_INSPECTION_PRESETS = {\n";
    for (const auto& file : config.files)
    {
        text << "    { source = " << quoteForLua(normalizedLuaPath(file.sourcePath))
             << ", relative = " << quoteForLua(file.relativePath.generic_string()) << " },\n";
    }
    text << "}\n\n"
         << "package.loaded[\"halionbridge_preset_inspect\"] = nil\n"
         << "local ok, result = pcall(require, \"halionbridge_preset_inspect\")\n"
         << "package.loaded[\"halionbridge_runtime\"] = nil\n"
         << "if not ok then\n"
         << "    error(\"halionbridge runtime failed while loading halionbridge_preset_inspect.lua.\\n\" .. tostring(result))\n"
         << "end\n";
    return text.str();
}

std::filesystem::path getDefaultHalionInspectionDirectory()
{
    return toStdPath(juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Steinberg"));
}

bool validateVstPresetInspectionOutput(const std::filesystem::path& destination, const bool overwrite, std::string& error)
{
    error.clear();
    if (destination.empty() || lowerCaseAscii(destination.extension().string()) != ".json")
    {
        error = "Inspection output must name a .json file.";
        return false;
    }

    auto filesystemError = std::error_code{};
    const auto exists = std::filesystem::exists(destination, filesystemError);
    if (filesystemError)
    {
        error = "Could not inspect output JSON path " + destination.string() + ": " + filesystemError.message();
        return false;
    }

    if (exists)
    {
        const auto status = std::filesystem::symlink_status(destination, filesystemError);
        if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
        {
            error = "Inspection report output exists but is not a regular non-symlink file: " + destination.string();
            return false;
        }
        if (!overwrite)
        {
            error = "Inspection report already exists. Use --overwrite to replace it: " + destination.string();
            return false;
        }
    }

    const auto parent = destination.parent_path();
    if (!parent.empty() && std::filesystem::exists(parent, filesystemError))
    {
        const auto status = std::filesystem::symlink_status(parent, filesystemError);
        if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
        {
            error = "Inspection report parent exists but is not a regular non-symlink directory: " + parent.string();
            return false;
        }
    }
    else if (filesystemError)
    {
        error = "Could not inspect output JSON parent " + parent.string() + ": " + filesystemError.message();
        return false;
    }

    return true;
}

bool readVstPresetInspectionReport(const std::filesystem::path& source, std::string& json, std::string& error)
{
    json.clear();
    error.clear();

    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(source, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "HALion inspection report is missing or is not a regular non-symlink file: " + source.string();
        return false;
    }

    const auto size = std::filesystem::file_size(source, filesystemError);
    if (filesystemError)
    {
        error = "Could not determine HALion inspection report size: " + filesystemError.message();
        return false;
    }
    if (size > kMaximumInspectionReportBytes)
    {
        error = "HALion inspection report exceeds the 64 MiB safety limit: " + source.string();
        return false;
    }

    auto stream = std::ifstream(source, std::ios::binary);
    if (!stream)
    {
        error = "Could not open HALion inspection report: " + source.string();
        return false;
    }

    json.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (stream.bad())
    {
        error = "Could not read HALion inspection report completely: " + source.string();
        json.clear();
        return false;
    }
    return true;
}

bool validateVstPresetInspectionReport(const std::string& json, const int expectedPresetCount, VstPresetInspectionReportSummary& summary,
                                       std::string& error)
{
    summary = {};
    error.clear();

    auto root = juce::var{};
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.data(), static_cast<int>(json.size())), root);
    if (parsed.failed())
    {
        error = "Inspection report is not valid JSON: " + parsed.getErrorMessage().toStdString();
        return false;
    }

    const auto* object = root.getDynamicObject();
    if (object == nullptr || object->getProperty("format").toString() != "halionbridge-vstpreset-inspection")
    {
        error = "Inspection report has an invalid format identifier.";
        return false;
    }

    const auto version = integerProperty(*object, "format_version");
    if (!version || *version != 1)
    {
        error = "Inspection report format_version must be the integer 1.";
        return false;
    }

    const auto* presets = object->getProperty("presets").getArray();
    const auto* summaryObject = object->getProperty("summary").getDynamicObject();
    if (presets == nullptr || summaryObject == nullptr)
    {
        error = "Inspection report must contain presets and summary.";
        return false;
    }

    const auto total = integerProperty(*summaryObject, "total");
    const auto inspected = integerProperty(*summaryObject, "inspected");
    const auto failed = integerProperty(*summaryObject, "failed");
    if (!total || !inspected || !failed || *total != expectedPresetCount || *total != presets->size() || *inspected + *failed != *total)
    {
        error = "Inspection report summary counts are inconsistent with the requested preset batch.";
        return false;
    }

    auto inspectedRecords = 0;
    auto failedRecords = 0;
    for (const auto& preset : *presets)
    {
        const auto* presetObject = preset.getDynamicObject();
        if (presetObject == nullptr || !presetObject->getProperty("path").isString() || !presetObject->getProperty("ok").isBool() ||
            presetObject->getProperty("errors").getArray() == nullptr)
        {
            error = "Inspection report contains an invalid preset record.";
            return false;
        }

        if (static_cast<bool>(presetObject->getProperty("ok")))
            ++inspectedRecords;
        else
            ++failedRecords;
    }

    if (inspectedRecords != *inspected || failedRecords != *failed)
    {
        error = "Inspection report summary counts disagree with the preset records.";
        return false;
    }

    summary = {*total, *inspected, *failed};
    return true;
}

bool publishVstPresetInspectionReport(const std::filesystem::path& source, const std::filesystem::path& destination, const bool overwrite,
                                      std::string& error)
{
    error.clear();
    auto filesystemError = std::error_code{};
    if (!std::filesystem::is_regular_file(source, filesystemError) || filesystemError)
    {
        error = "HALion inspection report is missing or is not a regular file: " + source.string();
        return false;
    }

    if (!validateVstPresetInspectionOutput(destination, overwrite, error))
        return false;

    if (!destination.parent_path().empty())
        std::filesystem::create_directories(destination.parent_path(), filesystemError);
    if (filesystemError)
    {
        error =
            "Could not create inspection report output directory " + destination.parent_path().string() + ": " + filesystemError.message();
        return false;
    }

    const auto temporary =
        destination.parent_path() / (destination.filename().string() + ".halionbridge-" + juce::Uuid().toString().toStdString() + ".tmp");
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::none, filesystemError);
    if (filesystemError)
    {
        error = "Could not stage inspection report beside its destination: " + filesystemError.message();
        return false;
    }

#if JUCE_WINDOWS
    const auto moved = MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!moved)
        filesystemError = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    std::filesystem::rename(temporary, destination, filesystemError);
#endif
    if (filesystemError)
    {
        error = "Could not move inspection report into place at " + destination.string() + ": " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

bool cleanupVstPresetInspectionDirectory(const std::filesystem::path& directory, const std::filesystem::path& expectedInspectionRoot,
                                         std::string& error)
{
    error.clear();
    if (directory.empty() || expectedInspectionRoot.empty() || directory.filename().string().rfind(kInspectionDirectoryPrefix, 0) != 0 ||
        comparableAbsolutePath(directory.parent_path()) != comparableAbsolutePath(expectedInspectionRoot))
    {
        error = "Refusing to clean a path outside the expected halionbridge inspection staging area: " + directory.string();
        return false;
    }

    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(directory, filesystemError);
    if (filesystemError)
    {
        if (isMissingPathError(filesystemError))
            return true;
        error = "Could not inspect temporary preset-inspection directory " + directory.string() + ": " + filesystemError.message();
        return false;
    }
    if (status.type() == std::filesystem::file_type::not_found)
        return true;
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
    {
        error = "Refusing to clean a non-directory or symlinked inspection staging root: " + directory.string();
        return false;
    }

    std::filesystem::remove_all(directory, filesystemError);
    if (filesystemError && !isMissingPathError(filesystemError))
    {
        error = "Could not delete temporary preset-inspection directory " + directory.string() + ": " + filesystemError.message();
        return false;
    }
    return true;
}

} // namespace halionbridge::detail
