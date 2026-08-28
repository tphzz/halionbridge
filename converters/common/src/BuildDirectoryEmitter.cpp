#include "halionbridge_converters/BuildDirectoryEmitter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace halionbridge::converters
{
namespace
{

constexpr const char* kBuildFileName = "halionbridge_build.lua";

Diagnostic makeError(const std::filesystem::path& source, std::string code, std::string message)
{
    return Diagnostic{DiagnosticLevel::error, source, 0, std::move(code), std::move(message)};
}

std::vector<std::byte> textBytes(const std::string_view text)
{
    auto result = std::vector<std::byte>{};
    result.reserve(text.size());
    for (const auto character : text)
        result.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    return result;
}

bool writeBinaryFile(const std::filesystem::path& path, const std::span<const std::byte> data)
{
    auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    if (!data.empty())
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return stream.good();
}

std::filesystem::path makeTransactionPath(const std::filesystem::path& directory, const char* label, const size_t index,
                                          const std::filesystem::path& targetFileName)
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    for (auto attempt = 0; attempt < 1000; ++attempt)
    {
        auto name = std::ostringstream{};
        name << ".halionbridge-" << label << "-" << now << "-" << index << "-" << attempt << "-"
             << targetFileName.filename().generic_string();

        auto path = directory / name.str();
        auto error = std::error_code{};
        if (!std::filesystem::exists(path, error) && !error)
            return path;
    }

    return {};
}

std::string normalizedKey(std::string text)
{
    std::replace(text.begin(), text.end(), '\\', '/');
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isFlatRelativeFilename(const std::filesystem::path& fileName)
{
    if (fileName.empty() || fileName.is_absolute() || fileName.has_root_name() || fileName.has_root_directory() ||
        fileName.has_parent_path())
    {
        return false;
    }

    const auto generic = fileName.generic_string();
    if (generic.empty() || generic == "." || generic == ".." || generic.find('/') != std::string::npos ||
        generic.find('\\') != std::string::npos)
    {
        return false;
    }

    const auto normalized = fileName.lexically_normal();
    return normalized == fileName && normalized.filename() == fileName;
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory() || path.filename().empty())
        return false;

    for (const auto& component : path)
    {
        if (component.empty() || component == "." || component == "..")
            return false;
    }

    return path.lexically_normal() == path;
}

bool hasLuaSuffix(const std::filesystem::path& fileName)
{
    auto extension = fileName.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".lua";
}

std::string normalizedModuleNameKey(std::string text)
{
    auto key = normalizedKey(std::move(text));
    constexpr auto luaSuffix = std::string_view{".lua"};
    if (key.size() > luaSuffix.size() && key.ends_with(luaSuffix))
        key.resize(key.size() - luaSuffix.size());
    return key;
}

bool generatedPathsConflict(const std::string_view first, const std::string_view second)
{
    if (first == second)
        return true;

    const auto isParent = [](const std::string_view parent, const std::string_view child)
    { return child.size() > parent.size() && child.starts_with(parent) && child[parent.size()] == '/'; };
    return isParent(first, second) || isParent(second, first);
}

bool addGeneratedPath(const std::filesystem::path& path, std::set<std::string>& seenPaths)
{
    auto key = normalizedKey(path.generic_string());
    const auto next = seenPaths.lower_bound(key);
    if (next != seenPaths.end() && generatedPathsConflict(*next, key))
        return false;
    if (next != seenPaths.begin() && generatedPathsConflict(*std::prev(next), key))
        return false;

    seenPaths.emplace_hint(next, std::move(key));
    return true;
}

bool pathExistsNoFollow(const std::filesystem::path& path, std::error_code& error)
{
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found || error == std::make_error_code(std::errc::no_such_file_or_directory))
    {
        error.clear();
        return false;
    }
    return !error && status.type() != std::filesystem::file_type::not_found;
}

struct PendingWrite
{
    std::filesystem::path relativePath;
    std::filesystem::path target;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    std::vector<std::byte> data;
    bool isLuaFile = false;
    bool includeInGeneratedFiles = false;
    bool hadExistingTarget = false;
    bool committed = false;
};

void cleanupTransactionFiles(std::span<const PendingWrite> writes)
{
    for (const auto& write : writes)
    {
        auto error = std::error_code{};
        if (!write.temporary.empty())
            std::filesystem::remove(write.temporary, error);
        if (!write.backup.empty())
            std::filesystem::remove(write.backup, error);
    }
}

void rollbackWrites(std::span<PendingWrite> writes)
{
    for (auto it = writes.rbegin(); it != writes.rend(); ++it)
    {
        auto error = std::error_code{};
        if (it->committed)
            std::filesystem::remove(it->target, error);

        error.clear();
        if (it->hadExistingTarget && !it->backup.empty() && pathExistsNoFollow(it->backup, error))
        {
            error.clear();
            std::filesystem::rename(it->backup, it->target, error);
        }

        error.clear();
        if (!it->temporary.empty())
            std::filesystem::remove(it->temporary, error);
    }
}

void removeCreatedDirectories(std::span<const std::filesystem::path> directories)
{
    for (auto it = directories.rbegin(); it != directories.rend(); ++it)
    {
        auto error = std::error_code{};
        std::filesystem::remove(*it, error);
    }
}

bool inspectParentDirectories(const std::filesystem::path& outputDirectory, const std::filesystem::path& relativePath,
                              std::filesystem::path& invalidParent)
{
    auto current = outputDirectory;
    for (const auto& component : relativePath.parent_path())
    {
        current /= component;
        auto error = std::error_code{};
        const auto exists = pathExistsNoFollow(current, error);
        if (error)
        {
            invalidParent = current;
            return false;
        }

        if (!exists)
            continue;

        const auto status = std::filesystem::symlink_status(current, error);
        if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
        {
            invalidParent = current;
            return false;
        }
    }

    return true;
}

bool createParentDirectories(const std::filesystem::path& outputDirectory, const std::filesystem::path& relativePath,
                             std::vector<std::filesystem::path>& createdDirectories, std::filesystem::path& failedDirectory)
{
    auto current = outputDirectory;
    for (const auto& component : relativePath.parent_path())
    {
        current /= component;
        auto error = std::error_code{};
        if (pathExistsNoFollow(current, error))
        {
            if (error)
            {
                failedDirectory = current;
                return false;
            }
            continue;
        }

        error.clear();
        if (!std::filesystem::create_directory(current, error) || error)
        {
            failedDirectory = current;
            return false;
        }
        createdDirectories.push_back(current);
    }

    return true;
}

} // namespace

std::string luaQuotedString(const std::string_view text)
{
    auto quoted = std::string("\"");

    for (const auto c : text)
    {
        switch (c)
        {
        case '\\':
            quoted += "\\\\";
            break;
        case '"':
            quoted += "\\\"";
            break;
        case '\r':
            quoted += "\\r";
            break;
        case '\n':
            quoted += "\\n";
            break;
        default:
            quoted.push_back(c);
            break;
        }
    }

    quoted += "\"";
    return quoted;
}

BuildDirectoryResult writeBuildDirectory(const BuildDirectoryRequest& request)
{
    auto result = BuildDirectoryResult{};
    result.buildFile = request.outputDirectory / kBuildFileName;

    if (request.outputDirectory.empty())
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "output-missing", "Output directory is not set."));
        return result;
    }

    if (request.scripts.empty())
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "no-scripts", "No Lua build scripts were generated."));
        return result;
    }

    auto buildEntrypointCount = 0;
    auto seenModuleNames = std::set<std::string>{};
    auto seenScriptFileNames = std::set<std::string>{};
    auto seenGeneratedPaths = std::set<std::string>{normalizedKey(kBuildFileName)};
    for (const auto& script : request.scripts)
    {
        const auto scriptFileName = std::filesystem::path(script.fileName);
        if (!isFlatRelativeFilename(scriptFileName) || !hasLuaSuffix(scriptFileName))
        {
            result.diagnostics.push_back(
                makeError(request.outputDirectory, "invalid-script-filename",
                          "Generated Lua script filenames must be flat relative .lua filenames: " + script.fileName));
            return result;
        }

        const auto fileKey = normalizedKey(scriptFileName.generic_string());
        if (!seenScriptFileNames.insert(fileKey).second)
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "duplicate-script-filename",
                                                   "Duplicate generated Lua script filename: " + script.fileName));
            return result;
        }

        if (!addGeneratedPath(scriptFileName, seenGeneratedPaths))
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "duplicate-generated-path",
                                                   "Generated output path collides with another output: " + script.fileName));
            return result;
        }

        if (script.role != GeneratedLuaFileRole::buildEntrypoint)
            continue;

        ++buildEntrypointCount;
        if (script.moduleName.empty())
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "invalid-module-name",
                                                   "Generated Lua build entrypoint module names must not be empty."));
            return result;
        }

        const auto moduleKey = normalizedModuleNameKey(script.moduleName);
        if (!seenModuleNames.insert(moduleKey).second)
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "duplicate-module-name",
                                                   "Duplicate generated Lua build entrypoint module name: " + script.moduleName));
            return result;
        }
    }

    for (const auto& file : request.files)
    {
        if (!isSafeRelativePath(file.relativePath))
        {
            result.diagnostics.push_back(
                makeError(request.outputDirectory, "invalid-generated-path",
                          "Generated file paths must stay below the build directory: " + file.relativePath.generic_string()));
            return result;
        }

        if (!addGeneratedPath(file.relativePath, seenGeneratedPaths))
        {
            result.diagnostics.push_back(
                makeError(request.outputDirectory, "duplicate-generated-path",
                          "Generated output path collides with another output: " + file.relativePath.generic_string()));
            return result;
        }
    }

    if (buildEntrypointCount == 0)
    {
        result.diagnostics.push_back(
            makeError(request.outputDirectory, "no-build-entrypoints", "No Lua build entrypoint files were generated."));
        return result;
    }

    auto error = std::error_code{};
    if (!std::filesystem::exists(request.outputDirectory, error))
        std::filesystem::create_directories(request.outputDirectory, error);

    if (error || !std::filesystem::is_directory(request.outputDirectory, error))
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "output-directory",
                                               "Could not create output directory: " + request.outputDirectory.string()));
        return result;
    }

    auto buildFileText = std::ostringstream{};
    buildFileText << "return {\n";
    for (const auto& script : request.scripts)
    {
        if (script.role == GeneratedLuaFileRole::buildEntrypoint)
            buildFileText << "    " << luaQuotedString(script.moduleName) << ",\n";
    }
    buildFileText << "}\n";

    auto pendingWrites = std::vector<PendingWrite>{};
    pendingWrites.reserve(request.scripts.size() + request.files.size() + 1);
    pendingWrites.push_back(PendingWrite{kBuildFileName, result.buildFile, {}, {}, textBytes(buildFileText.str()), false, false});
    for (const auto& script : request.scripts)
    {
        pendingWrites.push_back(
            PendingWrite{script.fileName, request.outputDirectory / script.fileName, {}, {}, textBytes(script.luaSource), true, true});
    }
    for (const auto& file : request.files)
    {
        pendingWrites.push_back(
            PendingWrite{file.relativePath, request.outputDirectory / file.relativePath, {}, {}, file.data, false, true});
    }

    for (const auto& write : pendingWrites)
    {
        auto invalidParent = std::filesystem::path{};
        if (!inspectParentDirectories(request.outputDirectory, write.relativePath, invalidParent))
        {
            result.diagnostics.push_back(
                makeError(invalidParent, "not-directory", invalidParent.string() + " is not a usable output directory."));
            return result;
        }

        error.clear();
        const auto targetExists = pathExistsNoFollow(write.target, error);
        if (error)
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to inspect " + write.target.string()));
            return result;
        }

        if (targetExists && !request.overwrite)
        {
            result.diagnostics.push_back(
                makeError(write.target, "already-exists", write.target.string() + " already exists. Use --overwrite to replace it."));
            return result;
        }

        if (targetExists)
        {
            const auto status = std::filesystem::symlink_status(write.target, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))
            {
                result.diagnostics.push_back(
                    makeError(write.target, "not-regular-file", write.target.string() + " exists but is not a regular file."));
                return result;
            }
        }
    }

    for (size_t i = 0; i < pendingWrites.size(); ++i)
    {
        auto& write = pendingWrites[i];
        write.temporary = makeTransactionPath(request.outputDirectory, "tmp", i, write.target.filename());
        if (write.temporary.empty())
        {
            result.diagnostics.push_back(
                makeError(write.target, "write-failed", "Failed to reserve temporary path for " + write.target.string()));
            cleanupTransactionFiles(pendingWrites);
            return result;
        }

        if (!writeBinaryFile(write.temporary, write.data))
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to write " + write.target.string()));
            cleanupTransactionFiles(pendingWrites);
            return result;
        }
    }

    auto createdDirectories = std::vector<std::filesystem::path>{};
    for (const auto& write : pendingWrites)
    {
        auto failedDirectory = std::filesystem::path{};
        if (!createParentDirectories(request.outputDirectory, write.relativePath, createdDirectories, failedDirectory))
        {
            result.diagnostics.push_back(
                makeError(failedDirectory, "write-failed", "Failed to create output directory " + failedDirectory.string()));
            rollbackWrites(pendingWrites);
            removeCreatedDirectories(createdDirectories);
            return result;
        }
    }

    for (size_t i = 0; i < pendingWrites.size(); ++i)
    {
        auto& write = pendingWrites[i];
        error.clear();
        if (pathExistsNoFollow(write.target, error))
        {
            if (error)
            {
                result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to inspect " + write.target.string()));
                rollbackWrites(pendingWrites);
                removeCreatedDirectories(createdDirectories);
                return result;
            }

            write.hadExistingTarget = true;
            write.backup = makeTransactionPath(request.outputDirectory, "bak", i, write.target.filename());
            if (write.backup.empty())
            {
                result.diagnostics.push_back(
                    makeError(write.target, "write-failed", "Failed to reserve backup path for " + write.target.string()));
                rollbackWrites(pendingWrites);
                removeCreatedDirectories(createdDirectories);
                return result;
            }

            error.clear();
            std::filesystem::rename(write.target, write.backup, error);
            if (error)
            {
                result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to back up " + write.target.string()));
                rollbackWrites(pendingWrites);
                removeCreatedDirectories(createdDirectories);
                return result;
            }
        }
        else if (error)
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to inspect " + write.target.string()));
            rollbackWrites(pendingWrites);
            removeCreatedDirectories(createdDirectories);
            return result;
        }

        error.clear();
        std::filesystem::rename(write.temporary, write.target, error);
        if (error)
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to write " + write.target.string()));
            rollbackWrites(pendingWrites);
            removeCreatedDirectories(createdDirectories);
            return result;
        }

        write.committed = true;
    }

    cleanupTransactionFiles(pendingWrites);

    for (const auto& write : pendingWrites)
    {
        if (!write.includeInGeneratedFiles)
            continue;

        result.generatedFiles.push_back(write.target);
        if (write.isLuaFile)
            result.generatedLuaFiles.push_back(write.target);
    }

    result.succeeded = true;
    return result;
}

} // namespace halionbridge::converters
