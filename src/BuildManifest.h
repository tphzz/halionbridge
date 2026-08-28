#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace halionbridge::detail
{

inline constexpr auto kBuildManifestFileName = "halionbridge_build_manifest.json";

struct BuildManifest
{
    std::vector<std::filesystem::path> outputDirectories;
};

struct BuildManifestError
{
    std::string code;
    std::string message;
};

struct BuildManifestParseResult
{
    std::optional<BuildManifest> manifest;
    std::optional<BuildManifestError> error;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return manifest.has_value() && !error.has_value();
    }
};

BuildManifestParseResult parseBuildManifest(std::string_view jsonText);
std::optional<BuildManifestError> prepareBuildManifestOutputDirectories(const std::filesystem::path& buildDirectory,
                                                                        const std::filesystem::path& outputDirectory);

} // namespace halionbridge::detail
