#include "MacroPageInjection.h"
#include "VstPresetMetadata.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace halionbridge::detail
{
namespace
{

constexpr auto kManifestFormatVersion = 1;
constexpr auto kTransformationRevision = 2;

std::string toLowerAscii(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool hasVstPresetExtension(const std::filesystem::path& path)
{
    return toLowerAscii(path.extension().string()) == ".vstpreset";
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    return std::ranges::none_of(path,
                                [](const auto& part)
                                {
                                    const auto text = part.generic_string();
                                    return text.empty() || text == "." || text == "..";
                                });
}

std::string genericPathString(const std::filesystem::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string quoteForLua(const std::string_view value)
{
    auto result = std::string{"\""};
    result.reserve(value.size() + 2);
    for (const auto character : value)
    {
        switch (character)
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
            result += character;
            break;
        }
    }
    result += '"';
    return result;
}

std::filesystem::path pathFromUtf8(const std::string_view value)
{
#if JUCE_WINDOWS
    const auto text = juce::String::fromUTF8(value.data(), static_cast<int>(value.size()));
    return std::filesystem::path(std::wstring(text.toWideCharPointer()));
#else
    return std::filesystem::path(std::string(value));
#endif
}

bool writeTextAtomically(const std::filesystem::path& path, const std::string_view text, std::string& error)
{
    auto filesystemError = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError)
    {
        error = "Could not create macro-page work directory " + path.parent_path().string() + ": " + filesystemError.message();
        return false;
    }
    if (std::filesystem::exists(path, filesystemError))
    {
        error = "Refusing to overwrite existing macro-page work file: " + path.string();
        return false;
    }
    if (filesystemError)
    {
        error = "Could not inspect macro-page work file " + path.string() + ": " + filesystemError.message();
        return false;
    }

    const auto temporary = path.parent_path() / (path.filename().string() + "." + juce::Uuid().toString().toStdString() + ".tmp");
    {
        auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
        {
            error = "Could not write macro-page work file: " + temporary.string();
            stream.close();
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
    }
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError)
    {
        error = "Could not publish macro-page work file " + path.string() + ": " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
    return true;
}

bool replaceTextAtomically(const std::filesystem::path& path, const std::string_view text, std::string& error)
{
    auto filesystemError = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), filesystemError);
    if (filesystemError)
    {
        error = "Could not create macro-page runtime directory " + path.parent_path().string() + ": " + filesystemError.message();
        return false;
    }

    const auto destinationStatus = std::filesystem::symlink_status(path, filesystemError);
    if (destinationStatus.type() != std::filesystem::file_type::not_found &&
        (filesystemError || std::filesystem::is_symlink(destinationStatus) || !std::filesystem::is_regular_file(destinationStatus)))
    {
        error = "Macro-page runtime destination is not a regular non-symlink file: " + path.string();
        return false;
    }
    filesystemError.clear();

    const auto temporary = path.parent_path() / (path.filename().string() + "." + juce::Uuid().toString().toStdString() + ".tmp");
    {
        auto stream = std::ofstream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream)
        {
            error = "Could not write macro-page runtime file: " + temporary.string();
            stream.close();
            std::filesystem::remove(temporary, filesystemError);
            return false;
        }
    }

#if JUCE_WINDOWS
    const auto destinationExists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const auto replaced = destinationExists
                              ? ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
                              : MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
    if (!replaced)
    {
        error = "Could not publish macro-page runtime file " + path.string() + ": Windows error " + std::to_string(GetLastError());
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
#else
    std::filesystem::rename(temporary, path, filesystemError);
    if (filesystemError)
    {
        error = "Could not publish macro-page runtime file " + path.string() + ": " + filesystemError.message();
        std::filesystem::remove(temporary, filesystemError);
        return false;
    }
#endif
    return true;
}

bool readTextFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "Macro-page work file is missing, not regular, or symlinked: " + path.string();
        return false;
    }
    auto stream = std::ifstream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        error = "Could not open macro-page work file: " + path.string();
        return false;
    }
    const auto size = stream.tellg();
    constexpr auto maximumBytes = std::streamoff{256 * 1024 * 1024};
    if (size < 0 || size > maximumBytes)
    {
        error = "Macro-page work file has an invalid or excessive size: " + path.string();
        return false;
    }
    text.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!text.empty())
        stream.read(text.data(), size);
    if (!stream)
    {
        error = "Could not read macro-page work file: " + path.string();
        return false;
    }
    return true;
}

bool readStringProperty(const juce::DynamicObject& object, const char* name, std::string& value, std::string& error)
{
    const auto property = object.getProperty(name);
    if (!property.isString())
    {
        error = std::string{"Macro-page manifest property must be a string: "} + name;
        return false;
    }
    value = property.toString().toStdString();
    return true;
}

bool pathStatusIsDirectoryNoFollow(const std::filesystem::path& path, std::string& error)
{
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
    {
        error = "Macro-page staging path is not a non-symlink directory: " + path.string();
        return false;
    }
    return true;
}

} // namespace

MacroPageInjectionCollectionResult collectMacroPageInjectionFiles(const std::filesystem::path& inputDirectory, const bool recursive)
{
    auto result = MacroPageInjectionCollectionResult{};
    auto error = std::error_code{};
    const auto rootStatus = std::filesystem::symlink_status(inputDirectory, error);
    if (error || !std::filesystem::is_directory(rootStatus) || std::filesystem::is_symlink(rootStatus))
    {
        result.errors.push_back("Input directory must be an existing non-symlink directory: " + inputDirectory.string());
        return result;
    }

    auto seen = std::set<std::string>{};
    const auto addEntry = [&](const std::filesystem::directory_entry& entry)
    {
        auto statusError = std::error_code{};
        const auto status = entry.symlink_status(statusError);
        if (statusError)
        {
            result.errors.push_back("Could not inspect input entry " + entry.path().string() + ": " + statusError.message());
            return;
        }
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || !hasVstPresetExtension(entry.path()))
            return;

        auto relativeError = std::error_code{};
        auto relativePath = std::filesystem::relative(entry.path(), inputDirectory, relativeError);
        if (relativeError || !isSafeRelativePath(relativePath))
        {
            result.errors.push_back("Could not derive a safe relative path for " + entry.path().string());
            return;
        }

        const auto relativeText = genericPathString(relativePath);
        if (!seen.insert(toLowerAscii(relativeText)).second)
        {
            result.errors.push_back("Duplicate relative .vstpreset path after case normalization: " + relativeText);
            return;
        }
        result.files.push_back({entry.path(), std::move(relativePath)});
    };

    if (recursive)
    {
        for (auto iterator = std::filesystem::recursive_directory_iterator(inputDirectory, std::filesystem::directory_options::none, error);
             !error && iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(error))
        {
            auto statusError = std::error_code{};
            const auto status = iterator->symlink_status(statusError);
            if (!statusError && std::filesystem::is_symlink(status) && iterator->is_directory(statusError))
                iterator.disable_recursion_pending();
            addEntry(*iterator);
        }
    }
    else
    {
        for (auto iterator = std::filesystem::directory_iterator(inputDirectory, std::filesystem::directory_options::none, error);
             !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error))
            addEntry(*iterator);
    }

    if (error)
        result.errors.push_back("Could not scan input directory " + inputDirectory.string() + ": " + error.message());

    std::ranges::sort(result.files, [](const auto& left, const auto& right)
                      { return toLowerAscii(genericPathString(left.relativePath)) < toLowerAscii(genericPathString(right.relativePath)); });
    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("Input directory contains no .vstpreset files: " + inputDirectory.string());
    return result;
}

std::string createMacroPageInjectionRuntimeModuleText(const MacroPageInjectionRuntimeConfig& config)
{
    auto text = std::ostringstream{};
    text << "-- Generated by halionbridge for one isolated macro-page injection chunk.\n"
         << "HALIONBRIDGE_MACRO_PAGE_DONOR = " << quoteForLua(genericPathString(config.donorPreset)) << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_OUTPUT_ROOT = " << quoteForLua(genericPathString(config.outputDirectory)) << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_RECEIPT = " << quoteForLua(genericPathString(config.receiptPath)) << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_TOKEN = " << quoteForLua(config.token) << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_PREFLIGHT = " << (config.preflight ? "true" : "false") << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_VALIDATE_ONLY = " << (config.validateOnly ? "true" : "false") << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_FAIL_FAST = " << (config.failFast ? "true" : "false") << "\n"
         << "HALIONBRIDGE_MACRO_PAGE_PRESETS = {\n";
    for (const auto& entry : config.entries)
    {
        text << "    { index = " << entry.index << ", source = " << quoteForLua(genericPathString(entry.sourcePath))
             << ", relative = " << quoteForLua(genericPathString(entry.relativePath)) << " },\n";
    }
    text << "}\n\n"
         << "package.loaded[\"halionbridge_macro_page_inject\"] = nil\n"
         << "local inject = require(\"halionbridge_macro_page_inject\")\n"
         << "return inject\n";
    return text.str();
}

MacroPageInjectionReceiptResult parseMacroPageInjectionReceipt(const std::string_view text, const std::string_view expectedToken,
                                                               const std::span<const std::size_t> expectedIndices)
{
    auto result = MacroPageInjectionReceiptResult{};
    const auto expected = std::set<std::size_t>(expectedIndices.begin(), expectedIndices.end());
    auto recorded = std::set<std::size_t>{};
    auto offset = std::size_t{0};
    auto lineNumber = std::size_t{0};

    while (offset < text.size())
    {
        const auto end = text.find('\n', offset);
        if (end == std::string_view::npos)
            break;

        ++lineNumber;
        auto line = text.substr(offset, end - offset);
        offset = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty())
            continue;

        const auto firstTab = line.find('\t');
        const auto secondTab = firstTab == std::string_view::npos ? std::string_view::npos : line.find('\t', firstTab + 1);
        const auto recordType = firstTab == std::string_view::npos ? std::string_view{} : line.substr(0, firstTab);
        const auto thirdTab = secondTab == std::string_view::npos ? std::string_view::npos : line.find('\t', secondTab + 1);
        const auto isSuccess = recordType == "HBMPI1" && thirdTab == std::string_view::npos;
        const auto isFailure =
            recordType == "HBMPE1" && thirdTab != std::string_view::npos && line.find('\t', thirdTab + 1) == std::string_view::npos;
        if (firstTab == std::string_view::npos || secondTab == std::string_view::npos || (!isSuccess && !isFailure))
        {
            result.errors.push_back("Malformed macro-page receipt line " + std::to_string(lineNumber) + ".");
            continue;
        }

        if (line.substr(firstTab + 1, secondTab - firstTab - 1) != expectedToken)
        {
            result.errors.push_back("Macro-page receipt line " + std::to_string(lineNumber) + " has a stale invocation token.");
            continue;
        }

        auto index = std::size_t{};
        const auto indexText = line.substr(secondTab + 1, (isFailure ? thirdTab : line.size()) - secondTab - 1);
        const auto [pointer, error] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
        if (error != std::errc{} || pointer != indexText.data() + indexText.size() || !expected.contains(index))
        {
            result.errors.push_back("Macro-page receipt line " + std::to_string(lineNumber) + " has an unexpected index.");
            continue;
        }
        if (!recorded.insert(index).second)
        {
            result.errors.push_back("Macro-page receipt contains duplicate index " + std::to_string(index) + ".");
            continue;
        }
        if (isFailure)
        {
            const auto message = line.substr(thirdTab + 1);
            if (message.empty())
            {
                result.errors.push_back("Macro-page failure receipt has an empty message for index " + std::to_string(index) + ".");
                continue;
            }
            result.reportedFailures.emplace_back(index, std::string(message));
            continue;
        }
        result.completedIndices.push_back(index);
    }

    return result;
}

MacroPageInjectionWorkPaths makeMacroPageInjectionWorkPaths(const std::filesystem::path& outputDirectory)
{
    auto root = outputDirectory;
    root += ".halionbridge-macro-page-work";
    return {root, root / "manifest.json", root / "completed.log", root / "runtime", root / "presets"};
}

bool writeMacroPageInjectionManifest(const std::filesystem::path& path, const MacroPageInjectionManifest& manifest, std::string& error)
{
    auto root = juce::var(new juce::DynamicObject());
    auto* object = root.getDynamicObject();
    object->setProperty("format", "halionbridge-macro-page-injection");
    object->setProperty("format_version", kManifestFormatVersion);
    object->setProperty("transformation_revision", kTransformationRevision);
    object->setProperty("input_directory", juce::String::fromUTF8(genericPathString(manifest.inputDirectory).c_str()));
    object->setProperty("output_directory", juce::String::fromUTF8(genericPathString(manifest.outputDirectory).c_str()));
    object->setProperty("donor_preset", juce::String::fromUTF8(genericPathString(manifest.donorPreset).c_str()));
    object->setProperty("donor_sha256", juce::String::fromUTF8(manifest.donorSha256.c_str()));
    object->setProperty("recursive", manifest.recursive);

    auto files = juce::Array<juce::var>{};
    files.ensureStorageAllocated(static_cast<int>(manifest.files.size()));
    for (const auto& file : manifest.files)
    {
        auto value = juce::var(new juce::DynamicObject());
        value.getDynamicObject()->setProperty("path", juce::String::fromUTF8(genericPathString(file.relativePath).c_str()));
        value.getDynamicObject()->setProperty("source_sha256", juce::String::fromUTF8(file.sourceSha256.c_str()));
        files.add(std::move(value));
    }
    object->setProperty("files", files);
    const auto json = juce::JSON::toString(root, true).toStdString() + "\n";
    return writeTextAtomically(path, json, error);
}

bool readMacroPageInjectionManifest(const std::filesystem::path& path, MacroPageInjectionManifest& manifest, std::string& error)
{
    auto text = std::string{};
    if (!readTextFile(path, text, error))
        return false;

    auto root = juce::var{};
    const auto parseResult = juce::JSON::parse(juce::String::fromUTF8(text.data(), static_cast<int>(text.size())), root);
    if (parseResult.failed())
    {
        error = "Macro-page manifest is not valid JSON: " + parseResult.getErrorMessage().toStdString();
        return false;
    }
    const auto* object = root.getDynamicObject();
    const auto formatVersion = object == nullptr ? juce::var{} : object->getProperty("format_version");
    const auto transformationRevision = object == nullptr ? juce::var{} : object->getProperty("transformation_revision");
    if (object == nullptr || object->getProperty("format").toString() != "halionbridge-macro-page-injection" || !formatVersion.isInt() ||
        static_cast<int>(formatVersion) != kManifestFormatVersion || !transformationRevision.isInt() ||
        !object->getProperty("recursive").isBool())
    {
        error = "Macro-page manifest has an unsupported or malformed header.";
        return false;
    }
    if (const auto revision = static_cast<int>(transformationRevision); revision != kTransformationRevision)
    {
        error = revision == 1
                    ? "Macro-page manifest transformation revision 1 used the rejected Layer-donor topology and cannot be resumed. "
                      "Remove the retained work directory and restart without --resume."
                    : "Macro-page manifest has an unsupported transformation revision " + std::to_string(revision) + ".";
        return false;
    }

    auto input = std::string{};
    auto output = std::string{};
    auto donor = std::string{};
    if (!readStringProperty(*object, "input_directory", input, error) || !readStringProperty(*object, "output_directory", output, error) ||
        !readStringProperty(*object, "donor_preset", donor, error) ||
        !readStringProperty(*object, "donor_sha256", manifest.donorSha256, error))
        return false;

    manifest.inputDirectory = pathFromUtf8(input);
    manifest.outputDirectory = pathFromUtf8(output);
    manifest.donorPreset = pathFromUtf8(donor);
    manifest.recursive = static_cast<bool>(object->getProperty("recursive"));
    manifest.files.clear();
    const auto* files = object->getProperty("files").getArray();
    if (files == nullptr)
    {
        error = "Macro-page manifest files property must be an array.";
        return false;
    }
    manifest.files.reserve(static_cast<std::size_t>(files->size()));
    auto seen = std::set<std::string>{};
    for (const auto& value : *files)
    {
        const auto* file = value.getDynamicObject();
        auto relative = std::string{};
        auto hash = std::string{};
        if (file == nullptr || !readStringProperty(*file, "path", relative, error) ||
            !readStringProperty(*file, "source_sha256", hash, error))
            return false;
        auto relativePath = pathFromUtf8(relative);
        if (!isSafeRelativePath(relativePath) || !seen.insert(toLowerAscii(genericPathString(relativePath))).second)
        {
            error = "Macro-page manifest contains an unsafe or duplicate relative path: " + relative;
            return false;
        }
        manifest.files.push_back({std::move(relativePath), std::move(hash)});
    }
    if (manifest.files.empty())
    {
        error = "Macro-page manifest contains no preset files.";
        return false;
    }
    return true;
}

bool macroPageInjectionManifestsMatch(const MacroPageInjectionManifest& expected, const MacroPageInjectionManifest& actual,
                                      std::string& error)
{
    if (expected == actual)
        return true;
    error = "The interrupted macro-page run does not match the current input, donor, output, recursion mode, or source hashes.";
    return false;
}

bool appendMacroPageInjectionCompletion(const std::filesystem::path& path, const MacroPageInjectionCompletion& completion,
                                        std::string& error)
{
    error.clear();
    if (completion.sourceSha256.empty() || completion.outputSha256.empty() || completion.sourceSha256.contains('\t') ||
        completion.sourceSha256.contains('\n') || completion.outputSha256.contains('\t') || completion.outputSha256.contains('\n'))
    {
        error = "Macro-page completion hashes are invalid.";
        return false;
    }
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (status.type() != std::filesystem::file_type::not_found &&
        (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)))
    {
        error = "Macro-page completion journal is not a regular non-symlink file: " + path.string();
        return false;
    }

    auto stream = std::ofstream(path, std::ios::binary | std::ios::app);
    if (!stream)
    {
        error = "Could not open macro-page completion journal: " + path.string();
        return false;
    }
    stream << "HBMPJ1\t" << completion.index << '\t' << completion.sourceSha256 << '\t' << completion.outputSha256 << '\n';
    stream.flush();
    if (!stream)
    {
        error = "Could not flush macro-page completion journal: " + path.string();
        return false;
    }
    return true;
}

MacroPageInjectionCompletionReadResult readMacroPageInjectionCompletions(const std::filesystem::path& path,
                                                                         const std::size_t maximumEntryCount)
{
    auto result = MacroPageInjectionCompletionReadResult{};
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (status.type() == std::filesystem::file_type::not_found ||
        filesystemError == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        return result;
    }
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        result.errors.push_back("Macro-page completion journal is not a regular non-symlink file: " + path.string());
        return result;
    }
    auto text = std::string{};
    auto readError = std::string{};
    if (!readTextFile(path, text, readError))
    {
        result.errors.push_back(std::move(readError));
        return result;
    }

    auto offset = std::size_t{};
    auto lineNumber = std::size_t{};
    while (offset < text.size())
    {
        const auto end = text.find('\n', offset);
        if (end == std::string::npos)
            break;
        ++lineNumber;
        auto line = std::string_view(text).substr(offset, end - offset);
        offset = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const auto first = line.find('\t');
        const auto second = first == std::string_view::npos ? first : line.find('\t', first + 1);
        const auto third = second == std::string_view::npos ? second : line.find('\t', second + 1);
        if (first == std::string_view::npos || second == std::string_view::npos || third == std::string_view::npos ||
            line.find('\t', third + 1) != std::string_view::npos || line.substr(0, first) != "HBMPJ1")
        {
            result.errors.push_back("Malformed macro-page completion journal line " + std::to_string(lineNumber) + ".");
            continue;
        }
        auto index = std::size_t{};
        const auto indexText = line.substr(first + 1, second - first - 1);
        const auto [pointer, parseError] = std::from_chars(indexText.data(), indexText.data() + indexText.size(), index);
        const auto sourceHash = std::string(line.substr(second + 1, third - second - 1));
        const auto outputHash = std::string(line.substr(third + 1));
        if (parseError != std::errc{} || pointer != indexText.data() + indexText.size() || index >= maximumEntryCount ||
            sourceHash.empty() || outputHash.empty())
        {
            result.errors.push_back("Invalid macro-page completion journal line " + std::to_string(lineNumber) + ".");
            continue;
        }
        if (!result.records.emplace(index, MacroPageInjectionCompletion{index, sourceHash, outputHash}).second)
            result.errors.push_back("Duplicate macro-page completion index " + std::to_string(index) + ".");
    }
    return result;
}

bool publishMacroPageInjectionPresets(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& outputDirectory,
                                      std::string& error)
{
    if (makeMacroPageInjectionWorkPaths(outputDirectory).root.lexically_normal() != paths.root.lexically_normal())
    {
        error = "Refusing to publish from an unexpected macro-page work directory.";
        return false;
    }
    auto filesystemError = std::error_code{};
    const auto outputStatus = std::filesystem::symlink_status(outputDirectory, filesystemError);
    if (outputStatus.type() != std::filesystem::file_type::not_found)
    {
        error = "Macro-page output directory already exists: " + outputDirectory.string();
        return false;
    }
    filesystemError.clear();
    if (!pathStatusIsDirectoryNoFollow(paths.presets, error))
        return false;
    std::filesystem::rename(paths.presets, outputDirectory, filesystemError);
    if (filesystemError)
    {
        error = "Could not publish macro-page preset tree at " + outputDirectory.string() + ": " + filesystemError.message();
        return false;
    }
    return true;
}

std::optional<std::string> sha256MacroPageInjectionFile(const std::filesystem::path& path, std::string& error)
{
    error.clear();
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "Cannot hash a missing, non-regular, or symlinked file: " + path.string();
        return std::nullopt;
    }

#if JUCE_WINDOWS
    auto stream = juce::File(juce::String(path.c_str())).createInputStream();
#else
    auto stream = juce::File(juce::String::fromUTF8(path.c_str())).createInputStream();
#endif
    if (stream == nullptr || !stream->openedOk())
    {
        error = "Could not open file for SHA-256 hashing: " + path.string();
        return std::nullopt;
    }
    return juce::SHA256(*stream).toHexString().toStdString();
}

bool writeMacroPageInjectionRuntimeFile(const std::filesystem::path& path, const std::string_view text, std::string& error)
{
    error.clear();
    return replaceTextAtomically(path, text, error);
}

bool readMacroPageInjectionReceiptFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    error.clear();
    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, filesystemError);
    if (status.type() == std::filesystem::file_type::not_found ||
        filesystemError == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        text.clear();
        return true;
    }
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "Macro-page receipt is not a regular non-symlink file: " + path.string();
        return false;
    }
    return readTextFile(path, text, error);
}

bool removeMacroPageInjectionStagedPreset(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& relativePath,
                                          std::string& error)
{
    error.clear();
    if (!isSafeRelativePath(relativePath))
    {
        error = "Refusing to remove an unsafe macro-page staging path: " + relativePath.string();
        return false;
    }
    const auto target = (paths.presets / relativePath).lexically_normal();
    if (target.parent_path().empty() || target == paths.presets.lexically_normal())
    {
        error = "Refusing to remove an invalid macro-page staging path: " + target.string();
        return false;
    }

    auto filesystemError = std::error_code{};
    const auto metadataTemporary = makeVstPresetInfoReplacementTemporaryPath(target);
    const auto temporaryStatus = std::filesystem::symlink_status(metadataTemporary, filesystemError);
    if (temporaryStatus.type() != std::filesystem::file_type::not_found)
    {
        if (filesystemError || std::filesystem::is_symlink(temporaryStatus) || !std::filesystem::is_regular_file(temporaryStatus) ||
            !std::filesystem::remove(metadataTemporary, filesystemError) || filesystemError)
        {
            error = "Refusing to remove an unexpected macro-page metadata sidecar: " + metadataTemporary.string();
            return false;
        }
    }
    filesystemError.clear();

    const auto status = std::filesystem::symlink_status(target, filesystemError);
    if (status.type() == std::filesystem::file_type::not_found ||
        filesystemError == std::make_error_code(std::errc::no_such_file_or_directory))
        return true;
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
    {
        error = "Refusing to remove an unexpected macro-page staging entry: " + target.string();
        return false;
    }
    if (!std::filesystem::remove(target, filesystemError) || filesystemError)
    {
        error = "Could not remove incomplete macro-page staging preset " + target.string() + ": " + filesystemError.message();
        return false;
    }
    return true;
}

bool cleanupMacroPageInjectionWorkDirectory(const MacroPageInjectionWorkPaths& paths, const std::filesystem::path& outputDirectory,
                                            std::string& error)
{
    error.clear();
    if (makeMacroPageInjectionWorkPaths(outputDirectory).root.lexically_normal() != paths.root.lexically_normal())
    {
        error = "Refusing to clean an unexpected macro-page work directory.";
        return false;
    }

    auto filesystemError = std::error_code{};
    const auto status = std::filesystem::symlink_status(paths.root, filesystemError);
    if (status.type() == std::filesystem::file_type::not_found ||
        filesystemError == std::make_error_code(std::errc::no_such_file_or_directory))
        return true;
    if (filesystemError || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
    {
        error = "Refusing to clean a non-directory or symlinked macro-page work path: " + paths.root.string();
        return false;
    }
    std::filesystem::remove_all(paths.root, filesystemError);
    if (filesystemError)
    {
        error = "Could not clean macro-page work directory " + paths.root.string() + ": " + filesystemError.message();
        return false;
    }
    return true;
}

} // namespace halionbridge::detail
