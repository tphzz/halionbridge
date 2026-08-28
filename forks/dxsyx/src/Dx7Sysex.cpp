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

#include "dxsyx/Dx7Sysex.h"

#include <algorithm>
#include <iterator>
#include <numeric>
#include <sstream>
#include <string_view>

namespace dxsyx
{
namespace
{

constexpr auto kYamahaManufacturer = std::uint8_t{0x43};
constexpr auto kSingleVoiceFormat = std::uint8_t{0x00};
constexpr auto kPackedBankFormat = std::uint8_t{0x09};
constexpr auto kMessageOverhead = std::size_t{8};

struct DecodeContext
{
    ParseResult& result;
    ParseOptions options;
    std::size_t messageIndex = 0;
    std::size_t payloadOffset = 0;
};

void addStructuralIssue(ParseResult& result, const std::string_view code, const std::size_t byteOffset, const std::size_t messageIndex,
                        std::string message)
{
    result.issues.push_back(ParseIssue{IssueSeverity::error,
                                       std::string(code),
                                       byteOffset,
                                       messageIndex,
                                       std::nullopt,
                                       {},
                                       std::nullopt,
                                       std::nullopt,
                                       std::move(message)});
}

std::uint8_t normalizeParameter(DecodeContext& context, const std::uint8_t value, const std::uint8_t maximum, const std::size_t byteOffset,
                                const std::size_t voiceIndex, std::string field)
{
    if (value <= maximum)
        return value;

    auto message = std::ostringstream{};
    message << field << " value " << static_cast<int>(value) << " exceeds " << static_cast<int>(maximum);
    if (context.options.strictParameters)
    {
        message << "; the containing DX7 message was rejected because strict parameter validation is enabled.";
        context.result.issues.push_back(ParseIssue{IssueSeverity::error, "parameter-range", byteOffset, context.messageIndex, voiceIndex,
                                                   std::move(field), value, std::nullopt, message.str()});
        return maximum;
    }

    message << " and was normalized to " << static_cast<int>(maximum) << ".";
    context.result.issues.push_back(ParseIssue{IssueSeverity::warning, "parameter-range", byteOffset, context.messageIndex, voiceIndex,
                                               std::move(field), value, maximum, message.str()});
    return maximum;
}

std::string operatorField(const std::size_t logicalOperatorIndex, const std::string_view field)
{
    return "operator_" + std::to_string(logicalOperatorIndex + 1) + "." + std::string(field);
}

char decodeNameCharacter(const std::uint8_t value)
{
    if (value < 32)
        return '_';

    switch (value)
    {
    case 92:
        return 'Y';
    case 126:
        return '<';
    case 127:
        return '>';
    default:
        return static_cast<char>(value);
    }
}

std::string decodeName(const std::span<const std::uint8_t> bytes)
{
    auto name = std::string{};
    name.reserve(bytes.size());
    std::ranges::transform(bytes, std::back_inserter(name), decodeNameCharacter);
    return name;
}

Operator decodePackedOperator(const std::span<const std::uint8_t, 17> bytes, DecodeContext& context, const std::size_t voiceIndex,
                              const std::size_t logicalOperatorIndex, const std::size_t absoluteOffset)
{
    auto result = Operator{};
    for (auto index = std::size_t{0}; index < 4; ++index)
    {
        result.envelopeRates[index] = normalizeParameter(context, bytes[index], 99, absoluteOffset + index, voiceIndex,
                                                         operatorField(logicalOperatorIndex, "envelope_rate_" + std::to_string(index + 1)));
        result.envelopeLevels[index] =
            normalizeParameter(context, bytes[4 + index], 99, absoluteOffset + 4 + index, voiceIndex,
                               operatorField(logicalOperatorIndex, "envelope_level_" + std::to_string(index + 1)));
    }

    result.keyLevelBreakpoint = normalizeParameter(context, bytes[8], 99, absoluteOffset + 8, voiceIndex,
                                                   operatorField(logicalOperatorIndex, "key_level_breakpoint"));
    result.keyLevelLeftDepth = normalizeParameter(context, bytes[9], 99, absoluteOffset + 9, voiceIndex,
                                                  operatorField(logicalOperatorIndex, "key_level_left_depth"));
    result.keyLevelRightDepth = normalizeParameter(context, bytes[10], 99, absoluteOffset + 10, voiceIndex,
                                                   operatorField(logicalOperatorIndex, "key_level_right_depth"));

    const auto curves = static_cast<std::uint8_t>(bytes[11] & 0x0fU);
    result.leftCurve = static_cast<std::uint8_t>(curves & 0x03U);
    result.rightCurve = static_cast<std::uint8_t>((curves >> 2U) & 0x03U);

    const auto detuneAndRate = bytes[12];
    result.rateScaling = static_cast<std::uint8_t>(detuneAndRate & 0x07U);
    result.detune = normalizeParameter(context, static_cast<std::uint8_t>((detuneAndRate >> 3U) & 0x0fU), 14, absoluteOffset + 12,
                                       voiceIndex, operatorField(logicalOperatorIndex, "detune"));

    const auto sensitivities = static_cast<std::uint8_t>(bytes[13] & 0x1fU);
    result.amplitudeModulationSensitivity = static_cast<std::uint8_t>(sensitivities & 0x03U);
    result.keyVelocitySensitivity = static_cast<std::uint8_t>((sensitivities >> 2U) & 0x07U);
    result.outputLevel =
        normalizeParameter(context, bytes[14], 99, absoluteOffset + 14, voiceIndex, operatorField(logicalOperatorIndex, "output_level"));

    const auto frequency = static_cast<std::uint8_t>(bytes[15] & 0x3fU);
    result.oscillatorMode = (frequency & 0x01U) == 0 ? OscillatorMode::ratio : OscillatorMode::fixed;
    result.frequencyCoarse = static_cast<std::uint8_t>((frequency >> 1U) & 0x1fU);
    result.frequencyFine =
        normalizeParameter(context, bytes[16], 99, absoluteOffset + 16, voiceIndex, operatorField(logicalOperatorIndex, "frequency_fine"));
    return result;
}

Operator decodeSingleOperator(const std::span<const std::uint8_t, 21> bytes, DecodeContext& context, const std::size_t voiceIndex,
                              const std::size_t logicalOperatorIndex, const std::size_t absoluteOffset)
{
    auto result = Operator{};
    for (auto index = std::size_t{0}; index < 4; ++index)
    {
        result.envelopeRates[index] = normalizeParameter(context, bytes[index], 99, absoluteOffset + index, voiceIndex,
                                                         operatorField(logicalOperatorIndex, "envelope_rate_" + std::to_string(index + 1)));
        result.envelopeLevels[index] =
            normalizeParameter(context, bytes[4 + index], 99, absoluteOffset + 4 + index, voiceIndex,
                               operatorField(logicalOperatorIndex, "envelope_level_" + std::to_string(index + 1)));
    }

    result.keyLevelBreakpoint = normalizeParameter(context, bytes[8], 99, absoluteOffset + 8, voiceIndex,
                                                   operatorField(logicalOperatorIndex, "key_level_breakpoint"));
    result.keyLevelLeftDepth = normalizeParameter(context, bytes[9], 99, absoluteOffset + 9, voiceIndex,
                                                  operatorField(logicalOperatorIndex, "key_level_left_depth"));
    result.keyLevelRightDepth = normalizeParameter(context, bytes[10], 99, absoluteOffset + 10, voiceIndex,
                                                   operatorField(logicalOperatorIndex, "key_level_right_depth"));
    result.leftCurve =
        normalizeParameter(context, bytes[11], 3, absoluteOffset + 11, voiceIndex, operatorField(logicalOperatorIndex, "left_curve"));
    result.rightCurve =
        normalizeParameter(context, bytes[12], 3, absoluteOffset + 12, voiceIndex, operatorField(logicalOperatorIndex, "right_curve"));
    result.rateScaling =
        normalizeParameter(context, bytes[13], 7, absoluteOffset + 13, voiceIndex, operatorField(logicalOperatorIndex, "rate_scaling"));
    result.amplitudeModulationSensitivity = normalizeParameter(context, bytes[14], 3, absoluteOffset + 14, voiceIndex,
                                                               operatorField(logicalOperatorIndex, "amplitude_modulation_sensitivity"));
    result.keyVelocitySensitivity = normalizeParameter(context, bytes[15], 7, absoluteOffset + 15, voiceIndex,
                                                       operatorField(logicalOperatorIndex, "key_velocity_sensitivity"));
    result.outputLevel =
        normalizeParameter(context, bytes[16], 99, absoluteOffset + 16, voiceIndex, operatorField(logicalOperatorIndex, "output_level"));
    const auto oscillatorMode =
        normalizeParameter(context, bytes[17], 1, absoluteOffset + 17, voiceIndex, operatorField(logicalOperatorIndex, "oscillator_mode"));
    result.oscillatorMode = oscillatorMode == 0 ? OscillatorMode::ratio : OscillatorMode::fixed;
    result.frequencyCoarse = normalizeParameter(context, bytes[18], 31, absoluteOffset + 18, voiceIndex,
                                                operatorField(logicalOperatorIndex, "frequency_coarse"));
    result.frequencyFine =
        normalizeParameter(context, bytes[19], 99, absoluteOffset + 19, voiceIndex, operatorField(logicalOperatorIndex, "frequency_fine"));
    result.detune =
        normalizeParameter(context, bytes[20], 14, absoluteOffset + 20, voiceIndex, operatorField(logicalOperatorIndex, "detune"));
    return result;
}

Voice decodePackedVoice(const std::span<const std::uint8_t, packedVoiceSize> bytes, DecodeContext& context, const std::size_t voiceIndex,
                        const std::size_t absoluteOffset)
{
    auto voice = Voice{};
    for (auto serializedOperator = std::size_t{0}; serializedOperator < operatorCount; ++serializedOperator)
    {
        const auto logicalOperator = operatorCount - 1 - serializedOperator;
        voice.operators[logicalOperator] =
            decodePackedOperator(std::span<const std::uint8_t, 17>{bytes.subspan(serializedOperator * 17, 17)}, context, voiceIndex,
                                 logicalOperator, absoluteOffset + (serializedOperator * 17));
    }

    for (auto index = std::size_t{0}; index < 4; ++index)
    {
        voice.pitchEnvelopeRates[index] = normalizeParameter(context, bytes[102 + index], 99, absoluteOffset + 102 + index, voiceIndex,
                                                             "pitch_envelope_rate_" + std::to_string(index + 1));
        voice.pitchEnvelopeLevels[index] = normalizeParameter(context, bytes[106 + index], 99, absoluteOffset + 106 + index, voiceIndex,
                                                              "pitch_envelope_level_" + std::to_string(index + 1));
    }

    voice.algorithm = static_cast<std::uint8_t>(bytes[110] & 0x1fU);
    const auto feedbackAndSync = static_cast<std::uint8_t>(bytes[111] & 0x0fU);
    voice.feedback = static_cast<std::uint8_t>(feedbackAndSync & 0x07U);
    voice.oscillatorSync = ((feedbackAndSync >> 3U) & 0x01U) != 0;
    voice.lfoSpeed = normalizeParameter(context, bytes[112], 99, absoluteOffset + 112, voiceIndex, "lfo_speed");
    voice.lfoDelay = normalizeParameter(context, bytes[113], 99, absoluteOffset + 113, voiceIndex, "lfo_delay");
    voice.lfoPitchModulationDepth =
        normalizeParameter(context, bytes[114], 99, absoluteOffset + 114, voiceIndex, "lfo_pitch_modulation_depth");
    voice.lfoAmplitudeModulationDepth =
        normalizeParameter(context, bytes[115], 99, absoluteOffset + 115, voiceIndex, "lfo_amplitude_modulation_depth");

    const auto lfo = bytes[116];
    voice.lfoSync = (lfo & 0x01U) != 0;
    voice.lfoWaveform =
        normalizeParameter(context, static_cast<std::uint8_t>((lfo >> 1U) & 0x07U), 5, absoluteOffset + 116, voiceIndex, "lfo_waveform");
    voice.lfoPitchModulationSensitivity = static_cast<std::uint8_t>((lfo >> 4U) & 0x07U);
    voice.transpose = normalizeParameter(context, bytes[117], 48, absoluteOffset + 117, voiceIndex, "transpose");
    voice.name = decodeName(bytes.subspan(118, 10));
    return voice;
}

Voice decodeSingleVoice(const std::span<const std::uint8_t, singleVoiceSize> bytes, DecodeContext& context,
                        const std::size_t absoluteOffset)
{
    auto voice = Voice{};
    constexpr auto voiceIndex = std::size_t{0};
    for (auto serializedOperator = std::size_t{0}; serializedOperator < operatorCount; ++serializedOperator)
    {
        const auto logicalOperator = operatorCount - 1 - serializedOperator;
        voice.operators[logicalOperator] =
            decodeSingleOperator(std::span<const std::uint8_t, 21>{bytes.subspan(serializedOperator * 21, 21)}, context, voiceIndex,
                                 logicalOperator, absoluteOffset + (serializedOperator * 21));
    }

    for (auto index = std::size_t{0}; index < 4; ++index)
    {
        voice.pitchEnvelopeRates[index] = normalizeParameter(context, bytes[126 + index], 99, absoluteOffset + 126 + index, voiceIndex,
                                                             "pitch_envelope_rate_" + std::to_string(index + 1));
        voice.pitchEnvelopeLevels[index] = normalizeParameter(context, bytes[130 + index], 99, absoluteOffset + 130 + index, voiceIndex,
                                                              "pitch_envelope_level_" + std::to_string(index + 1));
    }

    voice.algorithm = normalizeParameter(context, bytes[134], 31, absoluteOffset + 134, voiceIndex, "algorithm");
    voice.feedback = normalizeParameter(context, bytes[135], 7, absoluteOffset + 135, voiceIndex, "feedback");
    voice.oscillatorSync = normalizeParameter(context, bytes[136], 1, absoluteOffset + 136, voiceIndex, "oscillator_sync") != 0;
    voice.lfoSpeed = normalizeParameter(context, bytes[137], 99, absoluteOffset + 137, voiceIndex, "lfo_speed");
    voice.lfoDelay = normalizeParameter(context, bytes[138], 99, absoluteOffset + 138, voiceIndex, "lfo_delay");
    voice.lfoPitchModulationDepth =
        normalizeParameter(context, bytes[139], 99, absoluteOffset + 139, voiceIndex, "lfo_pitch_modulation_depth");
    voice.lfoAmplitudeModulationDepth =
        normalizeParameter(context, bytes[140], 99, absoluteOffset + 140, voiceIndex, "lfo_amplitude_modulation_depth");
    voice.lfoSync = normalizeParameter(context, bytes[141], 1, absoluteOffset + 141, voiceIndex, "lfo_sync") != 0;
    voice.lfoWaveform = normalizeParameter(context, bytes[142], 5, absoluteOffset + 142, voiceIndex, "lfo_waveform");
    voice.lfoPitchModulationSensitivity =
        normalizeParameter(context, bytes[143], 7, absoluteOffset + 143, voiceIndex, "lfo_pitch_modulation_sensitivity");
    voice.transpose = normalizeParameter(context, bytes[144], 48, absoluteOffset + 144, voiceIndex, "transpose");
    voice.name = decodeName(bytes.subspan(145, 10));
    return voice;
}

bool hasSevenBitData(const std::span<const std::uint8_t> bytes, ParseResult& result, const std::size_t absoluteOffset,
                     const std::size_t messageIndex)
{
    for (auto index = std::size_t{0}; index < bytes.size(); ++index)
    {
        if (bytes[index] <= 0x7fU)
            continue;

        addStructuralIssue(result, "non-seven-bit", absoluteOffset + index, messageIndex,
                           "DX7 SysEx payload and checksum bytes must be seven-bit values.");
        return false;
    }
    return true;
}

bool checksumIsValid(const std::span<const std::uint8_t> payload, const std::uint8_t checksum)
{
    const auto sum = std::accumulate(payload.begin(), payload.end(), std::uint32_t{checksum});
    return (sum & 0x7fU) == 0;
}

void decodeBank(const std::span<const std::uint8_t, packedBankSize> payload, DecodeContext& context, const std::uint8_t channel,
                const bool raw, const std::size_t messageOffset)
{
    auto bank = BankMessage{.channel = channel, .raw = raw};
    for (auto voiceIndex = std::size_t{0}; voiceIndex < bankVoiceCount; ++voiceIndex)
    {
        const auto voiceOffset = voiceIndex * packedVoiceSize;
        bank.voices[voiceIndex] =
            decodePackedVoice(std::span<const std::uint8_t, packedVoiceSize>{payload.subspan(voiceOffset, packedVoiceSize)}, context,
                              voiceIndex, context.payloadOffset + voiceOffset);
    }
    context.result.messages.push_back(Message{messageOffset, std::move(bank), context.messageIndex});
}

bool parseFramedMessage(const std::span<const std::uint8_t> bytes, std::size_t& offset, const std::size_t messageIndex,
                        const ParseOptions options, ParseResult& result)
{
    if (bytes[offset] != 0xf0U)
    {
        addStructuralIssue(result, "unsupported-data", offset, messageIndex,
                           "Every byte must belong to a supported framed Yamaha DX7 SysEx message.");
        return false;
    }

    if (bytes.size() - offset < kMessageOverhead)
    {
        addStructuralIssue(result, "truncated-message", offset, messageIndex, "DX7 SysEx message is truncated.");
        return false;
    }

    if (bytes[offset + 1] != kYamahaManufacturer)
    {
        addStructuralIssue(result, "manufacturer", offset + 1, messageIndex, "SysEx message is not a Yamaha message.");
        return false;
    }

    if (bytes[offset + 2] > 0x0fU)
    {
        addStructuralIssue(result, "substatus", offset + 2, messageIndex, "DX7 bulk-dump substatus must contain a channel nibble.");
        return false;
    }

    const auto format = bytes[offset + 3];
    const auto declaredSize = (static_cast<std::size_t>(bytes[offset + 4]) << 7U) | bytes[offset + 5];
    const auto expectedSize =
        format == kPackedBankFormat ? packedBankSize : (format == kSingleVoiceFormat ? singleVoiceSize : std::size_t{0});
    if (expectedSize == 0)
    {
        addStructuralIssue(result, "unsupported-format", offset + 3, messageIndex,
                           "Only Yamaha DX7 single-voice and 32-voice bank messages are supported.");
        return false;
    }

    if (declaredSize != expectedSize)
    {
        addStructuralIssue(result, "byte-count", offset + 4, messageIndex,
                           "DX7 SysEx byte-count field does not match the selected message format.");
        return false;
    }

    const auto totalSize = expectedSize + kMessageOverhead;
    if (bytes.size() - offset < totalSize)
    {
        addStructuralIssue(result, "truncated-message", offset, messageIndex, "DX7 SysEx message is truncated.");
        return false;
    }

    const auto payloadOffset = offset + 6;
    const auto payload = bytes.subspan(payloadOffset, expectedSize);
    const auto checksumOffset = payloadOffset + expectedSize;
    if (!hasSevenBitData(bytes.subspan(payloadOffset, expectedSize + 1), result, payloadOffset, messageIndex))
        return false;

    if (bytes[offset + totalSize - 1] != 0xf7U)
    {
        addStructuralIssue(result, "terminator", offset + totalSize - 1, messageIndex,
                           "DX7 SysEx message is missing its terminator at the declared boundary.");
        return false;
    }

    if (!checksumIsValid(payload, bytes[checksumOffset]))
    {
        addStructuralIssue(result, "checksum", checksumOffset, messageIndex, "DX7 SysEx checksum is invalid.");
        return false;
    }

    auto context = DecodeContext{result, options, messageIndex, payloadOffset};
    if (format == kPackedBankFormat)
    {
        decodeBank(std::span<const std::uint8_t, packedBankSize>{payload}, context, bytes[offset + 2], false, offset);
    }
    else
    {
        auto single = SingleVoiceMessage{
            bytes[offset + 2], decodeSingleVoice(std::span<const std::uint8_t, singleVoiceSize>{payload}, context, payloadOffset)};
        result.messages.push_back(Message{offset, std::move(single), messageIndex});
    }

    offset += totalSize;
    return true;
}

void appendRecoveredIssues(ParseResult& destination, std::vector<ParseIssue> issues)
{
    for (auto& issue : issues)
    {
        issue.severity = IssueSeverity::warning;
        destination.issues.push_back(std::move(issue));
    }
}

std::size_t findByte(const std::span<const std::uint8_t> bytes, const std::size_t start, const std::uint8_t value)
{
    for (auto index = start; index < bytes.size(); ++index)
    {
        if (bytes[index] == value)
            return index;
    }
    return bytes.size();
}

std::size_t recoveryOffsetAfterMessage(const std::span<const std::uint8_t> bytes, const std::size_t offset)
{
    const auto nextMessage = findByte(bytes, offset + 1, 0xf0U);
    const auto terminator = findByte(bytes, offset + 1, 0xf7U);
    if (terminator < nextMessage)
        return terminator + 1;
    return nextMessage;
}

ParseResult parseRecovering(const std::span<const std::uint8_t> bytes, const ParseOptions options)
{
    auto result = ParseResult{};
    auto offset = std::size_t{0};
    auto messageIndex = std::size_t{0};
    while (offset < bytes.size())
    {
        if (bytes[offset] != 0xf0U)
        {
            const auto nextMessage = findByte(bytes, offset + 1, 0xf0U);
            addStructuralIssue(result, "unsupported-data", offset, messageIndex,
                               "Skipped bytes that do not belong to a framed Yamaha DX7 SysEx message.");
            result.issues.back().severity = IssueSeverity::warning;
            ++result.fragmentsSkipped;
            offset = nextMessage;
            continue;
        }

        ++result.messagesSeen;
        auto candidate = ParseResult{};
        auto candidateOffset = offset;
        const auto structurallyParsed = parseFramedMessage(bytes, candidateOffset, messageIndex, options, candidate);
        if (structurallyParsed && candidate.succeeded())
        {
            result.messages.insert(result.messages.end(), std::make_move_iterator(candidate.messages.begin()),
                                   std::make_move_iterator(candidate.messages.end()));
            result.issues.insert(result.issues.end(), std::make_move_iterator(candidate.issues.begin()),
                                 std::make_move_iterator(candidate.issues.end()));
            offset = candidateOffset;
        }
        else
        {
            appendRecoveredIssues(result, std::move(candidate.issues));
            ++result.messagesSkipped;
            offset = recoveryOffsetAfterMessage(bytes, offset);
        }
        ++messageIndex;
    }
    return result;
}

} // namespace

bool ParseResult::succeeded() const noexcept
{
    return std::ranges::none_of(issues, [](const ParseIssue& issue) { return issue.severity == IssueSeverity::error; });
}

ParseResult parse(const std::span<const std::uint8_t> bytes, const ParseOptions options)
{
    auto result = ParseResult{};
    if (bytes.empty())
    {
        addStructuralIssue(result, "empty-input", 0, 0, "DX7 SysEx input is empty.");
        return result;
    }

    if (bytes.size() == packedBankSize)
    {
        if (!hasSevenBitData(bytes, result, 0, 0))
        {
            if (options.continueOnError)
            {
                auto issues = std::move(result.issues);
                result.issues.clear();
                appendRecoveredIssues(result, std::move(issues));
                result.messagesSeen = 1;
                result.messagesSkipped = 1;
            }
            return result;
        }

        auto context = DecodeContext{result, options, 0, 0};
        decodeBank(std::span<const std::uint8_t, packedBankSize>{bytes}, context, 0, true, 0);
        result.messagesSeen = 1;
        if (options.continueOnError && !result.succeeded())
        {
            result.messages.clear();
            auto issues = std::move(result.issues);
            result.issues.clear();
            appendRecoveredIssues(result, std::move(issues));
            result.messagesSkipped = 1;
        }
        return result;
    }

    if (options.continueOnError)
        return parseRecovering(bytes, options);

    auto offset = std::size_t{0};
    auto messageIndex = std::size_t{0};
    while (offset < bytes.size())
    {
        if (!parseFramedMessage(bytes, offset, messageIndex, options, result))
            break;
        ++messageIndex;
    }

    result.messagesSeen = messageIndex + (offset < bytes.size() && bytes[offset] == 0xf0U ? 1U : 0U);

    return result;
}

} // namespace dxsyx
