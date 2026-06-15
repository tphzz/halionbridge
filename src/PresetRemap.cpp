#include "PresetRemap.h"

#include "PathUtils.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <system_error>

namespace halionbridge::detail
{
namespace
{

std::string genericPathString(const std::filesystem::path& path)
{
    return path.generic_string();
}

std::string toLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const auto& element : path)
    {
        const auto text = element.generic_string();
        if (text.empty() || text == "." || text == "..")
            return false;
    }

    return true;
}

std::string quoteForLua(std::string text)
{
    auto out = std::string();
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const auto c : text)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

bool copyFileReplacingNever(const std::filesystem::path& source, const std::filesystem::path& destination, std::vector<std::string>& errors)
{
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        errors.push_back("Could not create directory " + destination.parent_path().string() + ": " + ec.message());
        return false;
    }

    if (std::filesystem::exists(destination, ec))
    {
        errors.push_back("Refusing to overwrite existing staged/output file: " + destination.string());
        return false;
    }

    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
    if (ec)
    {
        errors.push_back("Could not copy " + source.string() + " to " + destination.string() + ": " + ec.message());
        return false;
    }

    return true;
}

} // namespace

bool hasVstPresetExtension(const std::filesystem::path& path)
{
    return toLowerAscii(path.extension().string()) == ".vstpreset";
}

std::string normalizePresetRemapRoot(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

PresetRemapCollectionResult collectPresetRemapFiles(const std::filesystem::path& inputDirectory)
{
    PresetRemapCollectionResult result;
    std::error_code ec;

    const auto inputExists = std::filesystem::exists(inputDirectory, ec);
    if (ec)
    {
        result.errors.push_back("Could not inspect input directory " + inputDirectory.string() + ": " + ec.message());
        return result;
    }

    const auto inputIsDirectory = std::filesystem::is_directory(inputDirectory, ec);
    if (ec)
    {
        result.errors.push_back("Could not inspect input directory " + inputDirectory.string() + ": " + ec.message());
        return result;
    }

    if (!inputExists || !inputIsDirectory)
    {
        result.errors.push_back("Input directory does not exist: " + inputDirectory.string());
        return result;
    }

    std::set<std::string> seenRelativePaths;

    for (std::filesystem::recursive_directory_iterator it(inputDirectory, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
            break;

        const auto& entry = *it;
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || !hasVstPresetExtension(entry.path()))
            continue;

        auto relative = std::filesystem::relative(entry.path(), inputDirectory, ec);
        if (ec || !isSafeRelativePath(relative))
        {
            result.errors.push_back("Could not derive a safe relative path for " + entry.path().string());
            ec.clear();
            continue;
        }

        const auto key = toLowerAscii(genericPathString(relative));
        if (!seenRelativePaths.insert(key).second)
        {
            result.errors.push_back("Duplicate relative .vstpreset path after case normalization: " + genericPathString(relative));
            continue;
        }

        result.files.push_back({entry.path(), relative});
    }

    if (ec)
        result.errors.push_back("Could not recursively scan input directory " + inputDirectory.string() + ": " + ec.message());

    std::sort(result.files.begin(), result.files.end(), [](const PresetRemapFile& lhs, const PresetRemapFile& rhs)
              { return toLowerAscii(genericPathString(lhs.relativePath)) < toLowerAscii(genericPathString(rhs.relativePath)); });

    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("Input directory contains no .vstpreset files: " + inputDirectory.string());

    return result;
}

std::string createPresetRemapListText(std::span<const PresetRemapFile> files)
{
    auto text = std::ostringstream();
    for (const auto& file : files)
        text << genericPathString(file.relativePath) << '\n';
    return text.str();
}

std::string createPresetRemapRuntimeModuleText(const PresetRemapRuntimeConfig& config)
{
    const auto runtimeRoot = normalizePresetRemapRoot(genericPathString(config.runtimeRoot));
    const auto listFile = genericPathString(config.listFile);

    auto text = std::ostringstream();
    text << "-- Generated by halionbridge.exe for the embedded HALion preset-remap bootstrap.\n"
         << "HALIONBRIDGE_PRESET_REMAP_ROOT = " << quoteForLua(runtimeRoot) << "\n"
         << "HALIONBRIDGE_PRESET_REMAP_LIST = " << quoteForLua(listFile) << "\n"
         << "HALIONBRIDGE_PRESET_REMAP_OLD_ROOT = " << quoteForLua(normalizePresetRemapRoot(config.oldRoot)) << "\n"
         << "HALIONBRIDGE_PRESET_REMAP_NEW_ROOT = " << quoteForLua(normalizePresetRemapRoot(config.newRoot)) << "\n"
         << "HALIONBRIDGE_PRESET_REMAP_PLUGIN_CODE = " << quoteForLua(config.pluginCode) << "\n\n"
         << "local runtimePathPrefix = HALIONBRIDGE_PRESET_REMAP_ROOT .. \"?.lua;\" .. HALIONBRIDGE_PRESET_REMAP_ROOT .. \"?/init.lua;\"\n"
         << "if not package.path:find(runtimePathPrefix, 1, true) then\n"
         << "    package.path = runtimePathPrefix .. package.path\n"
         << "end\n\n"
         << "package.loaded[\"halionbridge_preset_remap\"] = nil\n"
         << "local ok, result = pcall(require, \"halionbridge_preset_remap\")\n"
         << "package.loaded[\"halionbridge_runtime\"] = nil\n"
         << "if not ok then\n"
         << "    error(\"halionbridge runtime failed while loading halionbridge_preset_remap.lua.\\n\" .. tostring(result))\n"
         << "end\n";
    return text.str();
}

std::filesystem::path getDefaultHalionUserPresetDirectory()
{
    auto root = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                    .getChildFile("VST3 Presets")
                    .getChildFile("Steinberg Media Technologies")
                    .getChildFile("HALion 7");
    return toStdPath(root);
}

bool isDirectoryEmpty(const std::filesystem::path& directory, std::string& errorMessage)
{
    std::error_code ec;
    const auto exists = std::filesystem::exists(directory, ec);
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    if (!exists)
        return true;

    const auto isDirectory = std::filesystem::is_directory(directory, ec);
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    if (!isDirectory)
    {
        errorMessage = "Output path exists but is not a directory: " + directory.string();
        return false;
    }

    auto it = std::filesystem::directory_iterator(directory, ec);
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    if (it != std::filesystem::directory_iterator())
    {
        errorMessage = "Output directory already exists and is not empty: " + directory.string();
        return false;
    }

    return true;
}

bool copyPresetRemapFilesToStage(std::span<const PresetRemapFile> files, const std::filesystem::path& stageDirectory,
                                 std::vector<std::string>& errors)
{
    auto ok = true;
    for (const auto& file : files)
    {
        if (!isSafeRelativePath(file.relativePath))
        {
            errors.push_back("Refusing unsafe relative path: " + genericPathString(file.relativePath));
            ok = false;
            continue;
        }

        ok = copyFileReplacingNever(file.sourcePath, stageDirectory / file.relativePath, errors) && ok;
    }

    return ok;
}

bool copyPresetRemapFilesFromStage(std::span<const PresetRemapFile> files, const std::filesystem::path& stageDirectory,
                                   const std::filesystem::path& outputDirectory, std::vector<std::string>& errors)
{
    auto ok = true;
    for (const auto& file : files)
    {
        if (!isSafeRelativePath(file.relativePath))
        {
            errors.push_back("Refusing unsafe relative path: " + genericPathString(file.relativePath));
            ok = false;
            continue;
        }

        ok = copyFileReplacingNever(stageDirectory / file.relativePath, outputDirectory / file.relativePath, errors) && ok;
    }

    return ok;
}

} // namespace halionbridge::detail
