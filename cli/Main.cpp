#include "halionbridge/Bridge.h"
#include "halionbridge/BuildInfo.h"
#include "halionbridge/CrashDiagnostics.h"
#include "BuildFile.h"
#include "Log.h"
#include "PathUtils.h"
#include "PluginScan.h"
#include <juce_events/juce_events.h>
#include <csignal>
#include <iostream>
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
              << "\n"
              << "Required argument:\n"
              << "  <build-directory>        Directory containing halionbridge_build.lua and build script Lua files.\n"
              << "\n"
              << "Setup command:\n"
              << "  init <build-directory>   Generate halionbridge_build.lua from top-level Lua files and exit.\n"
              << "  --overwrite              Replace an existing halionbridge_build.lua when used with init.\n"
              << "\n"
              << "Plugin selection:\n"
              << "  --plugin <path>          Override the HALion 7 VST3 path.\n"
              << "\n"
              << "Runtime options:\n"
              << "  --timeout-seconds <n>    Build completion timeout. Defaults to 0, which waits forever.\n"
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

    if ((juceArgs.isEmpty() || juceArgs.contains("--help") || juceArgs.contains("-h")) &&
        (juceArgs.size() == 0 || juceArgs[0] != "--halionbridge-scan-plugin"))
    {
        printHelp();
        return 0;
    }

    if (juceArgs.contains("--version") && (juceArgs.size() == 1 || juceArgs[0] != "--halionbridge-scan-plugin"))
    {
        printVersion();
        return 0;
    }

    halionbridge::log::configureFromEnvironment();

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

        return initResult.exitCode;
    }

    halionbridge::installCrashDiagnostics();
    installStopHandlers();

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ConsoleLogger logger;
    juce::Logger::setCurrentLogger(&logger);

    if (juceArgs.size() > 0 && juceArgs[0] == "--halionbridge-scan-plugin")
    {
        const auto exitCode = halionbridge::detail::runPluginScanWorker(juceArgs);
        juce::Logger::setCurrentLogger(nullptr);
        return exitCode;
    }

    auto options = halionbridge::Bridge::parseArguments(args);
    if (!options)
    {
        juce::Logger::setCurrentLogger(nullptr);
        return 1;
    }

    const auto executableFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    options->executableFile = halionbridge::detail::toStdPath(executableFile);

    halionbridge::Bridge app;
    if (!app.run(*options))
    {
        halionbridge::log::error("Failed to run halionbridge.");
        juce::Logger::setCurrentLogger(nullptr);
        return 1;
    }

    juce::Logger::setCurrentLogger(nullptr);
    return 0;
}
