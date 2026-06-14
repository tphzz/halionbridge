#include "halionbridge/Bridge.h"
#include "halionbridge/BuildInfo.h"
#include "BuildFile.h"
#include "Log.h"
#include "ChildProcessOutput.h"
#include "PathUtils.h"
#include "PluginScan.h"
#include "ProgressMarkers.h"
#if HALIONBRIDGE_ENABLE_CONVERTERS
#include "halionbridge_converters/BuildDirectoryEmitter.h"
#include "halionbridge_converters/Converter.h"
#include "halionbridge_converters/sfz/SfzConverter.h"
#endif
#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kHoldScriptLockArgument = "--halionbridge-tests-hold-script-lock";
constexpr const char* kScriptDirectoryLockName = "halionbridge_halion_user_scripts";

juce::File gTestTempRoot;

bool contains(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::span<const std::byte> asBytes(const juce::MemoryBlock& data)
{
    return {reinterpret_cast<const std::byte*>(data.getData()), data.getSize()};
}

juce::File cleanTempDirectory(const char* name)
{
    if (!gTestTempRoot.isDirectory())
    {
        gTestTempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("halionbridge_tests", "", false);
        gTestTempRoot.createDirectory();
    }

    auto directory = gTestTempRoot.getChildFile(name);
    directory.deleteRecursively();
    return directory;
}

bool waitForFileToExist(const juce::File& file, const int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        if (file.existsAsFile())
            return true;

        juce::Thread::sleep(10);
    }

    return file.existsAsFile();
}

#if HALIONBRIDGE_ENABLE_CONVERTERS
bool containsDiagnosticCode(const std::vector<halionbridge::converters::Diagnostic>& diagnostics, const std::string_view code)
{
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [code](const halionbridge::converters::Diagnostic& diagnostic) { return diagnostic.code == code; });
}
#endif

int runScriptLockHolder(const juce::String& readyFilePath, const juce::String& releaseFilePath)
{
    juce::InterProcessLock lock(kScriptDirectoryLockName);
    if (!lock.enter(5000))
        return 2;

    const auto readyFile = juce::File(readyFilePath);
    const auto releaseFile = juce::File(releaseFilePath);

    if (!readyFile.replaceWithText("ready"))
        return 3;

    const auto released = waitForFileToExist(releaseFile, 30000);
    lock.exit();
    return released ? 0 : 4;
}

} // namespace

class ConsoleLogger : public juce::Logger
{
    void logMessage(const juce::String& message) override
    {
        std::cout << message.toStdString() << std::endl;
    }
};

void configureQuietRuntimeLogger()
{
    halionbridge::log::configure(halionbridge::log::Level::off);
}

class BridgeTests : public juce::UnitTest
{
  public:
    BridgeTests() : juce::UnitTest("BridgeTests", "halionbridge") {}

    void runTest() override
    {
        beginTest("Argument Parsing - Missing build directory");
        {
            std::vector<std::string> args;
            auto options = halionbridge::Bridge::parseArguments(args);
            expect(!options.has_value());

            halionbridge::Bridge bridge;
            expect(bridge.runDetailed(halionbridge::AppOptions{}) == halionbridge::RunResult::invalidOptions);
        }

        beginTest("Bridge Run - Moved-from bridge reports invalid bridge");
        {
            halionbridge::Bridge source;
            halionbridge::Bridge moved{std::move(source)};
            juce::ignoreUnused(moved);

            expect(source.runDetailed(halionbridge::AppOptions{}) == halionbridge::RunResult::invalidBridge);
        }

        beginTest("Argument Parsing - Positional build directory");
        {
            auto tempDir = cleanTempDirectory("halionbridge_build_dir");
            expect(tempDir.createDirectory());
            auto buildFile = tempDir.getChildFile("halionbridge_build.lua");
            expect(buildFile.replaceWithText("return {}"));

            auto options = halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString()});
            expect(options.has_value());
            if (options)
            {
                expect(options->buildDirectory.has_value());
                expect(*options->buildDirectory == halionbridge::detail::toStdPath(tempDir));
                expectEquals(options->timeoutSeconds, 0);
                expect(!options->noKill);
                expect(!options->forceScan);
            }

            buildFile.deleteFile();
            expect(!halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString()}).has_value());

            tempDir.deleteRecursively();
        }

        beginTest("Argument Parsing - Multiple positional directories");
        {
            auto firstDir = cleanTempDirectory("halionbridge_first_build_dir");
            auto secondDir = cleanTempDirectory("halionbridge_second_build_dir");
            firstDir.createDirectory();
            secondDir.createDirectory();
            firstDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");
            secondDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");

            auto options =
                halionbridge::Bridge::parseArguments({firstDir.getFullPathName().toStdString(), secondDir.getFullPathName().toStdString()});
            expect(!options.has_value());

            firstDir.deleteRecursively();
            secondDir.deleteRecursively();
        }

        beginTest("Argument Parsing - Missing directory");
        {
            auto missingDirectory = cleanTempDirectory("nonexistent_halionbridge_build_dir");
            missingDirectory.deleteRecursively();

            std::vector<std::string> args = {missingDirectory.getFullPathName().toStdString()};
            auto options = halionbridge::Bridge::parseArguments(args);
            expect(!options.has_value());
        }

        beginTest("Argument Parsing - Timeout seconds");
        {
            auto tempDir = cleanTempDirectory("halionbridge_timeout_build_dir");
            tempDir.createDirectory();
            tempDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");

            {
                std::vector<std::string> args = {tempDir.getFullPathName().toStdString(), "--timeout-seconds", "0"};
                auto options = halionbridge::Bridge::parseArguments(args);
                expect(options.has_value());
                if (options)
                    expectEquals(options->timeoutSeconds, 0);
            }

            {
                std::vector<std::string> args = {tempDir.getFullPathName().toStdString(), "--timeout-seconds", "45"};
                auto options = halionbridge::Bridge::parseArguments(args);
                expect(options.has_value());
                if (options)
                    expectEquals(options->timeoutSeconds, 45);
            }

            {
                std::vector<std::string> args = {tempDir.getFullPathName().toStdString(), "--timeout-seconds", "abc"};
                auto options = halionbridge::Bridge::parseArguments(args);
                expect(!options.has_value());
            }

            {
                std::vector<std::string> args = {tempDir.getFullPathName().toStdString(), "--timeout-seconds", "99999999999999999999"};
                auto options = halionbridge::Bridge::parseArguments(args);
                expect(!options.has_value());
            }

            {
                std::vector<std::string> args = {tempDir.getFullPathName().toStdString(), "--timeout-seconds"};
                auto options = halionbridge::Bridge::parseArguments(args);
                expect(!options.has_value());
            }

            tempDir.deleteRecursively();
        }

        beginTest("Argument Parsing - No-kill inspection hold");
        {
            auto tempDir = cleanTempDirectory("halionbridge_nokill_build_dir");
            tempDir.createDirectory();
            tempDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");

            auto options = halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString(), "--nokill"});
            expect(options.has_value());
            if (options)
                expect(options->noKill);

            tempDir.deleteRecursively();
        }

        beginTest("Argument Parsing - Forced plugin scan");
        {
            auto tempDir = cleanTempDirectory("halionbridge_force_scan_build_dir");
            tempDir.createDirectory();
            tempDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");

            auto options = halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString(), "--force-scan"});
            expect(options.has_value());
            if (options)
                expect(options->forceScan);

            tempDir.deleteRecursively();
        }

        beginTest("Bridge Run - Concurrent runtime staging is rejected");
        {
            auto tempDir = cleanTempDirectory("halionbridge_lock_build_dir");
            tempDir.createDirectory();
            tempDir.getChildFile("halionbridge_build.lua").replaceWithText("return {}");

            auto lockHolderDir = cleanTempDirectory("halionbridge_lock_holder");
            expect(lockHolderDir.createDirectory());
            const auto readyFile = lockHolderDir.getChildFile("ready.txt");
            const auto releaseFile = lockHolderDir.getChildFile("release.txt");

            juce::ChildProcess lockHolder;
            const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
            const auto childStarted = lockHolder.start(juce::StringArray{executable.getFullPathName(), kHoldScriptLockArgument,
                                                                         readyFile.getFullPathName(), releaseFile.getFullPathName()});
            expect(childStarted);
            expect(childStarted && waitForFileToExist(readyFile, 5000));

            halionbridge::AppOptions options;
            options.buildDirectory = halionbridge::detail::toStdPath(tempDir);

            halionbridge::Bridge bridge;
            expect(bridge.runDetailed(options) == halionbridge::RunResult::anotherInstanceRunning);

            expect(releaseFile.replaceWithText("release"));
            if (childStarted)
            {
                expect(lockHolder.waitForProcessToFinish(5000));
                expectEquals(static_cast<int>(lockHolder.getExitCode()), 0);
                if (lockHolder.isRunning())
                    lockHolder.kill();
            }

            lockHolderDir.deleteRecursively();
            tempDir.deleteRecursively();
        }

        beginTest("Argument Parsing - Missing values and unknown arguments");
        {
            expect(!halionbridge::Bridge::parseArguments({"--plugin"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--unknown"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--build-dir"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--state"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--vstpreset"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--plugin-only"}).has_value());
            expect(!halionbridge::Bridge::parseArguments({"--plugin-class-id"}).has_value());
        }

        beginTest("Build File Module Name Parsing");
        {
            auto names = halionbridge::Bridge::parseBuildFileModuleNames(R"(
                local ignored = "not_a_build_entry"
                -- "commented_module"
                --[[
                    "block_commented_module"
                ]]
                --[==[
                    "leveled_block_commented_module"
                ]==]
                return {
                    "jad_kick_builder",
                    'snare_builder.lua',
                    "escaped\\path.lua",
                    "line\nbreak.lua",
                    "quote\"module.lua",
                    nested = { "nested_module" },
                    label = "metadata literal",
                    "jad_kick_builder"
                }
            )");

            expectEquals(static_cast<int>(names.size()), 5);
            expect(contains(names, "jad_kick_builder"));
            expect(contains(names, "snare_builder.lua"));
            expect(contains(names, "escaped\\path.lua"));
            expect(contains(names, "line\nbreak.lua"));
            expect(contains(names, "quote\"module.lua"));
            expect(!contains(names, "not_a_build_entry"));
            expect(!contains(names, "commented_module"));
            expect(!contains(names, "block_commented_module"));
            expect(!contains(names, "leveled_block_commented_module"));
            expect(!contains(names, "nested_module"));
            expect(!contains(names, "metadata literal"));
        }

        beginTest("Build File Generation - creates sorted top-level index");
        {
            auto tempDir = cleanTempDirectory("halionbridge_build_file_generation");
            expect(tempDir.createDirectory());
            expect(tempDir.getChildFile("002_second.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("001_first.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("halionbridge_runtime.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("halionbridge_builder.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("builder_bootstrap.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("halionbridge-sfz.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("HALIONBRIDGE_RUNTIME.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("HALIONBRIDGE_BUILDER.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("BUILDER_BOOTSTRAP.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("HALIONBRIDGE-SFZ.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("nested").createDirectory());
            expect(tempDir.getChildFile("nested").getChildFile("000_nested.lua").replaceWithText("return {}"));

            const auto result = halionbridge::detail::generateBuildFile(tempDir, false);
            expect(result.succeeded);
            expectEquals(static_cast<int>(result.moduleNames.size()), 2);
            if (result.moduleNames.size() == 2)
            {
                expectEquals(result.moduleNames[0], juce::String("001_first.lua"));
                expectEquals(result.moduleNames[1], juce::String("002_second.lua"));
            }

            const auto generatedNames = halionbridge::Bridge::parseBuildFileModuleNames(result.buildFile.loadFileAsString().toStdString());
            expectEquals(static_cast<int>(generatedNames.size()), 2);
            expect(contains(generatedNames, "001_first.lua"));
            expect(contains(generatedNames, "002_second.lua"));
            expect(!contains(generatedNames, "halionbridge_runtime.lua"));
            expect(!contains(generatedNames, "halionbridge_builder.lua"));
            expect(!contains(generatedNames, "builder_bootstrap.lua"));
            expect(!contains(generatedNames, "halionbridge-sfz.lua"));
            expect(!contains(generatedNames, "HALIONBRIDGE_RUNTIME.lua"));
            expect(!contains(generatedNames, "HALIONBRIDGE_BUILDER.lua"));
            expect(!contains(generatedNames, "BUILDER_BOOTSTRAP.lua"));
            expect(!contains(generatedNames, "HALIONBRIDGE-SFZ.lua"));
            expect(!contains(generatedNames, "000_nested.lua"));

            tempDir.deleteRecursively();
        }

        beginTest("Init Command - validates arguments and warns about helper modules");
        {
            auto tempDir = cleanTempDirectory("halionbridge_init_command");
            expect(tempDir.createDirectory());
            expect(tempDir.getChildFile("001_first.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("helper.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("halionbridge-sfz.lua").replaceWithText("return {}"));

            auto result = halionbridge::detail::runInitCommand(juce::StringArray{"init"});
            expectEquals(result.exitCode, 1);
            expect(result.message.contains("requires a build directory"));

            result = halionbridge::detail::runInitCommand(juce::StringArray{"init", "--unknown"});
            expectEquals(result.exitCode, 1);
            expect(result.message.contains("Unknown init argument"));

            result = halionbridge::detail::runInitCommand(juce::StringArray{
                "init", tempDir.getFullPathName(), cleanTempDirectory("halionbridge_init_command_other").getFullPathName()});
            expectEquals(result.exitCode, 1);
            expect(result.message.contains("exactly one build directory"));

            const auto missingDir = cleanTempDirectory("halionbridge_init_command_missing");
            missingDir.deleteRecursively();
            result = halionbridge::detail::runInitCommand(juce::StringArray{"init", missingDir.getFullPathName()});
            expectEquals(result.exitCode, 1);
            expect(result.message.contains("does not exist"));

            result = halionbridge::detail::runInitCommand(juce::StringArray{"init", tempDir.getFullPathName()});
            expectEquals(result.exitCode, 0);
            expectEquals(static_cast<int>(result.moduleNames.size()), 2);
            expect(result.warning.contains("helper modules"));
            expect(tempDir.getChildFile("halionbridge_build.lua").existsAsFile());
            expect(tempDir.getChildFile("halionbridge_build.lua").loadFileAsString().contains("helper.lua"));
            expect(!tempDir.getChildFile("halionbridge_build.lua").loadFileAsString().contains("halionbridge-sfz.lua"));

            result = halionbridge::detail::runInitCommand(juce::StringArray{"init", tempDir.getFullPathName()});
            expectEquals(result.exitCode, 1);
            expect(result.message.contains("already exists"));

            expect(tempDir.getChildFile("halionbridge_build.lua").replaceWithText("return { \"manual.lua\" }\n"));
            result = halionbridge::detail::runInitCommand(juce::StringArray{"init", tempDir.getFullPathName(), "--overwrite"});
            expectEquals(result.exitCode, 0);
            expect(tempDir.getChildFile("halionbridge_build.lua").loadFileAsString().contains("001_first.lua"));
            expect(!tempDir.getChildFile("halionbridge_build.lua").loadFileAsString().contains("manual.lua"));

            tempDir.deleteRecursively();
        }

        beginTest("Build File Generation - refuses and overwrites existing file explicitly");
        {
            auto tempDir = cleanTempDirectory("halionbridge_build_file_overwrite");
            expect(tempDir.createDirectory());
            const auto buildFile = tempDir.getChildFile("halionbridge_build.lua");
            expect(tempDir.getChildFile("001_first.lua").replaceWithText("return {}"));
            expect(buildFile.replaceWithText("return { \"manual.lua\" }\n"));

            auto result = halionbridge::detail::generateBuildFile(tempDir, false);
            expect(!result.succeeded);
            expect(buildFile.loadFileAsString().contains("manual.lua"));

            result = halionbridge::detail::generateBuildFile(tempDir, true);
            expect(result.succeeded);
            expect(buildFile.loadFileAsString().contains("001_first.lua"));
            expect(!buildFile.loadFileAsString().contains("manual.lua"));

            tempDir.deleteRecursively();
        }

        beginTest("Build File Generation - missing index with Lua files fails normal build parsing");
        {
            auto tempDir = cleanTempDirectory("halionbridge_missing_build_file_with_lua");
            expect(tempDir.createDirectory());
            expect(tempDir.getChildFile("001_first.lua").replaceWithText("return {}"));
            expect(halionbridge::detail::hasTopLevelLuaBuildScripts(tempDir));
            expect(!halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString()}).has_value());

            tempDir.deleteRecursively();
        }

        beginTest("Build File Generation - empty directory has no Lua build scripts");
        {
            auto tempDir = cleanTempDirectory("halionbridge_missing_build_file_empty");
            expect(tempDir.createDirectory());
            expect(!halionbridge::detail::hasTopLevelLuaBuildScripts(tempDir));
            expect(!halionbridge::Bridge::parseArguments({tempDir.getFullPathName().toStdString()}).has_value());

            tempDir.deleteRecursively();
        }

#if HALIONBRIDGE_ENABLE_CONVERTERS
        beginTest("Converter Registry - compiled converters are listed");
        {
            halionbridge::converters::ConverterRegistry registry;
            halionbridge::converters::registerCompiledConverters(registry);

            const auto converters = registry.list();
            expect(!converters.empty());
            expect(registry.find("sfz") != nullptr);
            expect(registry.find("missing") == nullptr);

            auto duplicate =
                halionbridge::converters::ConverterDefinition{"sfz", "Duplicate", "Duplicate", converters.front().run, nullptr};
            expect(!registry.registerConverter(duplicate));
        }

        beginTest("SFZ Helper Lua - exposes conservative v1 API");
        {
            const auto helperFile = juce::File::getCurrentWorkingDirectory()
                                        .getChildFile("converters")
                                        .getChildFile("sfz")
                                        .getChildFile("lua")
                                        .getChildFile("halionbridge-sfz.lua");
            expect(helperFile.existsAsFile());

            const auto source = helperFile.loadFileAsString();
            expect(source.contains("hb.version = 1"));
            expect(source.contains("hb.capabilities = {"));
            expect(source.contains("sample_zones = true"));
            expect(source.contains("volume = true"));
            expect(source.contains("pan = true"));
            expect(source.contains("sample_offset = true"));
            expect(source.contains("sample_end = true"));
            expect(source.contains("crossfade = false"));
            expect(source.contains("function hb.ok"));
            expect(source.contains("function hb.fail"));
            expect(source.contains("function hb.warn"));
            expect(source.contains("function hb.unsupported"));
            expect(source.contains("function hb.path_join"));
            expect(source.contains("function hb.sample_path"));
            expect(source.contains("function hb.region_mapping"));
            expect(source.contains("function hb.region_label"));
            expect(source.contains("function hb.set_parameter_required"));
            expect(source.contains("function hb.set_parameter_if_available"));
            expect(source.contains("function hb.assign_field_required"));
            expect(source.contains("function hb.create_layer"));
            expect(source.contains("function hb.create_sample_zone"));
            expect(source.contains("pcall(function()\n        return Layer()"));
            expect(source.contains("pcall(function()\n        return Zone()"));
            expect(source.contains("\"InheritVelocitySettings\", false"));
            expect(source.contains("\"VelocityToLevelCurve\", 1"));
            expect(source.contains("\"SampleOsc.Level\", sample_osc_level_db"));
            expect(source.contains("\"Amp.Pan\", amp_pan"));
            expect(!source.contains("type(Layer) ~= \"function\""));
            expect(!source.contains("type(Zone) ~= \"function\""));
            expect(source.contains("function sfz_inclusive_end_to_halion_marker"));
            expect(source.contains("return sample_index + 1"));
            expect(source.contains("function hb.set_amp_envelope_required"));
            expect(source.contains("function hb.append_sample_zone"));
            expect(source.contains("function hb.save_layer_preset"));

            juce::MemoryBlock data;
            expect(helperFile.loadFileAsData(data));
            const auto bytes = asBytes(data);
            expect(std::none_of(bytes.begin(), bytes.end(),
                                [](const std::byte value) { return value == std::byte{static_cast<unsigned char>('\r')}; }));
        }

        beginTest("Converter Emitter - deterministic build directory output");
        {
            auto tempDir = cleanTempDirectory("halionbridge_converter_emitter");
            const auto outputDirectory = halionbridge::detail::toStdPath(tempDir);

            auto request = halionbridge::converters::BuildDirectoryRequest{};
            request.outputDirectory = outputDirectory;
            request.scripts.push_back(halionbridge::converters::GeneratedLuaScript{"001_a.lua", "001_a.lua", "return {}\n"});
            request.scripts.push_back(
                halionbridge::converters::GeneratedLuaScript{"002_quote\"name.lua", "002_quote.lua", "return \"quoted\"\n"});
            request.scripts.push_back(halionbridge::converters::GeneratedLuaScript{
                "", "halionbridge-sfz.lua", "return {}\n", halionbridge::converters::GeneratedLuaFileRole::helperModule});

            auto result = halionbridge::converters::writeBuildDirectory(request);
            expect(result.succeeded);
            expectEquals(static_cast<int>(result.generatedLuaFiles.size()), 3);

            const auto buildFile = tempDir.getChildFile("halionbridge_build.lua");
            const auto buildText = buildFile.loadFileAsString();
            expect(buildText.contains("\"001_a.lua\""));
            expect(buildText.contains("\"002_quote\\\"name.lua\""));
            expect(!buildText.contains("halionbridge-sfz.lua"));
            expect(tempDir.getChildFile("halionbridge-sfz.lua").existsAsFile());

            auto refused = halionbridge::converters::writeBuildDirectory(request);
            expect(!refused.succeeded);
            expect(!refused.diagnostics.empty());

            request.overwrite = true;
            auto overwritten = halionbridge::converters::writeBuildDirectory(request);
            expect(overwritten.succeeded);

            tempDir.deleteRecursively();
        }

        beginTest("Converter Emitter - rejects unsafe and duplicate generated names");
        {
            auto tempDir = cleanTempDirectory("halionbridge_converter_emitter_invalid");
            const auto outputDirectory = halionbridge::detail::toStdPath(tempDir);

            auto request = halionbridge::converters::BuildDirectoryRequest{};
            request.outputDirectory = outputDirectory;
            request.scripts.push_back(halionbridge::converters::GeneratedLuaScript{"valid.lua", "valid.lua", "return {}\n"});

            const auto expectRejectedFileName = [&](const std::string& fileName)
            {
                auto invalid = request;
                invalid.scripts.front().fileName = fileName;
                const auto result = halionbridge::converters::writeBuildDirectory(invalid);
                expect(!result.succeeded);
                expect(containsDiagnosticCode(result.diagnostics, "invalid-script-filename"));
            };

            expectRejectedFileName("");
            expectRejectedFileName(".");
            expectRejectedFileName("..");
            expectRejectedFileName("../escape.lua");
            expectRejectedFileName("nested/script.lua");
            expectRejectedFileName("C:/escape.lua");
            expectRejectedFileName("script.txt");

            auto duplicateFile = request;
            duplicateFile.scripts.push_back(halionbridge::converters::GeneratedLuaScript{"other.lua", "VALID.lua", "return {}\n"});
            auto result = halionbridge::converters::writeBuildDirectory(duplicateFile);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "duplicate-script-filename"));

            auto duplicateModule = request;
            duplicateModule.scripts.push_back(halionbridge::converters::GeneratedLuaScript{"VALID.lua", "other.lua", "return {}\n"});
            result = halionbridge::converters::writeBuildDirectory(duplicateModule);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "duplicate-module-name"));

            auto duplicateEffectiveModule = request;
            duplicateEffectiveModule.scripts.push_back(
                halionbridge::converters::GeneratedLuaScript{"valid", "other.lua", "return {}\n"});
            result = halionbridge::converters::writeBuildDirectory(duplicateEffectiveModule);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "duplicate-module-name"));

            auto helperOnly = halionbridge::converters::BuildDirectoryRequest{};
            helperOnly.outputDirectory = outputDirectory;
            helperOnly.scripts.push_back(halionbridge::converters::GeneratedLuaScript{
                "", "halionbridge-sfz.lua", "return {}\n", halionbridge::converters::GeneratedLuaFileRole::helperModule});
            result = halionbridge::converters::writeBuildDirectory(helperOnly);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "no-build-entrypoints"));

            auto reservedHelperEntrypoint = request;
            reservedHelperEntrypoint.scripts.front().moduleName = "halionbridge-sfz.lua";
            reservedHelperEntrypoint.scripts.front().fileName = "halionbridge-sfz.lua";
            result = halionbridge::converters::writeBuildDirectory(reservedHelperEntrypoint);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "reserved-helper-entrypoint"));

            tempDir.deleteRecursively();
        }

        beginTest("SFZ Converter - validates source directories");
        {
            const auto tempDir = cleanTempDirectory("halionbridge_sfz_validation");
            expect(tempDir.createDirectory());
            const auto outputDirectory = halionbridge::detail::toStdPath(cleanTempDirectory("halionbridge_sfz_validation_out"));

            auto missingOptions = halionbridge::converters::sfz::ConversionOptions{};
            missingOptions.sourceDirectory = halionbridge::detail::toStdPath(tempDir.getChildFile("missing"));
            missingOptions.outputDirectory = outputDirectory;
            auto result = halionbridge::converters::sfz::convertDirectory(missingOptions);
            expect(!result.succeeded);

            const auto fileSource = tempDir.getChildFile("instrument.sfz");
            expect(fileSource.replaceWithText("<region> sample=missing.wav\n"));
            auto fileOptions = halionbridge::converters::sfz::ConversionOptions{};
            fileOptions.sourceDirectory = halionbridge::detail::toStdPath(fileSource);
            fileOptions.outputDirectory = outputDirectory;
            result = halionbridge::converters::sfz::convertDirectory(fileOptions);
            expect(!result.succeeded);

            const auto emptyDirectory = cleanTempDirectory("halionbridge_sfz_empty");
            expect(emptyDirectory.createDirectory());
            auto emptyOptions = halionbridge::converters::sfz::ConversionOptions{};
            emptyOptions.sourceDirectory = halionbridge::detail::toStdPath(emptyDirectory);
            emptyOptions.outputDirectory = outputDirectory;
            result = halionbridge::converters::sfz::convertDirectory(emptyOptions);
            expect(!result.succeeded);

            tempDir.deleteRecursively();
            emptyDirectory.deleteRecursively();
        }

        beginTest("SFZ Converter - recursive discovery is explicit");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_recursive_source");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("nested").createDirectory());
            expect(sourceDir.getChildFile("nested").getChildFile("instrument.sfz").replaceWithText("not an sfz region\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(cleanTempDirectory("halionbridge_sfz_recursive_out"));
            auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(!result.succeeded);

            options.recursive = true;
            result = halionbridge::converters::sfz::convertDirectory(options);
            expect(!result.succeeded);
            expect(!result.diagnostics.empty());

            sourceDir.deleteRecursively();
        }

        beginTest("SFZ Converter - rejects duplicate preset output names");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_duplicate_presets");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_duplicate_presets_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("bass-one.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=60 hikey=60 lovel=0 hivel=127 pitch_keycenter=60\n"));
            expect(sourceDir.getChildFile("bass_one.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=61 hikey=61 lovel=0 hivel=127 pitch_keycenter=61\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(!result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "duplicate-preset-name"));
            expect(!outputDir.getChildFile("halionbridge_build.lua").existsAsFile());

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - preserves inclusive velocity boundaries");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_velocity_boundaries");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_velocity_boundaries_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("velocity.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=60 hikey=60 lovel=0 hivel=1 pitch_keycenter=60\n"
                                        "<region> sample=sample.wav lokey=61 hikey=61 lovel=63 hivel=64 pitch_keycenter=61\n"
                                        "<region> sample=sample.wav lokey=62 hikey=62 lovel=126 hivel=127 pitch_keycenter=62\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_velocity.lua").loadFileAsString();
            expect(lua.contains("velocity_low = 0"));
            expect(lua.contains("velocity_high = 1"));
            expect(lua.contains("velocity_low = 63"));
            expect(lua.contains("velocity_high = 64"));
            expect(lua.contains("velocity_low = 126"));
            expect(lua.contains("velocity_high = 127"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes static pitch tuning fields");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_static_pitch");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_static_pitch_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("pitch.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=81 pitch_keycenter=57 tune=100\n"
                                        "<region> sample=sample.wav lokey=57 hikey=81 pitch_keycenter=57 transpose=-12\n"
                                        "<region> sample=sample.wav lokey=57 hikey=81 pitch_keycenter=57 pitch_keytrack=50\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_pitch.lua").loadFileAsString();
            expect(lua.contains("pitch = {"));
            expect(lua.contains("tune_cents = 100"));
            expect(lua.contains("tune_cents = -1200"));
            expect(lua.contains("keytrack = 50"));

            const auto helperLua = outputDir.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("\"SampleOsc.Tune\", pitch.tune_cents"));
            expect(helperLua.contains("\"Pitch.CenterKey\", mapping.root_key"));
            expect(helperLua.contains("\"Pitch.KeyFollow\", pitch.keytrack"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes calibrated volume and velocity response fields");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_gain_response");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_gain_response_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("gain.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=57 pitch_keycenter=57 volume=-6 amp_veltrack=100\n"
                                        "<region> sample=sample.wav lokey=58 hikey=58 pitch_keycenter=58 volume=6 amp_veltrack=0\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_gain.lua").loadFileAsString();
            expect(lua.contains("sample_osc_level_db = 1.8"));
            expect(lua.contains("amp_velocity_to_level = 100"));
            expect(lua.contains("sample_osc_level_db = 13.8"));
            expect(lua.contains("amp_velocity_to_level = 0"));

            const auto helperLua = outputDir.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("\"InheritVelocitySettings\", false"));
            expect(helperLua.contains("\"VelocityToLevelCurve\", 1"));
            expect(helperLua.contains("\"SampleOsc.Level\", sample_osc_level_db"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - clamps amp_veltrack to SFZ range");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_amp_veltrack_clamped");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_amp_veltrack_clamped_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("velocity.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=57 pitch_keycenter=57 amp_veltrack=200\n"
                                        "<region> sample=sample.wav lokey=58 hikey=58 pitch_keycenter=58 amp_veltrack=-200\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "amp-veltrack-clamped"));

            const auto lua = outputDir.getChildFile("000_velocity.lua").loadFileAsString();
            expect(lua.contains("amp_velocity_to_level = 100"));
            expect(lua.contains("amp_velocity_to_level = -100"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes verified pan field");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_pan");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_pan_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("pan.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=57 pitch_keycenter=57 pan=-50\n"
                                        "<region> sample=sample.wav lokey=58 hikey=58 pitch_keycenter=58 pan=100\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_pan.lua").loadFileAsString();
            expect(lua.contains("amp_pan = -50"));
            expect(lua.contains("amp_pan = 100"));

            const auto helperLua = outputDir.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("pan = true"));
            expect(helperLua.contains("\"Amp.Pan\", amp_pan"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes verified sample playback range fields");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_sample_range");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_sample_range_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("range.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=57 pitch_keycenter=57 offset=22050 end=44099\n"
                                        "<region> sample=sample.wav lokey=58 hikey=58 pitch_keycenter=58 offset=44100\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_range.lua").loadFileAsString();
            expect(lua.contains("offset = 22050"));
            expect(lua.contains("finish = 44099"));
            expect(lua.contains("offset = 44100"));

            const auto helperLua = outputDir.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("sample_offset = true"));
            expect(helperLua.contains("sample_end = true"));
            expect(helperLua.contains("\"SampleOsc.SampleEnd\", sfz_inclusive_end_to_halion_marker(sample_end)"));
            expect(helperLua.contains("sample_natural_end"));
            expect(helperLua.contains("\"SampleOsc.SampleStart\", sample_start"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - preserves verified loop mode semantics");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_loop_modes");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_loop_modes_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("loops.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=57 pitch_keycenter=57 loop_mode=loop_continuous loop_start=0 loop_end=199\n"
                                        "<region> sample=sample.wav lokey=58 hikey=58 pitch_keycenter=58 loop_mode=loop_sustain loop_start=0 loop_end=199\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_loops.lua").loadFileAsString();
            expect(lua.contains("mode = \"continuous\""));
            expect(lua.contains("mode = \"sustain\""));
            expect(lua.contains("finish = 199"));

            const auto helperLua = outputDir.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("loop_mode == \"sustain\""));
            expect(helperLua.contains("return 4"));
            expect(!helperLua.contains("\"SampleOsc.SampleEnd\", halion_loop_end"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - clamps static pitch values to HALion ranges");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_static_pitch_clamped");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_static_pitch_clamped_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("pitch.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=57 hikey=81 pitch_keycenter=57 transpose=24 tune=100\n"
                                        "<region> sample=sample.wav lokey=57 hikey=81 pitch_keycenter=57 pitch_keytrack=300\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "pitch-tune-clamped"));
            expect(containsDiagnosticCode(result.diagnostics, "pitch-keytrack-clamped"));

            const auto lua = outputDir.getChildFile("000_pitch.lua").loadFileAsString();
            expect(lua.contains("tune_cents = 1200"));
            expect(lua.contains("keytrack = 200"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes explicit static amp envelopes");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_amp_envelope");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_amp_envelope_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("envelope.sfz")
                       .replaceWithText("<global> ampeg_start=10 ampeg_delay=0.02 ampeg_attack=0.25 ampeg_hold=0.1 "
                                        "ampeg_decay=0.5 ampeg_sustain=40 ampeg_release=0\n"
                                        "<region> sample=sample.wav lokey=60 hikey=60 pitch_keycenter=60\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_envelope.lua").loadFileAsString();
            expect(lua.contains("amp_envelope = {"));
            expect(lua.contains("{ level = 0.100000001, duration = 0, curve = 0 }"));
            expect(lua.contains("{ level = 0.100000001, duration = 0.0199999996, curve = 0 }"));
            expect(lua.contains("{ level = 1, duration = 0.25, curve = 0 }"));
            expect(lua.contains("{ level = 1, duration = 0.100000001, curve = 0 }"));
            expect(lua.contains("{ level = 0.400000006, duration = 0.300000012, curve = 0 }"));
            expect(lua.contains("{ level = 0, duration = 0, curve = 0 }"));
            expect(lua.contains("sustain_index = 5"));
            expect(lua.contains("hb.append_sample_zone(ctx, layer, region)"));
            expect(!lua.contains("setAmpEnvelopeRequired"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - writes SFZv2 default release when release is omitted");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_default_release");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_default_release_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("default_release.sfz")
                       .replaceWithText("<region> sample=sample.wav lokey=60 hikey=60 pitch_keycenter=60\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);

            const auto lua = outputDir.getChildFile("000_default_release.lua").loadFileAsString();
            expect(lua.contains("{ level = 0, duration = 0.001"));
            expect(lua.contains("sustain_index = 3"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - reports unsupported advanced amp envelope features");
        {
            auto sourceDir = cleanTempDirectory("halionbridge_sfz_unsupported_amp_envelope");
            auto outputDir = cleanTempDirectory("halionbridge_sfz_unsupported_amp_envelope_out");
            expect(sourceDir.createDirectory());
            expect(sourceDir.getChildFile("sample.wav").replaceWithText(""));
            expect(sourceDir.getChildFile("unsupported.sfz")
                       .replaceWithText("<group> ampeg_release_shape=2.1 ampeg_release_oncc1=1 eg01_ampeg=100\n"
                                        "<region> sample=sample.wav lokey=60 hikey=60 pitch_keycenter=60\n"));

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(sourceDir);
            options.outputDirectory = halionbridge::detail::toStdPath(outputDir);

            const auto result = halionbridge::converters::sfz::convertDirectory(options);
            expect(result.succeeded);
            expect(containsDiagnosticCode(result.diagnostics, "unsupported-amp-envelope"));

            sourceDir.deleteRecursively();
            outputDir.deleteRecursively();
        }

        beginTest("SFZ Converter - converts synth single cycle fixture deterministically");
        {
            const auto fixtureDirectory =
                juce::File::getCurrentWorkingDirectory().getChildFile("examples").getChildFile("synth-single-cycle-sfz");
            expect(fixtureDirectory.isDirectory());

            auto firstOutput = cleanTempDirectory("halionbridge_sfz_fixture_one");
            auto secondOutput = cleanTempDirectory("halionbridge_sfz_fixture_two");

            auto options = halionbridge::converters::sfz::ConversionOptions{};
            options.sourceDirectory = halionbridge::detail::toStdPath(fixtureDirectory);
            options.outputDirectory = halionbridge::detail::toStdPath(firstOutput);
            auto firstResult = halionbridge::converters::sfz::convertDirectory(options);

            options.outputDirectory = halionbridge::detail::toStdPath(secondOutput);
            auto secondResult = halionbridge::converters::sfz::convertDirectory(options);

            expect(firstResult.succeeded);
            expect(secondResult.succeeded);
            expectEquals(firstResult.sfzFilesConverted, 2);
            expectEquals(firstResult.regionsConverted, 12);
            expectEquals(static_cast<int>(firstResult.generatedLuaFiles.size()), 3);

            const auto firstBuildText = firstOutput.getChildFile("halionbridge_build.lua").loadFileAsString();
            const auto secondBuildText = secondOutput.getChildFile("halionbridge_build.lua").loadFileAsString();
            expectEquals(firstBuildText, secondBuildText);
            expect(!firstBuildText.contains("halionbridge-sfz.lua"));
            expect(firstOutput.getChildFile("halionbridge-sfz.lua").existsAsFile());

            const auto helperLua = firstOutput.getChildFile("halionbridge-sfz.lua").loadFileAsString();
            expect(helperLua.contains("hb.version = 1"));
            expect(helperLua.contains("function hb.append_sample_zone"));

            const auto firstLua = firstOutput.getChildFile("000_000_synth_single_cycle_six_regions.lua").loadFileAsString();
            const auto secondLua = secondOutput.getChildFile("000_000_synth_single_cycle_six_regions.lua").loadFileAsString();
            expectEquals(firstLua, secondLua);
            expect(firstLua.contains("local hb = require(\"halionbridge-sfz\")"));
            expect(firstLua.contains("sample_playback = {"));
            expect(firstLua.contains("mapping = {"));
            expect(firstLua.contains("saw_A3_single_cycle.wav"));
            expect(firstLua.contains("name = \"additive_organ_A3_si\""));
            expect(!firstLua.contains("Region 2"));
            expect(firstLua.contains("key_low = 36"));
            expect(firstLua.contains("key_high = 43"));
            expect(firstLua.contains("key_low = 44"));
            expect(firstLua.contains("key_high = 51"));
            expect(firstLua.contains("velocity_high = 127"));
            expect(firstLua.contains("root_key = 57"));
            expect(firstLua.contains("loop = {"));
            expect(firstLua.contains("mode = \"continuous\""));
            expect(firstLua.contains("start = 0"));
            expect(firstLua.contains("finish = 199"));
            expect(!helperLua.contains("\"SampleOsc.SampleEnd\", halion_loop_end"));
            expect(helperLua.contains("\"SampleOsc.PlaybackMode\", 0"));
            expect(helperLua.contains("\"SampleOsc.LoopSelect\", 0"));
            expect(helperLua.contains("\"SampleOsc.SustainLoopModeA\", halion_sustain_loop_mode(loop_mode)"));
            expect(helperLua.contains("\"SampleOsc.SustainLoopEndA\", halion_loop_end"));
            expect(!firstLua.contains("start = 14"));
            expect(!firstLua.contains("start = 86"));
            expect(firstLua.contains("sample_osc_level_db = 7.8"));
            expect(firstLua.contains("amp_velocity_to_level = 100"));
            expect(firstLua.contains("filter = {"));
            expect(firstLua.contains("cutoff = 4978"));
            expect(firstLua.contains("hb.create_layer(ctx, layerName)"));
            expect(firstLua.contains("hb.append_sample_zone(ctx, layer, region)"));
            expect(firstLua.contains("hb.save_layer_preset(ctx, layer, outputFile)"));
            expect(!firstLua.contains("local function setNameIfAvailable"));
            expect(!firstLua.contains("local function setParameterRequired"));
            expect(!firstLua.contains("local function setParameterIfAvailable"));
            expect(!firstLua.contains("local function assignFieldRequired"));
            expect(!firstLua.contains("local function appendSampleZone"));
            expect(!firstLua.contains("SampleOsc.Filename"));
            expect(!firstLua.contains("SampleOsc.Rootkey"));

            const auto velocityLua =
                firstOutput.getChildFile("001_001_synth_single_cycle_three_regions_three_velocity_layers.lua").loadFileAsString();
            expect(velocityLua.contains("velocity_low = 64"));
            expect(velocityLua.contains("velocity_high = 63"));
            expect(velocityLua.contains("velocity_high = 127"));

            firstOutput.deleteRecursively();
            secondOutput.deleteRecursively();
        }
#endif

        beginTest("Progress Marker Text Decoding");
        {
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("50726f677265737320312f32202835302529")),
                         juce::String("Progress 1/2 (50%)"));
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("")), juce::String("HALion Lua progress"));
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("abc")), juce::String("abc"));
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("zz")), juce::String("zz"));
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("old_style-message")),
                         juce::String("old style message"));
            expectEquals(juce::String(halionbridge::detail::decodeProgressMarkerText("deadbeef")), juce::String("deadbeef"));
        }

        beginTest("Progress Marker Cleanup - deletes compact and legacy stale markers");
        {
            auto tempDir = cleanTempDirectory("halionbridge_progress_cleanup");
            expect(tempDir.createDirectory());

            auto compactMarker = tempDir.getChildFile("hbp_000001_i_50726F6772657373.vstpreset");
            auto legacyMarker = tempDir.getChildFile("halionbridge_progress_000001_info_50726F6772657373.vstpreset");
            auto failedStatusMarker = tempDir.getChildFile("halionbridge_status_failed.vstpreset");
            expect(compactMarker.replaceWithText("marker"));
            expect(legacyMarker.replaceWithText("marker"));
            expect(failedStatusMarker.replaceWithText("marker"));

            auto result = halionbridge::detail::deleteProgressMarkers(tempDir, "test progress marker");
            expectEquals(result.found, 2);
            expectEquals(result.failed, 0);
            expect(result.remainingNames.empty());
            expect(!compactMarker.existsAsFile());
            expect(!legacyMarker.existsAsFile());
            expect(failedStatusMarker.existsAsFile());

            tempDir.deleteRecursively();
        }

        beginTest("Progress Marker Cleanup - deletes malformed compact markers after polling");
        {
            auto tempDir = cleanTempDirectory("halionbridge_progress_malformed");
            expect(tempDir.createDirectory());

            auto marker = tempDir.getChildFile("hbp_malformed.vstpreset");
            expect(marker.replaceWithText("marker"));

            auto seenMarkers = std::set<std::string>();
            halionbridge::detail::logNewProgressMarkers(tempDir, seenMarkers);

            expect(!marker.existsAsFile());
            expect(contains(std::vector<std::string>(seenMarkers.begin(), seenMarkers.end()), marker.getFileName().toStdString()));

            tempDir.deleteRecursively();
        }

        beginTest("Progress Marker Cleanup - pre-seen stale markers are suppressed");
        {
            auto tempDir = cleanTempDirectory("halionbridge_progress_seen");
            expect(tempDir.createDirectory());

            auto marker = tempDir.getChildFile("hbp_000002_i_50726F6772657373.vstpreset");
            expect(marker.replaceWithText("marker"));

            auto seenMarkers = std::set<std::string>{marker.getFileName().toStdString()};
            halionbridge::detail::logNewProgressMarkers(tempDir, seenMarkers);
            expect(marker.existsAsFile());

            auto result = halionbridge::detail::deleteProgressMarkers(tempDir, "test progress marker");
            expectEquals(result.found, 1);
            expectEquals(result.failed, 0);
            expect(result.remainingNames.empty());
            expect(!marker.existsAsFile());

            tempDir.deleteRecursively();
        }

        beginTest("Progress Marker Cleanup - drain deletes late progress markers");
        {
            auto tempDir = cleanTempDirectory("halionbridge_progress_drain");
            expect(tempDir.createDirectory());

            auto lateMarker = tempDir.getChildFile("hbp_000003_i_4C617465.vstpreset");
            auto writer = std::thread(
                [lateMarker]()
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    lateMarker.replaceWithText("marker");
                });

            auto seenMarkers = std::set<std::string>();
            auto result = halionbridge::detail::drainProgressMarkers(tempDir, seenMarkers, 500, 100);

            writer.join();
            expect(result.found >= 1);
            expectEquals(result.failed, 0);
            expect(!lateMarker.existsAsFile());

            tempDir.deleteRecursively();
        }

        beginTest("Path Normalization");
        {
            auto quoted = halionbridge::detail::normalizeCliPath("\"relative-folder///\"");
            expect(quoted.getFullPathName().endsWith("relative-folder"));

            auto rootDirectory = juce::File::getCurrentWorkingDirectory();
            while (!rootDirectory.isRoot())
                rootDirectory = rootDirectory.getParentDirectory();

            auto root = halionbridge::detail::normalizeCliPath(rootDirectory.getFullPathName());
            expect(root.isRoot());
        }

        beginTest("Build Status Marker Paths - Build Directory");
        {
            auto buildDirectory = cleanTempDirectory("halionbridge_marker_test");

            auto markerFiles = halionbridge::Bridge::getBuildStatusMarkerFilesForDirectory(halionbridge::detail::toStdPath(buildDirectory));
            expect(markerFiles.okFile.filename() == "halionbridge_status_ok.vstpreset");
            expect(markerFiles.failedFile.filename() == "halionbridge_status_failed.vstpreset");
            expect(markerFiles.okFile.parent_path() == halionbridge::detail::toStdPath(buildDirectory));
            expect(markerFiles.failedFile.parent_path() == halionbridge::detail::toStdPath(buildDirectory));
        }

        beginTest("Child Process Output - split UTF-8 and final partial line");
        {
            halionbridge::detail::ChildProcessOutputBuffer output;

            auto firstChunk = std::string("first ");
            firstChunk.push_back(static_cast<char>(0xC3));

            auto lines = output.append(firstChunk);
            expect(lines.empty());

            auto secondChunk = std::string();
            secondChunk.push_back(static_cast<char>(0xB6));
            secondChunk += "\r\nsecond";

            lines = output.append(secondChunk);
            expectEquals(static_cast<int>(lines.size()), 1);

            auto expectedFirstLine = std::string("first ");
            expectedFirstLine.push_back(static_cast<char>(0xC3));
            expectedFirstLine.push_back(static_cast<char>(0xB6));

            if (!lines.empty())
                expectEquals(juce::String(lines[0]),
                             juce::String::fromUTF8(expectedFirstLine.data(), static_cast<int>(expectedFirstLine.size())));

            auto finalLine = output.flush();
            expect(finalLine.has_value());
            if (finalLine)
                expectEquals(juce::String(*finalLine), juce::String("second"));

            expect(!output.flush().has_value());
        }

        beginTest("Runtime Module Text");
        {
            auto buildDirectory = std::filesystem::path("C:\\test\\halion build");
            auto text = halionbridge::Bridge::createRuntimeModuleText(buildDirectory);
            expect(text.find("HALIONBRIDGE_RUNTIME_ROOT") != std::string::npos);
            expect(text.find("C:/test/halion build") != std::string::npos);
            expect(text.find("halionbridge_builder") != std::string::npos);
            expect(text.find("runtimePathPrefix") != std::string::npos);
        }

        beginTest("Plugin Location - platform default path");
        {
            const auto defaultPath = halionbridge::Bridge::getDefaultHalionPluginPath();
#if JUCE_WINDOWS
            expect(defaultPath == std::filesystem::path(R"(C:\Program Files\Common Files\VST3\Steinberg\HALion 7.vst3)"));
#elif JUCE_MAC
            expect(defaultPath == std::filesystem::path("/Library/Audio/Plug-Ins/VST3/Steinberg/HALion 7.vst3"));
#else
            expect(defaultPath.empty());
#endif
        }

        beginTest("VST3 Preset Inspection - embedded HALion bootstrap preset source");
        {
            auto presetFile =
                juce::File::getCurrentWorkingDirectory().getChildFile("halion-lua").getChildFile("builder_bootstrap.vstpreset");

            expect(presetFile.existsAsFile());

            juce::MemoryBlock presetData;
            expect(presetFile.loadFileAsData(presetData));

            auto info = halionbridge::Bridge::inspectVstPresetContainer(asBytes(presetData));
            expect(info.has_value());

            if (info)
            {
                expectEquals(juce::String(info->classId), juce::String("3B63D74130B34AE397AF92A9659137D5"));
                expect(!info->hasComponentState);
                expect(!info->hasControllerState);
                expect(info->hasProgramData);
                expect(info->programOrUnitId.has_value());
                expectEquals(*info->programOrUnitId, 0);
            }
        }

        beginTest("Plugin Description - embedded class ID synthesis");
        {
            auto pluginFile = juce::File::getCurrentWorkingDirectory().getChildFile("HALion 7.vst3");
            auto description = halionbridge::detail::makeHalionDescriptionFromClassId(pluginFile, "3B63D74130B34AE397AF92A9659137D5");
            expect(description.has_value());
            if (description)
            {
                expect(description->isInstrument);
                expectEquals(description->name, juce::String("HALion 7"));
                expectEquals(description->pluginFormatName, juce::String("VST3"));
            }
        }

        beginTest("Build Info - generated metadata");
        {
            const auto buildInfo = halionbridge::getBuildInfo();
            const auto version = juce::String(buildInfo.versionString);
            const auto packageBasename = juce::String(buildInfo.packageBasename);

            expect(version.isNotEmpty());
            expect(packageBasename.startsWith("halionbridge-"));
            expectEquals(packageBasename, "halionbridge-" + version);
            expect(juce::String(buildInfo.gitShaShort).isNotEmpty());
            expect(version.endsWith("-mod") == buildInfo.isDirty);
        }

        beginTest("Log Level Parsing");
        {
            auto parsed = halionbridge::log::parseLevel("debug");
            expect(parsed.valid);
            expect(parsed.level == halionbridge::log::Level::debug);

            parsed = halionbridge::log::parseLevel(" WARNING ");
            expect(parsed.valid);
            expect(parsed.level == halionbridge::log::Level::warn);

            parsed = halionbridge::log::parseLevel("silent");
            expect(parsed.valid);
            expect(parsed.level == halionbridge::log::Level::off);

            parsed = halionbridge::log::parseLevel("nope");
            expect(!parsed.valid);
            expect(parsed.level == halionbridge::log::Level::info);
        }
    }
};

static BridgeTests bridgeTests;

int main(int argc, char* argv[])
{
    if (argc == 4 && juce::String(argv[1]) == kHoldScriptLockArgument)
        return runScriptLockHolder(argv[2], argv[3]);

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ConsoleLogger logger;
    juce::Logger::setCurrentLogger(&logger);
    configureQuietRuntimeLogger();
    gTestTempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("halionbridge_tests", "", false);
    gTestTempRoot.createDirectory();

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure(false);
    runner.setPassesAreLogged(true);
    runner.runTestsInCategory("halionbridge");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult(i)->failures;
    }

    juce::Logger::setCurrentLogger(nullptr);
    gTestTempRoot.deleteRecursively();

    return failures == 0 ? 0 : 1;
}
