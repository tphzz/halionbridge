#include "halionbridge/CrashDiagnostics.h"
#include "Log.h"

#include <atomic>
#include <iostream>

#if defined(_WIN32) && defined(HALIONBRIDGE_ENABLE_CRASH_DUMPS)
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <iomanip>
#include <sstream>
#endif

namespace halionbridge
{
namespace
{

std::atomic<const char*> currentPhase{"process startup"};

#if defined(_WIN32) && defined(HALIONBRIDGE_ENABLE_CRASH_DUMPS)

std::wstring getModulePathForAddress(void* address)
{
    if (address == nullptr)
        return {};

    MEMORY_BASIC_INFORMATION memoryInfo{};
    if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) == 0 || memoryInfo.AllocationBase == nullptr)
        return {};

    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(static_cast<HMODULE>(memoryInfo.AllocationBase), modulePath, MAX_PATH) == 0)
        return {};

    return modulePath;
}

LONG WINAPI writeCrashDump(EXCEPTION_POINTERS* exceptionPointers)
{
    static LONG alreadyHandling = 0;
    if (InterlockedExchange(&alreadyHandling, 1) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME now{};
    GetLocalTime(&now);

    wchar_t dumpName[MAX_PATH]{};
    swprintf_s(dumpName, L"halionbridge_crash_%04u%02u%02u_%02u%02u%02u_%lu.dmp", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, GetCurrentProcessId());

    const auto dumpFile = CreateFileW(dumpName, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    const auto exceptionCode = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
                                   ? exceptionPointers->ExceptionRecord->ExceptionCode
                                   : 0;

    const auto exceptionAddress = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
                                      ? exceptionPointers->ExceptionRecord->ExceptionAddress
                                      : nullptr;

    const auto faultingModule = getModulePathForAddress(exceptionAddress);

    std::cerr << "Fatal Windows exception in halionbridge."
              << "\n  Diagnostic phase: " << currentPhase.load() << "\n  Exception code: 0x" << std::hex << exceptionCode << std::dec
              << "\n  Exception address: " << exceptionAddress << "\n";

    if (!faultingModule.empty())
        std::wcerr << L"  Faulting module: " << faultingModule << L"\n";

    if (dumpFile == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error: Could not create crash dump file.\n";
        return EXCEPTION_CONTINUE_SEARCH;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = exceptionPointers;
    exceptionInfo.ClientPointers = FALSE;

    const auto dumpType =
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo);

    const auto wroteDump =
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dumpFile, dumpType, &exceptionInfo, nullptr, nullptr);

    CloseHandle(dumpFile);

    if (wroteDump)
        std::wcerr << L"Crash dump written: " << dumpName << L"\n";
    else
        std::cerr << "Error: MiniDumpWriteDump failed with Windows error " << GetLastError() << ".\n";

    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

} // namespace

void installCrashDiagnostics()
{
#if defined(_WIN32) && defined(HALIONBRIDGE_ENABLE_CRASH_DUMPS)
    SetUnhandledExceptionFilter(writeCrashDump);

#if defined(HALIONBRIDGE_DIAGNOSTIC_BUILD)
    log::debug("Crash diagnostics enabled. Unhandled Windows exceptions will write halionbridge_crash_*.dmp.");
#endif
#endif
}

void setCrashDiagnosticPhase(const char* phase)
{
    currentPhase.store(phase != nullptr ? phase : "unspecified");
}

} // namespace halionbridge
