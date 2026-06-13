#include "ProgressMarkers.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace halionbridge::detail
{
namespace
{

constexpr const char* kCompactProgressMarkerPrefix = "hbp_";
constexpr const char* kLegacyProgressMarkerPrefix = "halionbridge_progress_";
constexpr const char* kPresetFileExtension = ".vstpreset";
constexpr int kDeleteRetryAttempts = 4;
constexpr int kDeleteRetryDelayMs = 25;

#if JUCE_WINDOWS
juce::String makeExtendedLengthPath(juce::String path)
{
    path = path.replaceCharacter('/', '\\');

    if (path.startsWith("\\\\?\\"))
        return path;

    if (path.startsWith("\\\\"))
        return "\\\\?\\UNC\\" + path.substring(2);

    return "\\\\?\\" + path;
}

bool fileExistsForProgressCleanup(const juce::File& file)
{
    const auto attributes = ::GetFileAttributesW(makeExtendedLengthPath(file.getFullPathName()).toWideCharPointer());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void findProgressMarkerFilesWithPattern(const juce::File& directory, const juce::String& pattern, std::vector<juce::File>& files)
{
    auto searchPath = directory.getChildFile(pattern).getFullPathName();
    WIN32_FIND_DATAW findData{};
    const auto handle = ::FindFirstFileW(makeExtendedLengthPath(searchPath).toWideCharPointer(), &findData);
    if (handle == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            files.push_back(directory.getChildFile(juce::String(findData.cFileName)));
    } while (::FindNextFileW(handle, &findData) != 0);

    ::FindClose(handle);
}
#else
bool fileExistsForProgressCleanup(const juce::File& file)
{
    return file.existsAsFile();
}
#endif

std::vector<juce::File> findProgressMarkerFiles(const juce::File& directory)
{
#if JUCE_WINDOWS
    std::vector<juce::File> sortedFiles;
    findProgressMarkerFilesWithPattern(directory, juce::String(kCompactProgressMarkerPrefix) + "*" + kPresetFileExtension, sortedFiles);
    findProgressMarkerFilesWithPattern(directory, juce::String(kLegacyProgressMarkerPrefix) + "*" + kPresetFileExtension, sortedFiles);
#else
    juce::Array<juce::File> files;
    directory.findChildFiles(files, juce::File::findFiles, false, juce::String(kCompactProgressMarkerPrefix) + "*" + kPresetFileExtension);
    directory.findChildFiles(files, juce::File::findFiles, false, juce::String(kLegacyProgressMarkerPrefix) + "*" + kPresetFileExtension);

    std::vector<juce::File> sortedFiles;
    sortedFiles.reserve(static_cast<size_t>(files.size()));
    for (const auto& file : files)
        sortedFiles.push_back(file);
#endif

    std::sort(sortedFiles.begin(), sortedFiles.end(),
              [](const juce::File& lhs, const juce::File& rhs) { return lhs.getFileName() < rhs.getFileName(); });

    return sortedFiles;
}

std::optional<std::pair<juce::String, juce::String>> parseProgressMarkerTail(juce::String rest)
{
    const auto firstSeparator = rest.indexOfChar('_');
    if (firstSeparator < 0)
        return std::nullopt;

    rest = rest.substring(firstSeparator + 1);
    const auto secondSeparator = rest.indexOfChar('_');
    if (secondSeparator < 0)
        return std::nullopt;

    const auto kind = rest.substring(0, secondSeparator);
    const auto message = juce::String(decodeProgressMarkerText(rest.substring(secondSeparator + 1).toStdString()));
    return std::make_pair(kind, message);
}

std::optional<std::pair<juce::String, juce::String>> parseProgressMarkerFile(const juce::File& file)
{
    auto name = file.getFileNameWithoutExtension();

    if (name.startsWith(kCompactProgressMarkerPrefix))
        return parseProgressMarkerTail(name.substring(static_cast<int>(std::strlen(kCompactProgressMarkerPrefix))));

    if (name.startsWith(kLegacyProgressMarkerPrefix))
        return parseProgressMarkerTail(name.substring(static_cast<int>(std::strlen(kLegacyProgressMarkerPrefix))));

    return std::nullopt;
}

bool logProgressMarker(const juce::File& file)
{
    const auto parsed = parseProgressMarkerFile(file);
    if (!parsed)
        return false;

    const auto kind = parsed->first.toLowerCase();
    const auto message = parsed->second.toStdString();

    if (kind == "e" || kind == "error" || kind == "failed" || kind == "failure")
        log::error("{}", message);
    else if (kind == "w" || kind == "warning" || kind == "warn")
        log::warn("{}", message);
    else
        log::info("{}", message);

    return true;
}

void pauseBeforeRetry()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(kDeleteRetryDelayMs));
}

void mergeCleanupResult(ProgressMarkerCleanupResult& destination, const ProgressMarkerCleanupResult& source)
{
    destination.found += source.found;
    destination.deleted += source.deleted;
    destination.failed += source.failed;
    destination.remainingNames.insert(source.remainingNames.begin(), source.remainingNames.end());
}

bool deleteProgressMarkerFile(const juce::File& file)
{
    for (int attempt = 0; attempt < kDeleteRetryAttempts; ++attempt)
    {
        if (!fileExistsForProgressCleanup(file))
            return true;

        if (file.deleteFile() || !fileExistsForProgressCleanup(file))
            return true;

        pauseBeforeRetry();
    }

#if JUCE_WINDOWS
    const auto extendedPath = makeExtendedLengthPath(file.getFullPathName());
    for (int attempt = 0; attempt < kDeleteRetryAttempts; ++attempt)
    {
        if (!fileExistsForProgressCleanup(file))
            return true;

        if (::DeleteFileW(extendedPath.toWideCharPointer()) != 0 || !fileExistsForProgressCleanup(file))
            return true;

        const auto error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return true;

        pauseBeforeRetry();
    }
#endif

    return !fileExistsForProgressCleanup(file);
}

bool isValidUtf8(std::string_view text)
{
    auto continuationBytes = 0;
    auto minimumCodePoint = 0;
    auto codePoint = 0;

    for (const auto byte : text)
    {
        const auto value = static_cast<unsigned char>(byte);

        if (continuationBytes == 0)
        {
            if (value <= 0x7f)
                continue;

            if (value >= 0xc2 && value <= 0xdf)
            {
                continuationBytes = 1;
                minimumCodePoint = 0x80;
                codePoint = value & 0x1f;
                continue;
            }

            if (value >= 0xe0 && value <= 0xef)
            {
                continuationBytes = 2;
                minimumCodePoint = 0x800;
                codePoint = value & 0x0f;
                continue;
            }

            if (value >= 0xf0 && value <= 0xf4)
            {
                continuationBytes = 3;
                minimumCodePoint = 0x10000;
                codePoint = value & 0x07;
                continue;
            }

            return false;
        }

        if ((value & 0xc0) != 0x80)
            return false;

        codePoint = (codePoint << 6) | (value & 0x3f);
        --continuationBytes;

        if (continuationBytes == 0)
        {
            if (codePoint < minimumCodePoint || (codePoint >= 0xd800 && codePoint <= 0xdfff) || codePoint > 0x10ffff)
                return false;
        }
    }

    return continuationBytes == 0;
}

} // namespace

std::string decodeProgressMarkerText(std::string_view rawText)
{
    auto text = juce::String(std::string(rawText));

    if (text.isNotEmpty() && text.length() % 2 == 0)
    {
        auto decodedBytes = std::string();
        auto validHex = true;

        for (int i = 0; i < text.length(); i += 2)
        {
            const auto byteText = text.substring(i, i + 2);
            auto byteValue = 0;

            for (int j = 0; j < 2; ++j)
            {
                const auto c = byteText[j];
                byteValue <<= 4;

                if (c >= '0' && c <= '9')
                    byteValue += c - '0';
                else if (c >= 'A' && c <= 'F')
                    byteValue += c - 'A' + 10;
                else if (c >= 'a' && c <= 'f')
                    byteValue += c - 'a' + 10;
                else
                    validHex = false;
            }

            if (!validHex)
                break;

            decodedBytes.push_back(static_cast<char>(byteValue));
        }

        if (validHex)
        {
            if (isValidUtf8(decodedBytes))
            {
                const auto decoded = juce::String::fromUTF8(decodedBytes.data(), static_cast<int>(decodedBytes.size()));
                return (decoded.isEmpty() ? juce::String("HALion Lua progress") : decoded).toStdString();
            }
        }
    }

    text = text.replace("_", " ");
    text = text.replace("-", " ");
    text = text.trim();
    return (text.isEmpty() ? juce::String("HALion Lua progress") : text).toStdString();
}

ProgressMarkerCleanupResult logNewProgressMarkers(const juce::File& directory, std::set<std::string>& seenMarkers)
{
    ProgressMarkerCleanupResult result;

    for (const auto& file : findProgressMarkerFiles(directory))
    {
        const auto name = file.getFileName().toStdString();
        if (!seenMarkers.insert(name).second)
            continue;

        ++result.found;

        const auto path = file.getFullPathName().toStdString();
        log::debug("HALion Lua progress marker written: {}", path);

        const auto logged = logProgressMarker(file);
        if (!logged)
            log::debug("Ignoring malformed HALion Lua progress marker: {}", path);

        if (deleteProgressMarkerFile(file))
        {
            ++result.deleted;
            log::debug("Deleted consumed HALion Lua progress marker: {}", path);
        }
        else
        {
            ++result.failed;
            result.remainingNames.insert(name);
            log::warn("Failed to delete consumed HALion Lua progress marker: {}", path);
        }
    }

    return result;
}

ProgressMarkerCleanupResult drainProgressMarkers(const juce::File& directory, std::set<std::string>& seenMarkers, const int maxDurationMs,
                                                 const int quietDurationMs)
{
    ProgressMarkerCleanupResult aggregate;
    const auto start = std::chrono::steady_clock::now();
    auto quietStart = start;

    while (true)
    {
        auto result = logNewProgressMarkers(directory, seenMarkers);
        if (result.found > 0)
            quietStart = std::chrono::steady_clock::now();

        mergeCleanupResult(aggregate, result);

        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        const auto quietMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - quietStart).count();

        if (elapsedMs >= maxDurationMs || quietMs >= quietDurationMs)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(kDeleteRetryDelayMs));
    }

    auto finalCleanup = deleteProgressMarkers(directory, "HALion Lua progress marker after progress drain");
    aggregate.found += finalCleanup.found;
    aggregate.deleted += finalCleanup.deleted;
    aggregate.failed = finalCleanup.failed;
    aggregate.remainingNames = std::move(finalCleanup.remainingNames);
    return aggregate;
}

ProgressMarkerCleanupResult deleteProgressMarkers(const juce::File& directory, const char* description)
{
    ProgressMarkerCleanupResult result;

    for (const auto& file : findProgressMarkerFiles(directory))
    {
        ++result.found;

        if (deleteProgressMarkerFile(file))
        {
            ++result.deleted;
            log::debug("Deleted {}: {}", description, file.getFullPathName().toStdString());
        }
        else
        {
            ++result.failed;
            result.remainingNames.insert(file.getFileName().toStdString());
            log::warn("Failed to delete {}: {}", description, file.getFullPathName().toStdString());
        }
    }

    return result;
}

} // namespace halionbridge::detail
