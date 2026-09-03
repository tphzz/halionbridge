#include "halionbridge_converters/Converter.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace halionbridge::converters
{

CommonConverterArgumentParseResult parseCommonConverterArguments(const std::span<const std::string> args)
{
    auto parsed = CommonConverterArgumentParseResult{};
    parsed.remainingArguments.reserve(args.size());

    if (std::ranges::any_of(args, [](const std::string_view argument) { return argument == "--help" || argument == "-h"; }))
    {
        parsed.remainingArguments.assign(args.begin(), args.end());
        return parsed;
    }

    auto foundPresetType = false;
    auto foundPresetTarget = false;
    for (auto index = std::size_t{0}; index < args.size(); ++index)
    {
        if (args[index] != "--preset-type" && args[index] != "--preset-target")
        {
            parsed.remainingArguments.push_back(args[index]);
            continue;
        }

        const auto isPresetType = args[index] == "--preset-type";
        auto& foundOption = isPresetType ? foundPresetType : foundPresetTarget;
        const auto optionName = isPresetType ? std::string_view{"--preset-type"} : std::string_view{"--preset-target"};
        if (foundOption)
        {
            parsed.result.exitCode = 1;
            parsed.result.errorKind = ConverterArgumentErrorKind::syntax;
            parsed.result.diagnostics.push_back(
                Diagnostic{DiagnosticLevel::error, {}, 0, "argument", std::string{optionName} + " may only be specified once."});
            return parsed;
        }
        foundOption = true;

        if (index + 1 >= args.size() || args[index + 1].starts_with('-'))
        {
            parsed.result.exitCode = 1;
            parsed.result.errorKind = ConverterArgumentErrorKind::syntax;
            parsed.result.diagnostics.push_back(Diagnostic{DiagnosticLevel::error,
                                                           {},
                                                           0,
                                                           "argument",
                                                           isPresetType ? "--preset-type requires either program or layer."
                                                                        : "--preset-target requires either halion or halion-sonic."});
            return parsed;
        }

        const auto& value = args[++index];
        if (isPresetType && value == "program")
            parsed.presetOutputType = PresetOutputType::program;
        else if (isPresetType && value == "layer")
            parsed.presetOutputType = PresetOutputType::layer;
        else if (!isPresetType && value == "halion")
            parsed.presetTarget = PresetTarget::halion;
        else if (!isPresetType && value == "halion-sonic")
            parsed.presetTarget = PresetTarget::halionSonic;
        else
        {
            parsed.result.exitCode = 1;
            parsed.result.errorKind = ConverterArgumentErrorKind::syntax;
            parsed.result.diagnostics.push_back(
                Diagnostic{DiagnosticLevel::error,
                           {},
                           0,
                           "argument",
                           "Invalid " + std::string{optionName} + " value '" + value +
                               (isPresetType ? "'; expected program or layer." : "'; expected halion or halion-sonic.")});
            return parsed;
        }
    }

    if (parsed.presetTarget == PresetTarget::halionSonic && parsed.presetOutputType == PresetOutputType::layer)
    {
        parsed.result.exitCode = 1;
        parsed.result.errorKind = ConverterArgumentErrorKind::syntax;
        parsed.result.diagnostics.push_back(
            Diagnostic{DiagnosticLevel::error, {}, 0, "argument", "--preset-target halion-sonic requires --preset-type program."});
    }

    return parsed;
}

std::string commonConverterOptionsHelpText()
{
    return "  --preset-type <program|layer>\n"
           "                          Save program presets (default) or layer presets.\n"
           "  --preset-target <halion|halion-sonic>\n"
           "                          Target HALion (default) or HALion Sonic program presets.\n";
}

std::string_view presetOutputTypeName(const PresetOutputType type) noexcept
{
    switch (type)
    {
    case PresetOutputType::program:
        return "program";
    case PresetOutputType::layer:
        return "layer";
    }

    return "program";
}

std::string_view presetTargetName(const PresetTarget target) noexcept
{
    switch (target)
    {
    case PresetTarget::halion:
        return "halion";
    case PresetTarget::halionSonic:
        return "halion-sonic";
    }

    return "halion";
}

bool ConverterRegistry::registerConverter(ConverterDefinition definition)
{
    if (definition.id.empty() || (definition.run == nullptr && definition.runWithContext == nullptr))
        return false;

    if (find(definition.id) != nullptr)
        return false;

    definitions.push_back(std::move(definition));
    std::sort(definitions.begin(), definitions.end(),
              [](const ConverterDefinition& lhs, const ConverterDefinition& rhs) { return lhs.id < rhs.id; });
    return true;
}

const ConverterDefinition* ConverterRegistry::find(const std::string_view id) const noexcept
{
    const auto it =
        std::find_if(definitions.begin(), definitions.end(), [id](const ConverterDefinition& definition) { return definition.id == id; });
    return it == definitions.end() ? nullptr : &*it;
}

std::vector<ConverterDefinition> ConverterRegistry::list() const
{
    return definitions;
}

std::vector<ConverterDefinition> ConverterRegistry::listVisible() const
{
    auto visibleDefinitions = std::vector<ConverterDefinition>{};
    std::copy_if(definitions.begin(), definitions.end(), std::back_inserter(visibleDefinitions),
                 [](const ConverterDefinition& definition) { return definition.visibility == ConverterVisibility::listed; });
    return visibleDefinitions;
}

} // namespace halionbridge::converters
