#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace halionbridge::converters
{

enum class DiagnosticLevel
{
    info,
    warning,
    error
};

struct Diagnostic
{
    DiagnosticLevel level = DiagnosticLevel::info;
    std::filesystem::path source;
    int line = 0;
    std::string code;
    std::string message;
};

struct ConverterResult
{
    int exitCode = 1;
    std::vector<Diagnostic> diagnostics;
};

enum class ConverterArgumentErrorKind
{
    none,
    syntax,
    validation,
};

enum class ConverterVisibility
{
    listed,
    incognito,
};

enum class ConverterSourcePathKind
{
    directory,
    file,
    fileOrDirectory,
};

struct ConverterArgumentParseResult
{
    int exitCode = 0;
    ConverterArgumentErrorKind errorKind = ConverterArgumentErrorKind::none;
    std::vector<Diagnostic> diagnostics;
};

struct ConverterRunContext
{
    void (*diagnostic)(const Diagnostic& diagnostic, void* userData) = nullptr;
    bool (*stopRequested)(void* userData) = nullptr;
    void* userData = nullptr;

    void report(const Diagnostic& entry) const
    {
        if (this->diagnostic != nullptr)
            this->diagnostic(entry, userData);
    }

    bool shouldStop() const
    {
        return stopRequested != nullptr && stopRequested(userData);
    }
};

struct ConverterDefinition
{
    std::string id;
    std::string displayName;
    std::string summary;
    ConverterResult (*run)(std::span<const std::string> args) = nullptr;
    ConverterResult (*runWithContext)(std::span<const std::string> args, const ConverterRunContext& context) = nullptr;
    std::string (*helpText)() = nullptr;
    ConverterArgumentParseResult (*validateArguments)(std::span<const std::string> args) = nullptr;
    ConverterVisibility visibility = ConverterVisibility::listed;
    ConverterSourcePathKind sourcePathKind = ConverterSourcePathKind::directory;
};

class ConverterRegistry
{
  public:
    bool registerConverter(ConverterDefinition definition);
    const ConverterDefinition* find(std::string_view id) const noexcept;
    std::vector<ConverterDefinition> list() const;
    std::vector<ConverterDefinition> listVisible() const;

  private:
    std::vector<ConverterDefinition> definitions;
};

void registerCompiledConverters(ConverterRegistry& registry);

} // namespace halionbridge::converters
