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
            expect(tempDir.getChildFile("HALIONBRIDGE_RUNTIME.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("HALIONBRIDGE_BUILDER.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("BUILDER_BOOTSTRAP.lua").replaceWithText("return {}"));
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
            expect(!contains(generatedNames, "HALIONBRIDGE_RUNTIME.lua"));
            expect(!contains(generatedNames, "HALIONBRIDGE_BUILDER.lua"));
            expect(!contains(generatedNames, "BUILDER_BOOTSTRAP.lua"));
            expect(!contains(generatedNames, "000_nested.lua"));

            tempDir.deleteRecursively();
        }

        beginTest("Init Command - validates arguments and warns about helper modules");
        {
            auto tempDir = cleanTempDirectory("halionbridge_init_command");
            expect(tempDir.createDirectory());
            expect(tempDir.getChildFile("001_first.lua").replaceWithText("return {}"));
            expect(tempDir.getChildFile("helper.lua").replaceWithText("return {}"));

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

        beginTest("Converter Emitter - deterministic build directory output");
        {
            auto tempDir = cleanTempDirectory("halionbridge_converter_emitter");
            const auto outputDirectory = halionbridge::detail::toStdPath(tempDir);

            auto request = halionbridge::converters::BuildDirectoryRequest{};
            request.outputDirectory = outputDirectory;
            request.scripts.push_back(halionbridge::converters::GeneratedLuaScript{"001_a.lua", "001_a.lua", "return {}\n"});
            request.scripts.push_back(
                halionbridge::converters::GeneratedLuaScript{"002_quote\"name.lua", "002_quote.lua", "return \"quoted\"\n"});

            auto result = halionbridge::converters::writeBuildDirectory(request);
            expect(result.succeeded);
            expectEquals(static_cast<int>(result.generatedLuaFiles.size()), 2);

            const auto buildFile = tempDir.getChildFile("halionbridge_build.lua");
            const auto buildText = buildFile.loadFileAsString();
            expect(buildText.contains("\"001_a.lua\""));
            expect(buildText.contains("\"002_quote\\\"name.lua\""));

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
            expectRejectedFileName("../escape.lua");
            expectRejectedFileName("nested/script.lua");
            expectRejectedFileName("C:/escape.lua");

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
            expect(lua.contains("lovel = 0"));
            expect(lua.contains("hivel = 1"));
            expect(lua.contains("lovel = 63"));
            expect(lua.contains("hivel = 64"));
            expect(lua.contains("lovel = 126"));
            expect(lua.contains("hivel = 127"));

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
            expectEquals(static_cast<int>(firstResult.generatedLuaFiles.size()), 2);

            const auto firstBuildText = firstOutput.getChildFile("halionbridge_build.lua").loadFileAsString();
            const auto secondBuildText = secondOutput.getChildFile("halionbridge_build.lua").loadFileAsString();
            expectEquals(firstBuildText, secondBuildText);

            const auto firstLua = firstOutput.getChildFile("000_000_synth_single_cycle_six_regions.lua").loadFileAsString();
            const auto secondLua = secondOutput.getChildFile("000_000_synth_single_cycle_six_regions.lua").loadFileAsString();
            expectEquals(firstLua, secondLua);
            expect(firstLua.contains("saw_A3_single_cycle.wav"));
            expect(firstLua.contains("lokey = 36"));
            expect(firstLua.contains("hikey = 43"));
            expect(firstLua.contains("hivel = 127"));
            expect(firstLua.contains("pitch_keycenter = 57"));
            expect(firstLua.contains("loop_start = 0"));
            expect(firstLua.contains("loop_end = 199"));

            const auto velocityLua =
                firstOutput.getChildFile("001_001_synth_single_cycle_three_regions_three_velocity_layers.lua").loadFileAsString();
            expect(velocityLua.contains("lovel = 64"));
            expect(velocityLua.contains("hivel = 63"));
            expect(velocityLua.contains("hivel = 127"));

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
