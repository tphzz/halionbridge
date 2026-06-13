#include "halionbridge_converters/Converter.h"

#include <algorithm>
#include <utility>

namespace halionbridge::converters
{

bool ConverterRegistry::registerConverter(ConverterDefinition definition)
{
    if (definition.id.empty() || definition.run == nullptr)
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

} // namespace halionbridge::converters
