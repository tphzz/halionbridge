#include "halionbridge_converters/BuildDirectoryEmitter.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace halionbridge::converters
{
namespace
{

constexpr const char* kBuildFileName = "halionbridge_build.lua";
constexpr const char* kSfzHelperFileName = "halionbridge-sfz.lua";

Diagnostic makeError(const std::filesystem::path& source, std::string code, std::string message)
{
    return Diagnostic{DiagnosticLevel::error, source, 0, std::move(code), std::move(message)};
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;

    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
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
        std::error_code error;
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

bool isReservedHelperEntrypointName(const GeneratedLuaScript& script)
{
    return normalizedKey(script.fileName) == kSfzHelperFileName || normalizedModuleNameKey(script.moduleName) == "halionbridge-sfz";
}

struct PendingWrite
{
    std::filesystem::path target;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    std::string text;
    bool includeInGeneratedFiles = false;
    bool hadExistingTarget = false;
    bool committed = false;
};

void cleanupTransactionFiles(std::span<const PendingWrite> writes)
{
    for (const auto& write : writes)
    {
        std::error_code error;
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
        std::error_code error;
        if (it->committed)
            std::filesystem::remove(it->target, error);

        if (it->hadExistingTarget && !it->backup.empty() && std::filesystem::exists(it->backup, error))
        {
            error.clear();
            std::filesystem::rename(it->backup, it->target, error);
        }

        error.clear();
        if (!it->temporary.empty())
            std::filesystem::remove(it->temporary, error);
    }
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
    std::set<std::string> seenModuleNames;
    std::set<std::string> seenFileNames;
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
        if (!seenFileNames.insert(fileKey).second)
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "duplicate-script-filename",
                                                   "Duplicate generated Lua script filename: " + script.fileName));
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

        if (isReservedHelperEntrypointName(script))
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "reserved-helper-entrypoint",
                                                   "Generated helper module must not be listed as a build entrypoint: " + script.fileName));
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

    if (buildEntrypointCount == 0)
    {
        result.diagnostics.push_back(
            makeError(request.outputDirectory, "no-build-entrypoints", "No Lua build entrypoint files were generated."));
        return result;
    }

    std::error_code error;
    if (!std::filesystem::exists(request.outputDirectory, error))
        std::filesystem::create_directories(request.outputDirectory, error);

    if (error || !std::filesystem::is_directory(request.outputDirectory, error))
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "output-directory",
                                               "Could not create output directory: " + request.outputDirectory.string()));
        return result;
    }

    std::vector<std::filesystem::path> outputFiles;
    outputFiles.reserve(request.scripts.size() + 1);
    outputFiles.push_back(result.buildFile);
    for (const auto& script : request.scripts)
        outputFiles.push_back(request.outputDirectory / script.fileName);

    if (!request.overwrite)
    {
        for (const auto& outputFile : outputFiles)
        {
            if (std::filesystem::exists(outputFile, error))
            {
                result.diagnostics.push_back(
                    makeError(outputFile, "already-exists", outputFile.string() + " already exists. Use --overwrite to replace it."));
                return result;
            }
        }
    }
    else
    {
        for (const auto& outputFile : outputFiles)
        {
            if (std::filesystem::exists(outputFile, error) && !std::filesystem::is_regular_file(outputFile, error))
            {
                result.diagnostics.push_back(
                    makeError(outputFile, "not-regular-file", outputFile.string() + " exists but is not a regular file."));
                return result;
            }
        }
    }

    std::ostringstream buildFileText;
    buildFileText << "return {\n";
    for (const auto& script : request.scripts)
    {
        if (script.role == GeneratedLuaFileRole::buildEntrypoint)
            buildFileText << "    " << luaQuotedString(script.moduleName) << ",\n";
    }
    buildFileText << "}\n";

    auto pendingWrites = std::vector<PendingWrite>();
    pendingWrites.reserve(request.scripts.size() + 1);
    pendingWrites.push_back(PendingWrite{result.buildFile, {}, {}, buildFileText.str(), false});
    for (const auto& script : request.scripts)
        pendingWrites.push_back(PendingWrite{request.outputDirectory / script.fileName, {}, {}, script.luaSource, true});

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

        if (!writeTextFile(write.temporary, write.text))
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to write " + write.target.string()));
            cleanupTransactionFiles(pendingWrites);
            return result;
        }
    }

    for (size_t i = 0; i < pendingWrites.size(); ++i)
    {
        auto& write = pendingWrites[i];
        if (std::filesystem::exists(write.target, error))
        {
            if (error)
            {
                result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to inspect " + write.target.string()));
                rollbackWrites(pendingWrites);
                return result;
            }

            write.hadExistingTarget = true;
            write.backup = makeTransactionPath(request.outputDirectory, "bak", i, write.target.filename());
            if (write.backup.empty())
            {
                result.diagnostics.push_back(
                    makeError(write.target, "write-failed", "Failed to reserve backup path for " + write.target.string()));
                rollbackWrites(pendingWrites);
                return result;
            }

            error.clear();
            std::filesystem::rename(write.target, write.backup, error);
            if (error)
            {
                result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to back up " + write.target.string()));
                rollbackWrites(pendingWrites);
                return result;
            }
        }
        else if (error)
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to inspect " + write.target.string()));
            rollbackWrites(pendingWrites);
            return result;
        }

        error.clear();
        std::filesystem::rename(write.temporary, write.target, error);
        if (error)
        {
            result.diagnostics.push_back(makeError(write.target, "write-failed", "Failed to write " + write.target.string()));
            rollbackWrites(pendingWrites);
            return result;
        }

        write.committed = true;
    }

    cleanupTransactionFiles(pendingWrites);

    for (const auto& write : pendingWrites)
    {
        if (write.includeInGeneratedFiles)
            result.generatedLuaFiles.push_back(write.target);
    }

    result.succeeded = true;
    return result;
}

} // namespace halionbridge::converters
