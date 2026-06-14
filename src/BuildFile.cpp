#include "BuildFile.h"

#include "halionbridge/Bridge.h"
#include "PathUtils.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

namespace halionbridge
{
namespace
{

constexpr const char* kBuildFileName = "halionbridge_build.lua";
constexpr const char* kRuntimeModuleFileName = "halionbridge_runtime.lua";
constexpr const char* kBuilderModuleFileName = "halionbridge_builder.lua";
constexpr const char* kBuilderBootstrapFileName = "builder_bootstrap.lua";
constexpr const char* kSfzHelperModuleFileName = "halionbridge-sfz.lua";

bool isInfrastructureLuaFile(const juce::String& fileName)
{
    return fileName.equalsIgnoreCase(kBuildFileName) || fileName.equalsIgnoreCase(kRuntimeModuleFileName) ||
           fileName.equalsIgnoreCase(kBuilderModuleFileName) || fileName.equalsIgnoreCase(kBuilderBootstrapFileName) ||
           fileName.equalsIgnoreCase(kSfzHelperModuleFileName);
}

std::string luaQuotedString(const juce::String& text)
{
    auto quoted = std::string("\"");

    for (const auto c : text.toStdString())
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

} // namespace

namespace detail
{

std::vector<juce::File> findTopLevelLuaBuildScripts(const juce::File& directory)
{
    juce::Array<juce::File> files;
    directory.findChildFiles(files, juce::File::findFiles, false, "*.lua");

    std::vector<juce::File> scripts;
    scripts.reserve(static_cast<size_t>(files.size()));

    for (const auto& file : files)
    {
        if (!isInfrastructureLuaFile(file.getFileName()))
            scripts.push_back(file);
    }

    std::sort(scripts.begin(), scripts.end(),
              [](const juce::File& lhs, const juce::File& rhs) { return lhs.getFileName() < rhs.getFileName(); });
    return scripts;
}

bool hasTopLevelLuaBuildScripts(const juce::File& directory)
{
    return !findTopLevelLuaBuildScripts(directory).empty();
}

BuildFileGenerationResult generateBuildFile(const juce::File& directory, const bool overwriteExisting)
{
    auto result = BuildFileGenerationResult{};
    result.buildFile = directory.getChildFile(kBuildFileName);

    if (!directory.isDirectory())
    {
        result.message = juce::String("Build directory does not exist at ") + directory.getFullPathName();
        return result;
    }

    if (result.buildFile.existsAsFile() && !overwriteExisting)
    {
        result.message = result.buildFile.getFullPathName() + " already exists. Use --overwrite to replace it.";
        return result;
    }

    const auto scripts = findTopLevelLuaBuildScripts(directory);
    if (scripts.empty())
    {
        result.message = juce::String("No top-level Lua build script files were found in ") + directory.getFullPathName();
        return result;
    }

    std::ostringstream text;
    text << "return {\n";
    for (const auto& script : scripts)
    {
        const auto moduleName = script.getFileName();
        result.moduleNames.push_back(moduleName);
        text << "    " << luaQuotedString(moduleName) << ",\n";
    }
    text << "}\n";

    if (!result.buildFile.replaceWithText(juce::String(text.str()), false, false, "\n"))
    {
        result.message = "Failed to write " + result.buildFile.getFullPathName();
        result.moduleNames.clear();
        return result;
    }

    result.succeeded = true;
    result.message = "Generated " + result.buildFile.getFullPathName();
    return result;
}

InitCommandResult runInitCommand(const juce::StringArray& args)
{
    auto commandResult = InitCommandResult{};
    std::optional<juce::File> buildDirectory;
    auto overwrite = false;

    for (int i = 1; i < args.size(); ++i)
    {
        const auto arg = args[i];

        if (arg == "--overwrite")
        {
            overwrite = true;
        }
        else if (arg.startsWith("-"))
        {
            commandResult.message = "Unknown init argument: " + arg + "\nRun halionbridge --help to see available options.";
            return commandResult;
        }
        else
        {
            if (buildDirectory)
            {
                commandResult.message = "Provide exactly one build directory for halionbridge init.";
                return commandResult;
            }

            buildDirectory = normalizeCliPath(arg);
        }
    }

    if (!buildDirectory)
    {
        commandResult.message = "halionbridge init requires a build directory.";
        return commandResult;
    }

    const auto generationResult = generateBuildFile(*buildDirectory, overwrite);
    commandResult.moduleNames = generationResult.moduleNames;
    commandResult.message = generationResult.message;

    if (!generationResult.succeeded)
        return commandResult;

    commandResult.exitCode = 0;
    commandResult.warning = "Review " + generationResult.buildFile.getFullPathName() +
                            " before running the build. halionbridge init lists every top-level non-infrastructure .lua file, so remove "
                            "helper modules that are required by build scripts but are not build entrypoints.";
    return commandResult;
}

} // namespace detail

std::vector<std::string> Bridge::parseBuildFileModuleNames(std::string_view luaText)
{
    auto trim = [](std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string();

        const auto last = value.find_last_not_of(" \t\r\n");
        return value.substr(first, last - first + 1);
    };

    std::vector<std::string> names;
    const auto text = std::string(luaText);

    auto isIdentifierCharacter = [](const char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };

    auto findLongBracketEnd = [&text](const size_t openBracketIndex) -> std::optional<std::pair<size_t, size_t>>
    {
        if (openBracketIndex >= text.size() || text[openBracketIndex] != '[')
            return std::nullopt;

        size_t i = openBracketIndex + 1;
        while (i < text.size() && text[i] == '=')
            ++i;

        if (i >= text.size() || text[i] != '[')
            return std::nullopt;

        const auto equalsCount = i - openBracketIndex - 1;
        auto closing = std::string("]");
        closing.append(equalsCount, '=');
        closing += "]";

        const auto contentStart = i + 1;
        const auto closingStart = text.find(closing, contentStart);
        if (closingStart == std::string::npos)
            return std::make_pair(text.size(), text.size());

        return std::make_pair(closingStart, closingStart + closing.size() - 1);
    };

    auto skipQuotedString = [&text](size_t& i)
    {
        const auto quote = text[i];
        bool escaped = false;

        for (++i; i < text.size(); ++i)
        {
            const auto c = text[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }

            if (c == '\\')
            {
                escaped = true;
                continue;
            }

            if (c == quote)
                break;
        }
    };

    auto skipComment = [&text, &findLongBracketEnd](size_t& i)
    {
        if (i + 1 < text.size() && text[i] == '-' && text[i + 1] == '-')
        {
            if (const auto longBracketEnd = findLongBracketEnd(i + 2))
            {
                i = longBracketEnd->second;
                return true;
            }

            i += 2;
            while (i < text.size() && text[i] != '\n')
                ++i;

            return true;
        }

        return false;
    };

    auto decodeEscapedCharacter = [](const char c)
    {
        switch (c)
        {
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case '\\':
            return '\\';
        case '"':
            return '"';
        case '\'':
            return '\'';
        default:
            return c;
        }
    };

    auto findReturnedTableStart = [&]() -> std::optional<size_t>
    {
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (skipComment(i))
                continue;

            if (text[i] == '"' || text[i] == '\'')
            {
                skipQuotedString(i);
                continue;
            }

            if (text.compare(i, 6, "return") != 0)
                continue;

            const auto beforeIsIdentifier = i > 0 && isIdentifierCharacter(text[i - 1]);
            const auto afterIsIdentifier = i + 6 < text.size() && isIdentifierCharacter(text[i + 6]);
            if (beforeIsIdentifier || afterIsIdentifier)
                continue;

            for (i += 6; i < text.size(); ++i)
            {
                if (skipComment(i))
                    continue;

                if (text[i] == '"' || text[i] == '\'')
                {
                    skipQuotedString(i);
                    continue;
                }

                if (text[i] == '{')
                    return i;
            }

            return std::nullopt;
        }

        return std::nullopt;
    };

    auto isTopLevelListEntry = [&text](const size_t quoteIndex)
    {
        auto i = quoteIndex;
        while (i > 0)
        {
            --i;
            if (std::isspace(static_cast<unsigned char>(text[i])))
                continue;

            return text[i] == '{' || text[i] == ',';
        }

        return false;
    };

    const auto tableStart = findReturnedTableStart();
    if (!tableStart)
        return names;

    auto depth = 0;
    for (size_t i = *tableStart; i < text.size(); ++i)
    {
        if (skipComment(i))
            continue;

        const auto c = text[i];
        if (c == '{')
        {
            ++depth;
            continue;
        }

        if (c == '}')
        {
            if (depth == 1)
                break;

            if (depth > 0)
                --depth;

            continue;
        }

        if (c != '"' && c != '\'')
            continue;

        if (depth != 1 || !isTopLevelListEntry(i))
        {
            skipQuotedString(i);
            continue;
        }

        std::string value;
        bool escaped = false;
        const auto quote = c;

        for (++i; i < text.size(); ++i)
        {
            const auto stringCharacter = text[i];
            if (escaped)
            {
                value.push_back(decodeEscapedCharacter(stringCharacter));
                escaped = false;
                continue;
            }

            if (stringCharacter == '\\')
            {
                escaped = true;
                continue;
            }

            if (stringCharacter == quote)
                break;

            value.push_back(stringCharacter);
        }

        auto name = trim(std::move(value));
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }

    return names;
}

} // namespace halionbridge
