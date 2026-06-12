#include "halionbridge/Bridge.h"

#include <algorithm>
#include <cctype>

namespace halionbridge
{

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

    auto skipComment = [&text](size_t& i)
    {
        if (i + 3 < text.size() && text[i] == '-' && text[i + 1] == '-' && text[i + 2] == '[' && text[i + 3] == '[')
        {
            i += 4;
            while (i + 1 < text.size() && !(text[i] == ']' && text[i + 1] == ']'))
                ++i;

            if (i + 1 < text.size())
                ++i;

            return true;
        }

        if (i + 1 < text.size() && text[i] == '-' && text[i + 1] == '-')
        {
            i += 2;
            while (i < text.size() && text[i] != '\n')
                ++i;

            return true;
        }

        return false;
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
                value.push_back(stringCharacter);
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
