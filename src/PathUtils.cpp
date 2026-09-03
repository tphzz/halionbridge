#include "PathUtils.h"

#include <algorithm>
#include <utility>

namespace halionbridge::detail
{

juce::String toJuceString(const std::filesystem::path& path)
{
#if JUCE_WINDOWS
    return juce::String(path.wstring().c_str());
#else
    return juce::String(path.string());
#endif
}

juce::String toJuceString(std::string_view text)
{
    return juce::String(std::string(text));
}

juce::File toJuceFile(const std::filesystem::path& path)
{
    return juce::File(toJuceString(path));
}

std::filesystem::path toFilesystemAccessPath(const std::filesystem::path& path)
{
#if JUCE_WINDOWS
    if (!path.is_absolute())
        return path;

    auto text = path.native();
    std::replace(text.begin(), text.end(), L'/', L'\\');
    if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\"))
        return std::filesystem::path(std::move(text));
    if (text.starts_with(L"\\\\"))
        return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
    return std::filesystem::path(L"\\\\?\\" + text);
#else
    return path;
#endif
}

std::filesystem::path toStdPath(const juce::File& file)
{
#if JUCE_WINDOWS
    return std::filesystem::path(std::wstring(file.getFullPathName().toWideCharPointer()));
#else
    return std::filesystem::path(file.getFullPathName().toStdString());
#endif
}

std::string toStdString(const juce::String& text)
{
    return text.toStdString();
}

juce::File normalizeCliPath(juce::String path)
{
    path = path.trim().unquoted();

    while (path.length() > 3 && (path.endsWithChar('\\') || path.endsWithChar('/')))
        path = path.dropLastCharacters(1);

    if (juce::File::isAbsolutePath(path))
        return juce::File(path);

    return juce::File::getCurrentWorkingDirectory().getChildFile(path);
}

} // namespace halionbridge::detail
