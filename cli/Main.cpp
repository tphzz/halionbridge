#include "halionbridge/Bridge.h"
#include "halionbridge/BuildInfo.h"
#include "halionbridge/CrashDiagnostics.h"
#include "BuildWorker.h"
#include "BuildFile.h"
#include "Log.h"
#include "PathUtils.h"
#include "PluginScan.h"
#if HALIONBRIDGE_ENABLE_CONVERTERS
#include "halionbridge_converters/Converter.h"
#endif
#include <juce_events/juce_events.h>
#include <csignal>
#include <iostream>
#include <span>
#include <vector>

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace
{

void signalStopHandler(int)
{
    halionbridge::requestStop();
}

#if JUCE_WINDOWS
BOOL WINAPI consoleStopHandler(DWORD controlType)
{
    switch (controlType)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        halionbridge::requestStop();
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void installStopHandlers()
{
    halionbridge::resetStopRequest();
    std::signal(SIGINT, signalStopHandler);
    std::signal(SIGTERM, signalStopHandler);

#if JUCE_WINDOWS
    SetConsoleCtrlHandler(consoleStopHandler, TRUE);
#endif
}

void printHelp()
{
    const auto buildInfo = halionbridge::getBuildInfo();

    std::cout << "halionbridge " << buildInfo.versionString << "\n"
              << "\n"
              << "Usage:\n"
              << "  halionbridge <build-directory> [options]\n"
              << "  halionbridge init <build-directory> [--overwrite]\n"
#if HALIONBRIDGE_ENABLE_CONVERTERS
              << "  halionbridge convert <converter-id> <source-directory> [output-directory] [converter-options]\n"
#endif
              << "\n"
              << "Required argument:\n"
              << "  <build-directory>        Directory containing halionbridge_build.lua and build script Lua files.\n"
              << "\n"
              << "Setup command:\n"
              << "  init <build-directory>   Generate halionbridge_build.lua from top-level Lua files and exit.\n"
              << "  --overwrite              Replace an existing halionbridge_build.lua when used with init.\n"
              << "\n"
#if HALIONBRIDGE_ENABLE_CONVERTERS
              << "Converter command:\n"
              << "  convert --list           List converter IDs compiled into this halionbridge binary.\n"
              << "  convert <id> --help      Show converter-specific options.\n"
              << "\n"
#endif
              << "Plugin selection:\n"
              << "  --plugin <path>          Override the HALion 7 VST3 path.\n"
              << "\n"
              << "Runtime options:\n"
              << "  --timeout-seconds <n>    Build completion timeout. Defaults to 3600 seconds.\n"
              << "  --no-timeout             Wait indefinitely. Equivalent to --timeout-seconds 0.\n"
              << "  --build-chunk-size <n>   Number of Lua build scripts per HALion run. Defaults to 15.\n"
              << "  --fail-fast              Stop after the first failed Lua build chunk.\n"
              << "  --gui                    Use JUCE's GUI-capable VST3 host format and show HALion's editor.\n"
              << "  --nokill                 Keep HALion loaded after build completion or failure for inspection.\n"
              << "\n"
              << "Diagnostics:\n"
              << "  --force-scan             Force VST3 plugin scanning instead of using the embedded class ID shortcut.\n"
              << "\n"
              << "General:\n"
              << "  --version                Show build version information and exit.\n"
              << "  --help, -h               Show this help and exit.\n"
              << "\n"
              << "Default behavior:\n"
              << "  Without --gui, halionbridge uses JUCE's headless VST3 host format and does not open an audio device.\n";
}

void printVersion()
{
    const auto buildInfo = halionbridge::getBuildInfo();
    const auto ref = buildInfo.isTaggedRelease ? buildInfo.gitTag : buildInfo.gitBranch;

    std::cout << "halionbridge " << buildInfo.versionString << "\n"
              << "package: " << buildInfo.packageBasename << "\n"
              << "git: " << buildInfo.gitShaShort << "\n"
              << "ref: " << ref << "\n"
              << "source: " << (buildInfo.isDirty ? "modified" : "clean") << "\n";
}

bool isInternalWorkerCommand(const juce::StringArray& args)
{
    return args.size() > 0 && (args[0] == halionbridge::detail::kBuildWorkerArgument || args[0] == "--halionbridge-scan-plugin");
}

#if HALIONBRIDGE_ENABLE_CONVERTERS
void printConvertHelp()
{
    std::cout << "halionbridge convert\n"
              << "\n"
              << "Usage:\n"
              << "  halionbridge convert <converter-id> <source-directory> [output-directory] [converter-options]\n"
              << "  halionbridge convert --list\n"
              << "  halionbridge convert <converter-id> --help\n";
}

void logDiagnostic(const halionbridge::converters::Diagnostic& diagnostic)
{
    const auto text = diagnostic.source.empty() ? diagnostic.message : diagnostic.source.string() + ": " + diagnostic.message;

    switch (diagnostic.level)
    {
    case halionbridge::converters::DiagnosticLevel::info:
        halionbridge::log::info("{}", text);
        break;
    case halionbridge::converters::DiagnosticLevel::warning:
        halionbridge::log::warn("{}", text);
        break;
    case halionbridge::converters::DiagnosticLevel::error:
        halionbridge::log::error("{}", text);
        break;
    }
}

int runConvertCommand(std::span<const std::string> args)
{
    halionbridge::converters::ConverterRegistry registry;
    halionbridge::converters::registerCompiledConverters(registry);

    if (args.empty() || args[0] == "--help" || args[0] == "-h")
    {
        printConvertHelp();
        return 0;
    }

    if (args[0] == "--list")
    {
        const auto converters = registry.list();
        if (converters.empty())
        {
            std::cout << "No converters are compiled into this halionbridge binary.\n";
            return 0;
        }

        std::cout << "Available converters:\n";
        for (const auto& converter : converters)
            std::cout << "  " << converter.id << "  " << converter.summary << "\n";
        return 0;
    }

    const auto* converter = registry.find(args[0]);
    if (converter == nullptr)
    {
        halionbridge::log::error("Unknown converter '{}'. Run halionbridge convert --list to see available converters.", args[0]);
        return 1;
    }

    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h"))
    {
        if (converter->helpText != nullptr)
            std::cout << converter->helpText();
        else
            printConvertHelp();
        return 0;
    }

    const auto converterArgs = std::span<const std::string>(args.data() + 1, args.size() - 1);
    auto usedStreamingContext = false;
    auto result = halionbridge::converters::ConverterResult{};

    if (converter->runWithContext != nullptr)
    {
        auto context = halionbridge::converters::ConverterRunContext{[](const halionbridge::converters::Diagnostic& diagnostic, void*)
                                                                     { logDiagnostic(diagnostic); },
                                                                     [](void*) { return halionbridge::isStopRequested(); }, nullptr};
        result = converter->runWithContext(converterArgs, context);
        usedStreamingContext = true;
    }
    else if (converter->run != nullptr)
    {
        result = converter->run(converterArgs);
    }
    else
    {
        halionbridge::log::error("Converter '{}' is not executable.", args[0]);
        return 1;
    }

    if (!usedStreamingContext)
        for (const auto& diagnostic : result.diagnostics)
            logDiagnostic(diagnostic);

    halionbridge::log::flush();

    return result.exitCode;
}
#endif

} // namespace

class ConsoleLogger : public juce::Logger
{
    void logMessage(const juce::String& message) override
    {
        for (auto line : juce::StringArray::fromLines(message))
        {
            line = line.trim();
            if (line.isEmpty())
                continue;

            const auto lower = line.toLowerCase();
            const auto text = line.toStdString();

            if (lower.startsWith("error:") || lower.startsWith("failed:") || lower.contains("[error]"))
                halionbridge::log::error("{}", text);
            else if (lower.startsWith("warning:") || lower.startsWith("warn:") || lower.contains("[warning]") || lower.contains("[warn]"))
                halionbridge::log::warn("{}", text);
            else if (line.startsWithChar('['))
                halionbridge::log::debug("{}", text);
            else
                halionbridge::log::info("{}", text);
        }
    }
};

int main(int argc, char* argv[])
{
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    juce::StringArray juceArgs;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        juceArgs.add(juce::String(argv[i]));
        args.emplace_back(argv[i]);
    }

    if ((juceArgs.isEmpty() || juceArgs[0] == "--help" || juceArgs[0] == "-h") && !isInternalWorkerCommand(juceArgs))
    {
        printHelp();
        return 0;
    }

    if (juceArgs[0] == "--version" && !isInternalWorkerCommand(juceArgs))
    {
        printVersion();
        return 0;
    }

    halionbridge::log::configureFromEnvironment();
    installStopHandlers();

    if (juceArgs.size() > 0 && juceArgs[0] == "init")
    {
        const auto initResult = halionbridge::detail::runInitCommand(juceArgs);
        if (initResult.exitCode == 0)
        {
            halionbridge::log::info("{} with {} Lua build script file(s).", initResult.message.toStdString(),
                                    initResult.moduleNames.size());
            if (initResult.warning.isNotEmpty())
                halionbridge::log::warn("{}", initResult.warning.toStdString());
        }
        else
        {
            for (const auto& line : juce::StringArray::fromLines(initResult.message))
            {
                if (line.isNotEmpty())
                    halionbridge::log::error("{}", line.toStdString());
            }
        }

        halionbridge::log::flush();
        return initResult.exitCode;
    }

#if HALIONBRIDGE_ENABLE_CONVERTERS
    if (juceArgs.size() > 0 && juceArgs[0] == "convert")
    {
        const auto convertArgs = std::span<const std::string>(args.data() + 1, args.size() - 1);
        const auto exitCode = runConvertCommand(convertArgs);
        halionbridge::log::flush();
        return exitCode;
    }
#endif

    halionbridge::installCrashDiagnostics();
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ConsoleLogger logger;
    juce::Logger::setCurrentLogger(&logger);

    if (juceArgs.size() > 0 && juceArgs[0] == "--halionbridge-scan-plugin")
    {
        const auto exitCode = halionbridge::detail::runPluginScanWorker(juceArgs);
        juce::Logger::setCurrentLogger(nullptr);
        halionbridge::log::flush();
        return exitCode;
    }

    if (juceArgs.size() > 0 && juceArgs[0] == halionbridge::detail::kBuildWorkerArgument)
    {
        auto options = halionbridge::detail::parseBuildWorkerArguments(args);
        if (!options)
        {
            juce::Logger::setCurrentLogger(nullptr);
            halionbridge::log::flush();
            return halionbridge::detail::runResultToBuildWorkerExitCode(halionbridge::RunResult::invalidOptions);
        }

        const auto executableFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        options->executableFile = halionbridge::detail::toStdPath(executableFile);

        halionbridge::Bridge app;
        const auto runResult = app.runDetailed(*options);
        juce::Logger::setCurrentLogger(nullptr);
        halionbridge::log::flush();
        return halionbridge::detail::runResultToBuildWorkerExitCode(runResult);
    }

    auto options = halionbridge::Bridge::parseArguments(args);
    if (!options)
    {
        juce::Logger::setCurrentLogger(nullptr);
        halionbridge::log::flush();
        return 1;
    }

    const auto executableFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    options->executableFile = halionbridge::detail::toStdPath(executableFile);

    halionbridge::Bridge app;
    const auto runResult = app.runDetailed(*options);
    if (runResult != halionbridge::RunResult::success)
    {
        if (runResult == halionbridge::RunResult::stopped)
            halionbridge::log::warn("halionbridge stopped by user request.");
        else
            halionbridge::log::error("Failed to run halionbridge.");

        juce::Logger::setCurrentLogger(nullptr);
        halionbridge::log::flush();
        return 1;
    }

    juce::Logger::setCurrentLogger(nullptr);
    halionbridge::log::flush();
    return 0;
}
