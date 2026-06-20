#include "VstPresetMetadata.h"

#include "PathUtils.h"

#include <CLI/CLI.hpp>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace halionbridge::detail
{
namespace
{

constexpr std::string_view kFilenamePathColumn = "filename_path";
constexpr std::array<std::string_view, 7> kMetadataColumns = {
    "MediaAuthor", "MediaLibraryName", "MediaComment", "MusicalCategory", "MusicalInstrument", "MusicalProperties", "VST3UnitTypePath"};
constexpr std::array<std::string_view, 4> kHelperColumns = {"source_bank", "preset_number", "source_class", "preset_name"};
constexpr std::string_view kVstPresetExtension = ".vstpreset";
constexpr std::size_t kVstPresetHeaderSize = 48;
constexpr std::size_t kVstPresetListEntrySize = 20;
constexpr std::int32_t kVstPresetFormatVersion = 1;

struct CsvTable
{
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

struct PresetFileEntry
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativePath;
    std::string relativePathText;
};

struct PresetScanResult
{
    std::vector<PresetFileEntry> files;
    std::vector<std::string> errors;
};

struct ChunkEntry
{
    std::string id;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
};

struct VstPresetContainer
{
    std::vector<std::byte> bytes;
    std::string classId;
    std::vector<ChunkEntry> entries;
    std::optional<std::string> infoXml;
};

void addDiagnostic(std::vector<CliDiagnostic>& diagnostics, const CliDiagnosticLevel level, std::string message)
{
    diagnostics.push_back(CliDiagnostic{level, std::move(message)});
}

void addError(std::vector<CliDiagnostic>& diagnostics, std::string message)
{
    addDiagnostic(diagnostics, CliDiagnosticLevel::error, std::move(message));
}

std::vector<std::string> toMutableArgs(std::span<const std::string> args)
{
    return {args.rbegin(), args.rend()};
}

bool parseCli11App(CLI::App& app, std::span<const std::string> args, std::vector<CliDiagnostic>& diagnostics)
{
    auto mutableArgs = toMutableArgs(args);
    try
    {
        app.parse(mutableArgs);
        return true;
    }
    catch (const CLI::ParseError& error)
    {
        addError(diagnostics, error.what());
        return false;
    }
}

std::string toLowerAscii(std::string text)
{
    std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};

    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string removeUtf8Bom(std::string text)
{
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef && static_cast<unsigned char>(text[1]) == 0xbb &&
        static_cast<unsigned char>(text[2]) == 0xbf)
        text.erase(0, 3);
    return text;
}

std::string genericPathString(const std::filesystem::path& path)
{
    auto text = path.generic_string();
    while (!text.empty() && text.front() == '/')
        text.erase(text.begin());
    return text;
}

bool hasVstPresetExtension(const std::filesystem::path& path)
{
    return toLowerAscii(path.extension().string()) == kVstPresetExtension;
}

bool isSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    for (const auto& element : path)
    {
        const auto text = element.generic_string();
        if (text.empty() || text == "." || text == "..")
            return false;
    }

    return true;
}

std::filesystem::path comparableAbsolutePath(const std::filesystem::path& path)
{
    std::error_code ec;
    auto result = std::filesystem::weakly_canonical(path, ec);
    if (ec)
    {
        ec.clear();
        result = std::filesystem::absolute(path, ec);
    }
    if (ec)
        result = path;

    result = result.lexically_normal();

#if JUCE_WINDOWS
    return std::filesystem::path(toLowerAscii(result.generic_string()));
#else
    return result;
#endif
}

bool pathContainsOrEquals(const std::filesystem::path& ancestor, const std::filesystem::path& candidate)
{
    const auto comparableAncestor = comparableAbsolutePath(ancestor);
    const auto comparableCandidate = comparableAbsolutePath(candidate);

    auto ancestorIt = comparableAncestor.begin();
    auto candidateIt = comparableCandidate.begin();
    for (; ancestorIt != comparableAncestor.end(); ++ancestorIt, ++candidateIt)
    {
        if (candidateIt == comparableCandidate.end() || *ancestorIt != *candidateIt)
            return false;
    }

    return true;
}

bool isDirectoryEmpty(const std::filesystem::path& directory, std::string& errorMessage)
{
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec))
        return true;
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    if (!std::filesystem::is_directory(directory, ec))
    {
        errorMessage = "Output path exists but is not a directory: " + directory.string();
        return false;
    }
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    auto iterator = std::filesystem::directory_iterator(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        errorMessage = "Could not inspect output directory " + directory.string() + ": " + ec.message();
        return false;
    }

    return iterator == std::filesystem::directory_iterator();
}

PresetScanResult scanVstPresetFiles(const std::filesystem::path& inputDirectory, const bool recursive)
{
    PresetScanResult result;
    std::error_code ec;

    if (!std::filesystem::exists(inputDirectory, ec) || !std::filesystem::is_directory(inputDirectory, ec))
    {
        result.errors.push_back("Input directory does not exist: " + inputDirectory.string());
        return result;
    }
    if (ec)
    {
        result.errors.push_back("Could not inspect input directory " + inputDirectory.string() + ": " + ec.message());
        return result;
    }

    std::set<std::string> seenRelativePaths;
    const auto addFile = [&](const std::filesystem::directory_entry& entry)
    {
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc) || !hasVstPresetExtension(entry.path()))
            return;

        std::error_code relativeEc;
        auto relative = std::filesystem::relative(entry.path(), inputDirectory, relativeEc);
        if (relativeEc || !isSafeRelativePath(relative))
        {
            result.errors.push_back("Could not derive a safe relative path for " + entry.path().string());
            return;
        }

        const auto relativeText = genericPathString(relative);
        const auto key = toLowerAscii(relativeText);
        if (!seenRelativePaths.insert(key).second)
        {
            result.errors.push_back("Duplicate relative .vstpreset path after case normalization: " + relativeText);
            return;
        }

        result.files.push_back({entry.path(), relative, relativeText});
    };

    if (recursive)
    {
        for (std::filesystem::recursive_directory_iterator it(inputDirectory, std::filesystem::directory_options::skip_permission_denied,
                                                              ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            addFile(*it);
    }
    else
    {
        for (std::filesystem::directory_iterator it(inputDirectory, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
            addFile(*it);
    }

    if (ec)
        result.errors.push_back("Could not scan input directory " + inputDirectory.string() + ": " + ec.message());

    std::ranges::sort(result.files, [](const PresetFileEntry& lhs, const PresetFileEntry& rhs)
                      { return toLowerAscii(lhs.relativePathText) < toLowerAscii(rhs.relativePathText); });

    if (result.files.empty() && result.errors.empty())
        result.errors.push_back("Input directory contains no .vstpreset files: " + inputDirectory.string());

    return result;
}

std::uint32_t readU32(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::uint64_t readU64(const std::vector<std::byte>& bytes, const std::size_t offset)
{
    std::uint64_t value = 0;
    for (auto i = std::size_t{0}; i < 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

void appendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    for (auto i = 0U; i < 4U; ++i)
        bytes.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xffU));
}

void appendU64(std::vector<std::byte>& bytes, const std::uint64_t value)
{
    for (auto i = 0U; i < 8U; ++i)
        bytes.push_back(static_cast<std::byte>((value >> (i * 8U)) & 0xffU));
}

void appendText(std::vector<std::byte>& bytes, std::string_view text)
{
    for (const auto c : text)
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
}

bool rangeFits(const std::size_t totalSize, const std::uint64_t offset, const std::uint64_t size)
{
    if (offset > static_cast<std::uint64_t>(totalSize))
        return false;

    return size <= static_cast<std::uint64_t>(totalSize) - offset;
}

bool readBinaryFile(const std::filesystem::path& path, std::vector<std::byte>& bytes, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Could not open preset file: " + path.string();
        return false;
    }

    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    if (size < 0)
    {
        error = "Could not determine preset file size: " + path.string();
        return false;
    }

    stream.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty())
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));

    if (!stream)
    {
        error = "Could not read preset file: " + path.string();
        return false;
    }

    return true;
}

bool parseVstPresetContainer(const std::filesystem::path& path, VstPresetContainer& container, std::string& error)
{
    if (!readBinaryFile(path, container.bytes, error))
        return false;

    if (container.bytes.size() < kVstPresetHeaderSize)
    {
        error = "Preset file is too small to contain a VST3 header: " + path.string();
        return false;
    }

    const auto headerId = std::string(reinterpret_cast<const char*>(container.bytes.data()), 4);
    if (headerId != "VST3")
    {
        error = "Preset file does not start with a VST3 header: " + path.string();
        return false;
    }

    const auto version = readU32(container.bytes, 4);
    if (version != kVstPresetFormatVersion)
    {
        error = "Unsupported VSTPreset format version " + std::to_string(version) + " in " + path.string();
        return false;
    }

    container.classId.assign(reinterpret_cast<const char*>(container.bytes.data() + 8), 32);
    const auto listOffset = readU64(container.bytes, 40);
    if (!rangeFits(container.bytes.size(), listOffset, 8))
    {
        error = "Preset chunk list offset is outside the file: " + path.string();
        return false;
    }

    const auto listOffsetSize = static_cast<std::size_t>(listOffset);
    const auto listId = std::string(reinterpret_cast<const char*>(container.bytes.data() + listOffsetSize), 4);
    if (listId != "List")
    {
        error = "Preset chunk list is missing or invalid: " + path.string();
        return false;
    }

    const auto count = readU32(container.bytes, listOffsetSize + 4);
    if (count > 128)
    {
        error = "Preset chunk list contains too many entries: " + path.string();
        return false;
    }

    const auto listBytes = static_cast<std::uint64_t>(8) + static_cast<std::uint64_t>(count) * kVstPresetListEntrySize;
    if (!rangeFits(container.bytes.size(), listOffset, listBytes))
    {
        error = "Preset chunk list extends beyond the file: " + path.string();
        return false;
    }

    container.entries.clear();
    container.infoXml.reset();
    std::set<std::string> seenInfoChunks;
    for (auto i = std::uint32_t{0}; i < count; ++i)
    {
        const auto entryOffset = listOffsetSize + 8 + static_cast<std::size_t>(i) * kVstPresetListEntrySize;
        ChunkEntry entry;
        entry.id.assign(reinterpret_cast<const char*>(container.bytes.data() + entryOffset), 4);
        entry.offset = readU64(container.bytes, entryOffset + 4);
        entry.size = readU64(container.bytes, entryOffset + 12);

        if (entry.id == "List" || entry.id == "VST3")
        {
            error = "Preset chunk list contains an invalid chunk id: " + path.string();
            return false;
        }

        if (!rangeFits(container.bytes.size(), entry.offset, entry.size))
        {
            error = "Preset chunk " + entry.id + " extends beyond the file: " + path.string();
            return false;
        }

        if (entry.id == "Info")
        {
            if (!seenInfoChunks.insert(entry.id).second)
            {
                error = "Preset contains multiple Info metadata chunks: " + path.string();
                return false;
            }

            container.infoXml = std::string(reinterpret_cast<const char*>(container.bytes.data() + static_cast<std::size_t>(entry.offset)),
                                            static_cast<std::size_t>(entry.size));
        }

        container.entries.push_back(entry);
    }

    return true;
}

std::unique_ptr<juce::XmlElement> makeEmptyMetaInfoXml()
{
    return std::make_unique<juce::XmlElement>("MetaInfo");
}

std::unique_ptr<juce::XmlElement> parseMetaInfoXml(const std::optional<std::string>& xmlText, std::string& error)
{
    if (!xmlText)
        return makeEmptyMetaInfoXml();

    const auto normalizedXmlText = removeUtf8Bom(*xmlText);
    if (trim(normalizedXmlText).empty())
        return makeEmptyMetaInfoXml();

    auto xml = juce::XmlDocument::parse(juce::String::fromUTF8(normalizedXmlText.data(), static_cast<int>(normalizedXmlText.size())));
    if (!xml)
    {
        error = "Preset Info metadata XML is invalid.";
        return {};
    }

    if (!xml->hasTagName("MetaInfo"))
    {
        error = "Preset Info metadata XML root must be MetaInfo.";
        return {};
    }

    return xml;
}

juce::XmlElement* findAttributeElement(juce::XmlElement& root, const std::string_view id)
{
    for (auto* child = root.getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        if (child->hasTagName("Attribute") &&
            child->getStringAttribute("id") == juce::String::fromUTF8(id.data(), static_cast<int>(id.size())))
            return child;
    }

    return nullptr;
}

std::unordered_map<std::string, std::string> extractMetadataFields(const std::optional<std::string>& xmlText, std::string& error)
{
    std::unordered_map<std::string, std::string> fields;
    auto xml = parseMetaInfoXml(xmlText, error);
    if (!xml)
        return fields;

    for (const auto column : kMetadataColumns)
    {
        if (auto* attribute = findAttributeElement(*xml, column))
            fields.emplace(std::string(column), attribute->getStringAttribute("value").toStdString());
    }

    return fields;
}

bool updateMetadataXml(const std::optional<std::string>& originalXml, const std::unordered_map<std::string, std::string>& updates,
                       std::string& updatedXml, std::string& error)
{
    auto xml = parseMetaInfoXml(originalXml, error);
    if (!xml)
        return false;

    for (const auto& [id, value] : updates)
    {
        if (std::ranges::find(kMetadataColumns, std::string_view(id)) == kMetadataColumns.end())
            continue;

        if (auto* existing = findAttributeElement(*xml, id))
        {
            if (value.empty())
                xml->removeChildElement(existing, true);
            else
            {
                existing->setAttribute("value", juce::String::fromUTF8(value.data(), static_cast<int>(value.size())));
                if (!existing->hasAttribute("type"))
                    existing->setAttribute("type", "string");
            }
            continue;
        }

        if (!value.empty())
        {
            auto* attribute = new juce::XmlElement("Attribute");
            attribute->setAttribute("id", juce::String::fromUTF8(id.data(), static_cast<int>(id.size())));
            attribute->setAttribute("value", juce::String::fromUTF8(value.data(), static_cast<int>(value.size())));
            attribute->setAttribute("type", "string");
            if (id == "VST3UnitTypePath")
                attribute->setAttribute("flags", "hidden|writeProtected");
            xml->addChildElement(attribute);
        }
    }

    updatedXml = xml->toString().toStdString();
    return true;
}

bool writeBinaryFileAtomically(const std::filesystem::path& destination, const std::vector<std::byte>& bytes, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = "Could not create output directory " + destination.parent_path().string() + ": " + ec.message();
        return false;
    }

    if (std::filesystem::exists(destination, ec))
    {
        error = "Refusing to overwrite existing output preset: " + destination.string();
        return false;
    }
    if (ec)
    {
        error = "Could not inspect output preset path " + destination.string() + ": " + ec.message();
        return false;
    }

    const auto temporary = destination.parent_path() / (destination.filename().string() + ".halionbridge.tmp");
    std::filesystem::remove(temporary, ec);
    ec.clear();

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error = "Could not create temporary output preset: " + temporary.string();
            return false;
        }
        if (!bytes.empty())
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream)
        {
            error = "Could not write temporary output preset: " + temporary.string();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    std::filesystem::rename(temporary, destination, ec);
    if (ec)
    {
        error = "Could not move temporary preset into place at " + destination.string() + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }

    return true;
}

std::vector<std::byte> rebuildPresetWithInfoChunk(const VstPresetContainer& container, const std::string& infoXml)
{
    std::vector<std::byte> output;
    output.reserve(container.bytes.size() + infoXml.size() + 128);
    appendText(output, "VST3");
    appendU32(output, kVstPresetFormatVersion);
    appendText(output, std::string_view(container.classId.data(), container.classId.size()));
    appendU64(output, 0);

    std::vector<ChunkEntry> outputEntries;
    outputEntries.reserve(container.entries.size() + 1);

    for (const auto& entry : container.entries)
    {
        if (entry.id == "Info")
            continue;

        ChunkEntry rewritten = entry;
        rewritten.offset = output.size();
        const auto begin = container.bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset);
        const auto end = begin + static_cast<std::ptrdiff_t>(entry.size);
        output.insert(output.end(), begin, end);
        outputEntries.push_back(rewritten);
    }

    if (!infoXml.empty())
    {
        ChunkEntry infoEntry;
        infoEntry.id = "Info";
        infoEntry.offset = output.size();
        infoEntry.size = infoXml.size();
        appendText(output, infoXml);
        outputEntries.push_back(infoEntry);
    }

    const auto listOffset = output.size();
    appendText(output, "List");
    appendU32(output, static_cast<std::uint32_t>(outputEntries.size()));
    for (const auto& entry : outputEntries)
    {
        appendText(output, entry.id);
        appendU64(output, entry.offset);
        appendU64(output, entry.size);
    }

    for (auto i = 0U; i < 8U; ++i)
        output[40 + i] = static_cast<std::byte>((static_cast<std::uint64_t>(listOffset) >> (i * 8U)) & 0xffU);

    return output;
}

bool rewritePresetMetadata(const std::filesystem::path& source, const std::filesystem::path& destination,
                           const std::unordered_map<std::string, std::string>& updates, std::string& error)
{
    VstPresetContainer container;
    if (!parseVstPresetContainer(source, container, error))
        return false;

    std::string updatedXml;
    if (!updateMetadataXml(container.infoXml, updates, updatedXml, error))
    {
        error += " File: " + source.string();
        return false;
    }

    const auto outputBytes = rebuildPresetWithInfoChunk(container, updatedXml);
    return writeBinaryFileAtomically(destination, outputBytes, error);
}

bool writeTextFileAtomically(const std::filesystem::path& destination, const std::string& text, const bool overwrite, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec)
    {
        error = "Could not create CSV output directory " + destination.parent_path().string() + ": " + ec.message();
        return false;
    }

    if (!overwrite && std::filesystem::exists(destination, ec))
    {
        error = "Metadata CSV already exists. Use --overwrite to replace it: " + destination.string();
        return false;
    }
    if (ec)
    {
        error = "Could not inspect metadata CSV path " + destination.string() + ": " + ec.message();
        return false;
    }

    const auto temporary = destination.parent_path() / (destination.filename().string() + ".halionbridge.tmp");
    std::filesystem::remove(temporary, ec);
    ec.clear();

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            error = "Could not create temporary metadata CSV: " + temporary.string();
            return false;
        }
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!stream)
        {
            error = "Could not write temporary metadata CSV: " + temporary.string();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }

    if (overwrite)
        std::filesystem::remove(destination, ec);
    ec.clear();

    std::filesystem::rename(temporary, destination, ec);
    if (ec)
    {
        error = "Could not move temporary metadata CSV into place at " + destination.string() + ": " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }

    return true;
}

bool readTextFile(const std::filesystem::path& path, std::string& text, std::string& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        error = "Could not open metadata CSV: " + path.string();
        return false;
    }

    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (!stream.eof() && stream.fail())
    {
        error = "Could not read metadata CSV: " + path.string();
        return false;
    }

    return true;
}

std::optional<CsvTable> parseCsvTable(std::string_view rawText, std::vector<std::string>& errors)
{
    const auto text = removeUtf8Bom(std::string(rawText));
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    auto inQuotes = false;
    auto afterQuote = false;

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = text[i];
        if (inQuotes)
        {
            if (c == '"')
            {
                if (i + 1 < text.size() && text[i + 1] == '"')
                {
                    cell.push_back('"');
                    ++i;
                }
                else
                {
                    inQuotes = false;
                    afterQuote = true;
                }
            }
            else
            {
                cell.push_back(c);
            }
            continue;
        }

        if (afterQuote && c != ',' && c != '\r' && c != '\n')
        {
            errors.push_back("CSV has unexpected text after a quoted cell.");
            return std::nullopt;
        }

        if (c == '"')
        {
            if (!cell.empty())
            {
                errors.push_back("CSV quotes must begin at the start of a cell.");
                return std::nullopt;
            }
            inQuotes = true;
            continue;
        }

        if (c == ',')
        {
            row.push_back(cell);
            cell.clear();
            afterQuote = false;
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            row.push_back(cell);
            cell.clear();
            rows.push_back(row);
            row.clear();
            afterQuote = false;
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            continue;
        }

        cell.push_back(c);
        afterQuote = false;
    }

    if (inQuotes)
    {
        errors.push_back("CSV ends inside a quoted cell.");
        return std::nullopt;
    }

    if (!cell.empty() || !row.empty() || afterQuote)
    {
        row.push_back(cell);
        rows.push_back(row);
    }

    while (!rows.empty() && rows.back().size() == 1 && rows.back().front().empty())
        rows.pop_back();

    if (rows.empty())
    {
        errors.push_back("Metadata CSV is empty.");
        return std::nullopt;
    }

    CsvTable table;
    table.headers = rows.front();
    table.rows.assign(rows.begin() + 1, rows.end());
    return table;
}

std::string quoteCsvCell(std::string_view cell)
{
    const auto mustQuote = cell.find_first_of(",\"\r\n") != std::string_view::npos;
    if (!mustQuote)
        return std::string(cell);

    std::string output;
    output.reserve(cell.size() + 2);
    output.push_back('"');
    for (const auto c : cell)
    {
        if (c == '"')
            output.push_back('"');
        output.push_back(c);
    }
    output.push_back('"');
    return output;
}

bool validateCsvHeaders(const std::vector<std::string>& headers, std::map<std::string, std::size_t>& indices,
                        std::vector<std::string>& errors)
{
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        const auto header = trim(headers[i]);
        if (header.empty())
        {
            errors.push_back("Metadata CSV contains an empty header.");
            return false;
        }

        if (!indices.emplace(header, i).second)
        {
            errors.push_back("Metadata CSV contains duplicate header: " + header);
            return false;
        }
    }

    if (!indices.contains(std::string(kFilenamePathColumn)))
    {
        errors.push_back("Metadata CSV must contain a filename_path column.");
        return false;
    }

    return true;
}

std::optional<std::filesystem::path> safeRelativePathFromCsv(std::string text, std::string& error)
{
    text = trim(std::move(text));
    std::replace(text.begin(), text.end(), '\\', '/');
    while (!text.empty() && text.front() == '/')
        text.erase(text.begin());

    auto relative = std::filesystem::path(text).lexically_normal();
    if (!isSafeRelativePath(relative) || !hasVstPresetExtension(relative))
    {
        error = "CSV filename_path must be a safe relative .vstpreset path: " + text;
        return std::nullopt;
    }

    return relative;
}

int runExportCommand(const VstPresetMetadataOptions& options, std::vector<CliDiagnostic>& diagnostics)
{
    const auto scan = scanVstPresetFiles(options.inputDirectory, options.recursive);
    for (const auto& error : scan.errors)
        addError(diagnostics, error);
    if (!diagnostics.empty())
        return 1;

    std::vector<VstPresetMetadataRecord> records;
    for (const auto& file : scan.files)
    {
        VstPresetContainer container;
        std::string error;
        if (!parseVstPresetContainer(file.sourcePath, container, error))
        {
            addError(diagnostics, error);
            continue;
        }

        auto metadata = extractMetadataFields(container.infoXml, error);
        if (!error.empty())
        {
            addError(diagnostics, error + " File: " + file.sourcePath.string());
            continue;
        }

        metadata.emplace("preset_name", file.relativePath.stem().string());
        records.push_back({file.relativePathText, std::move(metadata)});
    }

    if (!diagnostics.empty())
        return 1;

    std::string error;
    if (!writeTextFileAtomically(options.metadataCsv, writeVstPresetMetadataCsv(records), options.overwrite, error))
    {
        addError(diagnostics, error);
        return 1;
    }

    return 0;
}

int runApplyCommand(const VstPresetMetadataOptions& options, std::vector<CliDiagnostic>& diagnostics)
{
    if (!options.outputDirectory)
    {
        addError(diagnostics, "vstpreset-metadata apply requires --output-directory.");
        return 1;
    }

    if (pathContainsOrEquals(options.inputDirectory, *options.outputDirectory) ||
        pathContainsOrEquals(*options.outputDirectory, options.inputDirectory))
    {
        addError(diagnostics, "--output-directory must not overlap --input-directory.");
        return 1;
    }

    std::string emptyError;
    if (!isDirectoryEmpty(*options.outputDirectory, emptyError))
    {
        addError(diagnostics, emptyError.empty() ? "Output directory must be empty or missing." : emptyError);
        return 1;
    }

    const auto scan = scanVstPresetFiles(options.inputDirectory, options.recursive);
    for (const auto& error : scan.errors)
        addError(diagnostics, error);
    if (!diagnostics.empty())
        return 1;

    std::string csvText;
    std::string error;
    if (!readTextFile(options.metadataCsv, csvText, error))
    {
        addError(diagnostics, error);
        return 1;
    }

    std::vector<std::string> csvErrors;
    auto records = parseVstPresetMetadataCsv(csvText, csvErrors);
    for (const auto& csvError : csvErrors)
        addError(diagnostics, csvError);
    if (!diagnostics.empty())
        return 1;

    std::map<std::string, VstPresetMetadataRecord> recordsByPath;
    for (auto& record : records)
    {
        std::string pathError;
        const auto relativePath = safeRelativePathFromCsv(record.filenamePath, pathError);
        if (!relativePath)
        {
            addError(diagnostics, pathError);
            continue;
        }

        const auto normalized = genericPathString(*relativePath);
        record.filenamePath = normalized;
        const auto key = toLowerAscii(normalized);
        if (!recordsByPath.emplace(key, std::move(record)).second)
            addError(diagnostics, "Metadata CSV contains duplicate filename_path after case normalization: " + normalized);
    }

    std::set<std::string> scannedKeys;
    for (const auto& file : scan.files)
    {
        const auto key = toLowerAscii(file.relativePathText);
        scannedKeys.insert(key);
        if (!recordsByPath.contains(key))
            addError(diagnostics, "Metadata CSV is missing a row for scanned preset: " + file.relativePathText);
    }

    for (const auto& [key, record] : recordsByPath)
        if (!scannedKeys.contains(key))
            addError(diagnostics, "Metadata CSV row does not match a scanned preset: " + record.filenamePath);

    if (!diagnostics.empty())
        return 1;

    for (const auto& file : scan.files)
    {
        const auto key = toLowerAscii(file.relativePathText);
        const auto& record = recordsByPath.at(key);
        const auto destination = *options.outputDirectory / file.relativePath;
        if (!rewritePresetMetadata(file.sourcePath, destination, record.metadata, error))
            addError(diagnostics, error);
    }

    return diagnostics.empty() ? 0 : 1;
}

} // namespace

std::vector<std::string> vstPresetMetadataFieldNames()
{
    std::vector<std::string> fields;
    fields.reserve(kMetadataColumns.size());
    for (const auto column : kMetadataColumns)
        fields.emplace_back(column);
    return fields;
}

std::vector<VstPresetMetadataRecord> parseVstPresetMetadataCsv(std::string_view text, std::vector<std::string>& errors)
{
    const auto table = parseCsvTable(text, errors);
    if (!table)
        return {};

    std::map<std::string, std::size_t> indices;
    if (!validateCsvHeaders(table->headers, indices, errors))
        return {};

    std::vector<std::string> presentMetadataColumns;
    for (const auto column : kMetadataColumns)
        if (indices.contains(std::string(column)))
            presentMetadataColumns.emplace_back(column);

    if (presentMetadataColumns.empty())
    {
        errors.push_back("Metadata CSV must contain at least one metadata column.");
        return {};
    }

    std::vector<VstPresetMetadataRecord> records;
    for (std::size_t rowIndex = 0; rowIndex < table->rows.size(); ++rowIndex)
    {
        auto row = table->rows[rowIndex];
        if (row.size() > table->headers.size())
        {
            errors.push_back("Metadata CSV row " + std::to_string(rowIndex + 2) + " has more cells than headers.");
            continue;
        }
        row.resize(table->headers.size());

        VstPresetMetadataRecord record;
        record.filenamePath = row[indices.at(std::string(kFilenamePathColumn))];
        if (trim(record.filenamePath).empty())
        {
            errors.push_back("Metadata CSV row " + std::to_string(rowIndex + 2) + " has an empty filename_path.");
            continue;
        }

        for (const auto& column : presentMetadataColumns)
            record.metadata.emplace(column, row[indices.at(column)]);

        records.push_back(std::move(record));
    }

    return records;
}

std::string writeVstPresetMetadataCsv(std::span<const VstPresetMetadataRecord> records)
{
    std::vector<std::string> headers;
    for (const auto column : kHelperColumns)
        headers.emplace_back(column);
    headers.emplace_back(kFilenamePathColumn);
    for (const auto column : kMetadataColumns)
        headers.emplace_back(column);

    std::ostringstream output;
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        if (i != 0)
            output << ',';
        output << quoteCsvCell(headers[i]);
    }
    output << '\n';

    for (const auto& record : records)
    {
        for (std::size_t i = 0; i < headers.size(); ++i)
        {
            if (i != 0)
                output << ',';

            std::string value;
            if (headers[i] == kFilenamePathColumn)
                value = record.filenamePath;
            else if (auto it = record.metadata.find(headers[i]); it != record.metadata.end())
                value = it->second;

            output << quoteCsvCell(value);
        }
        output << '\n';
    }

    return output.str();
}

VstPresetMetadataOptionsParseResult parseVstPresetMetadataOptionsDetailed(std::span<const std::string> args)
{
    VstPresetMetadataOptionsParseResult result;
    VstPresetMetadataOptions options;

    if (args.empty())
    {
        addError(result.diagnostics, "vstpreset-metadata requires a subcommand: export or apply.");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    const auto action = args.front();
    if (action == "export")
        options.action = VstPresetMetadataAction::exportCsv;
    else if (action == "apply")
        options.action = VstPresetMetadataAction::applyCsv;
    else
    {
        addError(result.diagnostics, "Unknown vstpreset-metadata subcommand: " + action);
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    std::string inputDirectoryText;
    std::string metadataCsvText;
    std::string outputDirectoryText;
    CLI::App app{"halionbridge vstpreset-metadata " + action};
    app.set_help_flag();
    app.add_option("--input-directory", inputDirectoryText);
    app.add_option("--metadata-csv", metadataCsvText);
    app.add_option("--output-directory", outputDirectoryText);
    app.add_flag("--recursive", options.recursive);
    app.add_flag("--overwrite", options.overwrite);

    const auto subcommandArgs = std::span<const std::string>(args.begin() + 1, args.end());
    if (!parseCli11App(app, subcommandArgs, result.diagnostics))
    {
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (app.remaining_size() > 0)
    {
        const auto unexpected = app.remaining().empty() ? std::string() : app.remaining().front();
        addError(result.diagnostics, "vstpreset-metadata uses named options. Unexpected positional argument: " + unexpected);
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (inputDirectoryText.empty() || metadataCsvText.empty() ||
        (options.action == VstPresetMetadataAction::applyCsv && outputDirectoryText.empty()))
    {
        addError(result.diagnostics, options.action == VstPresetMetadataAction::exportCsv
                                         ? "vstpreset-metadata export requires --input-directory and --metadata-csv."
                                         : "vstpreset-metadata apply requires --input-directory, --metadata-csv, and --output-directory.");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    if (options.action == VstPresetMetadataAction::applyCsv && options.overwrite)
    {
        addError(result.diagnostics, "--overwrite is only valid for vstpreset-metadata export.");
        result.errorKind = CliParseErrorKind::syntax;
        return result;
    }

    const auto inputDirectory = normalizeCliPath(toJuceString(std::string_view(inputDirectoryText)));
    if (!inputDirectory.isDirectory())
    {
        addError(result.diagnostics, "Input directory does not exist at " + inputDirectory.getFullPathName().toStdString());
        result.errorKind = CliParseErrorKind::validation;
        return result;
    }
    options.inputDirectory = toStdPath(inputDirectory);
    options.metadataCsv = toStdPath(normalizeCliPath(toJuceString(std::string_view(metadataCsvText))));
    if (!outputDirectoryText.empty())
        options.outputDirectory = toStdPath(normalizeCliPath(toJuceString(std::string_view(outputDirectoryText))));

    result.options = std::move(options);
    return result;
}

VstPresetMetadataResult runVstPresetMetadataCommand(const VstPresetMetadataOptions& options)
{
    VstPresetMetadataResult result;
    if (options.action == VstPresetMetadataAction::exportCsv)
        result.exitCode = runExportCommand(options, result.diagnostics);
    else
        result.exitCode = runApplyCommand(options, result.diagnostics);
    return result;
}

} // namespace halionbridge::detail
