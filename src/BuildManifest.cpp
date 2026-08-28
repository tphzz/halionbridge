#include "BuildManifest.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <span>
#include <system_error>

namespace halionbridge::detail
{
namespace
{

constexpr auto kMaximumManifestBytes = std::streamoff{4 * 1024 * 1024};

BuildManifestParseResult parseFailure(std::string code, std::string message)
{
    return BuildManifestParseResult{std::nullopt, BuildManifestError{std::move(code), std::move(message)}};
}

std::optional<BuildManifestError> failure(std::string code, std::string message)
{
    return BuildManifestError{std::move(code), std::move(message)};
}

std::string lowerCase(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}

bool isSafeManifestDirectory(const std::string_view text, std::filesystem::path& path)
{
    if (text.empty() || text.contains('\\') || text.contains('\0'))
        return false;

    path = std::filesystem::path(std::string(text));
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() || path.filename().empty())
        return false;

    for (const auto& component : path)
    {
        if (component.empty() || component == "." || component == "..")
            return false;
    }

    return path.lexically_normal() == path;
}

bool pathExistsNoFollow(const std::filesystem::path& path, std::error_code& error)
{
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found || error == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        error.clear();
        return false;
    }
    return !error;
}

void removeCreatedDirectories(const std::span<const std::filesystem::path> directories)
{
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
    {
        auto error = std::error_code{};
        std::filesystem::remove(*it, error);
    }
}

std::optional<BuildManifestError> readManifest(const std::filesystem::path& path, std::string& text)
{
    auto stream = std::ifstream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return failure("manifest-read", "Could not open build manifest: " + path.string());

    const auto size = stream.tellg();
    if (size < 0 || size > kMaximumManifestBytes)
        return failure("manifest-size", "Build manifest has an invalid or excessive size: " + path.string());

    text.resize(static_cast<size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!text.empty())
        stream.read(text.data(), size);
    if (!stream)
        return failure("manifest-read", "Could not read build manifest: " + path.string());

    return std::nullopt;
}

} // namespace

BuildManifestParseResult parseBuildManifest(const std::string_view jsonText)
{
    auto root = juce::var{};
    const auto parseResult = juce::JSON::parse(juce::String::fromUTF8(jsonText.data(), static_cast<int>(jsonText.size())), root);
    if (parseResult.failed())
        return parseFailure("manifest-json", "Build manifest is not valid JSON: " + parseResult.getErrorMessage().toStdString());

    const auto* object = root.getDynamicObject();
    if (object == nullptr)
        return parseFailure("manifest-root", "Build manifest root must be a JSON object.");

    const auto schemaVersion = object->getProperty("schema_version");
    if ((!schemaVersion.isInt() && !schemaVersion.isInt64()) || static_cast<int64_t>(schemaVersion) != 1)
        return parseFailure("manifest-schema", "Build manifest schema_version must be the integer 1.");

    const auto outputDirectories = object->getProperty("output_directories");
    const auto* directoryArray = outputDirectories.getArray();
    if (directoryArray == nullptr)
        return parseFailure("manifest-output-directories", "Build manifest output_directories must be an array.");

    auto manifest = BuildManifest{};
    manifest.outputDirectories.reserve(static_cast<size_t>(directoryArray->size()));
    auto seenDirectories = std::set<std::string>{};
    for (const auto& value : *directoryArray)
    {
        if (!value.isString())
            return parseFailure("manifest-output-directory", "Every build manifest output directory must be a string.");

        const auto text = value.toString().toStdString();
        auto path = std::filesystem::path{};
        if (!isSafeManifestDirectory(text, path))
            return parseFailure("manifest-output-directory",
                                "Build manifest output directories must be normalized relative paths using forward slashes: " + text);

        const auto key = lowerCase(path.generic_string());
        if (!seenDirectories.insert(key).second)
            return parseFailure("manifest-output-directory", "Duplicate build manifest output directory: " + text);

        manifest.outputDirectories.push_back(std::move(path));
    }

    return BuildManifestParseResult{std::move(manifest), std::nullopt};
}

std::optional<BuildManifestError> prepareBuildManifestOutputDirectories(const std::filesystem::path& buildDirectory,
                                                                        const std::filesystem::path& outputDirectory)
{
    const auto manifestPath = buildDirectory / kBuildManifestFileName;
    auto error = std::error_code{};
    if (!std::filesystem::exists(manifestPath, error))
    {
        if (error)
            return failure("manifest-read", "Could not inspect build manifest: " + manifestPath.string());
        return std::nullopt;
    }

    auto text = std::string{};
    if (auto readError = readManifest(manifestPath, text))
        return readError;

    auto parsed = parseBuildManifest(text);
    if (!parsed.succeeded())
        return parsed.error;

    if (!std::filesystem::is_directory(outputDirectory, error) || error)
        return failure("manifest-output-root", "Build output root is not a directory: " + outputDirectory.string());

    for (const auto& relativeDirectory : parsed.manifest->outputDirectories)
    {
        auto current = outputDirectory;
        for (const auto& component : relativeDirectory)
        {
            current /= component;
            error.clear();
            if (!pathExistsNoFollow(current, error))
            {
                if (error)
                    return failure("manifest-output-directory", "Could not inspect build output directory: " + current.string());
                continue;
            }

            const auto status = std::filesystem::symlink_status(current, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
                return failure("manifest-output-directory", "Build output path is not a usable directory: " + current.string());
        }
    }

    auto createdDirectories = std::vector<std::filesystem::path>{};
    for (const auto& relativeDirectory : parsed.manifest->outputDirectories)
    {
        auto current = outputDirectory;
        for (const auto& component : relativeDirectory)
        {
            current /= component;
            error.clear();
            if (pathExistsNoFollow(current, error))
            {
                if (error)
                {
                    removeCreatedDirectories(createdDirectories);
                    return failure("manifest-output-directory", "Could not inspect build output directory: " + current.string());
                }
                continue;
            }

            error.clear();
            if (!std::filesystem::create_directory(current, error) || error)
            {
                removeCreatedDirectories(createdDirectories);
                return failure("manifest-output-directory", "Could not create build output directory: " + current.string());
            }
            createdDirectories.push_back(current);
        }
    }

    return std::nullopt;
}

} // namespace halionbridge::detail
