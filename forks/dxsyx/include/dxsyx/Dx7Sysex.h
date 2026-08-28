//
// Adapted from dxsyx by Roger Allen.
// Copyright (c) 2015 Roger Allen. All rights reserved.
//
// Dxsyx is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Dxsyx is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace dxsyx
{

inline constexpr auto packedVoiceSize = std::size_t{128};
inline constexpr auto packedBankSize = std::size_t{4096};
inline constexpr auto singleVoiceSize = std::size_t{155};
inline constexpr auto bankVoiceCount = std::size_t{32};
inline constexpr auto operatorCount = std::size_t{6};

enum class IssueSeverity
{
    warning,
    error,
};

struct ParseIssue
{
    IssueSeverity severity = IssueSeverity::error;
    std::string code;
    std::size_t byteOffset = 0;
    std::size_t messageIndex = 0;
    std::optional<std::size_t> voiceIndex;
    std::string field;
    std::optional<std::uint8_t> originalValue;
    std::optional<std::uint8_t> normalizedValue;
    std::string message;
};

enum class OscillatorMode : std::uint8_t
{
    ratio = 0,
    fixed = 1,
};

struct Operator
{
    std::array<std::uint8_t, 4> envelopeRates{};
    std::array<std::uint8_t, 4> envelopeLevels{};
    std::uint8_t keyLevelBreakpoint = 0;
    std::uint8_t keyLevelLeftDepth = 0;
    std::uint8_t keyLevelRightDepth = 0;
    std::uint8_t leftCurve = 0;
    std::uint8_t rightCurve = 0;
    std::uint8_t rateScaling = 0;
    std::uint8_t amplitudeModulationSensitivity = 0;
    std::uint8_t keyVelocitySensitivity = 0;
    std::uint8_t outputLevel = 0;
    OscillatorMode oscillatorMode = OscillatorMode::ratio;
    std::uint8_t frequencyCoarse = 0;
    std::uint8_t frequencyFine = 0;
    std::uint8_t detune = 0;
};

struct Voice
{
    // Logical order is operator 1 through operator 6, independent of serialized order.
    std::array<Operator, operatorCount> operators{};
    std::array<std::uint8_t, 4> pitchEnvelopeRates{};
    std::array<std::uint8_t, 4> pitchEnvelopeLevels{};
    std::uint8_t algorithm = 0;
    std::uint8_t feedback = 0;
    bool oscillatorSync = false;
    std::uint8_t lfoSpeed = 0;
    std::uint8_t lfoDelay = 0;
    std::uint8_t lfoPitchModulationDepth = 0;
    std::uint8_t lfoAmplitudeModulationDepth = 0;
    std::uint8_t lfoPitchModulationSensitivity = 0;
    std::uint8_t lfoWaveform = 0;
    bool lfoSync = false;
    std::uint8_t transpose = 0;
    std::string name;
};

struct BankMessage
{
    std::uint8_t channel = 0;
    bool raw = false;
    std::array<Voice, bankVoiceCount> voices{};
};

struct SingleVoiceMessage
{
    std::uint8_t channel = 0;
    Voice voice;
};

using MessageData = std::variant<BankMessage, SingleVoiceMessage>;

struct Message
{
    std::size_t byteOffset = 0;
    MessageData data;
};

struct ParseOptions
{
    bool strictParameters = false;
};

struct ParseResult
{
    std::vector<Message> messages;
    std::vector<ParseIssue> issues;

    [[nodiscard]] bool succeeded() const noexcept;
};

[[nodiscard]] ParseResult parse(std::span<const std::uint8_t> bytes, ParseOptions options = {});

} // namespace dxsyx
