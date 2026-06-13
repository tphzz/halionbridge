#include "halionbridge_converters/BuildDirectoryEmitter.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

namespace halionbridge::converters
{
namespace
{

constexpr const char* kBuildFileName = "halionbridge_build.lua";

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

    std::error_code error;
    if (!std::filesystem::exists(request.outputDirectory, error))
        std::filesystem::create_directories(request.outputDirectory, error);

    if (error || !std::filesystem::is_directory(request.outputDirectory, error))
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "output-directory",
                                               "Could not create output directory: " + request.outputDirectory.string()));
        return result;
    }

    if (request.scripts.empty())
    {
        result.diagnostics.push_back(makeError(request.outputDirectory, "no-scripts", "No Lua build scripts were generated."));
        return result;
    }

    std::set<std::string> seenModuleNames;
    std::set<std::string> seenFileNames;
    for (const auto& script : request.scripts)
    {
        if (script.moduleName.empty())
        {
            result.diagnostics.push_back(
                makeError(request.outputDirectory, "invalid-module-name", "Generated Lua script module names must not be empty."));
            return result;
        }

        const auto moduleKey = normalizedKey(script.moduleName);
        if (!seenModuleNames.insert(moduleKey).second)
        {
            result.diagnostics.push_back(
                makeError(request.outputDirectory, "duplicate-module-name", "Duplicate generated Lua module name: " + script.moduleName));
            return result;
        }

        const auto scriptFileName = std::filesystem::path(script.fileName);
        if (!isFlatRelativeFilename(scriptFileName))
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "invalid-script-filename",
                                                   "Generated Lua script filenames must be flat relative filenames: " + script.fileName));
            return result;
        }

        const auto fileKey = normalizedKey(scriptFileName.generic_string());
        if (!seenFileNames.insert(fileKey).second)
        {
            result.diagnostics.push_back(makeError(request.outputDirectory, "duplicate-script-filename",
                                                   "Duplicate generated Lua script filename: " + script.fileName));
            return result;
        }
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

    std::ostringstream buildFileText;
    buildFileText << "return {\n";
    for (const auto& script : request.scripts)
        buildFileText << "    " << luaQuotedString(script.moduleName) << ",\n";
    buildFileText << "}\n";

    if (!writeTextFile(result.buildFile, buildFileText.str()))
    {
        result.diagnostics.push_back(makeError(result.buildFile, "write-failed", "Failed to write " + result.buildFile.string()));
        return result;
    }

    for (const auto& script : request.scripts)
    {
        const auto path = request.outputDirectory / script.fileName;
        if (!writeTextFile(path, script.luaSource))
        {
            result.diagnostics.push_back(makeError(path, "write-failed", "Failed to write " + path.string()));
            return result;
        }

        result.generatedLuaFiles.push_back(path);
    }

    result.succeeded = true;
    return result;
}

} // namespace halionbridge::converters
