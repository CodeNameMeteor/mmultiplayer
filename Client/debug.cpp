#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <conio.h>
#include <stdio.h>
#include <string>

#include "debug.h"
#include "engine.h"
#include "hook.h"
#include "pattern.h"

static bool GetImageBase(void *pc, void **base) {
    // CBA to use PEB
    const auto snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());

    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32 entry = {sizeof(MODULEENTRY32)};
    if (Module32First(snapshot, &entry)) {
        do {
            if (pc >= entry.modBaseAddr && pc < entry.modBaseAddr + entry.modBaseSize) {
                if (base) {
                    *base = reinterpret_cast<void *>(entry.modBaseAddr);
                }
                
                CloseHandle(snapshot);
                return true;
            }
        } while (Module32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return false;
}

static void SuspendOtherThreads() {
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    THREADENTRY32 entry = {sizeof(THREADENTRY32)};
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
                entry.th32ThreadID != GetCurrentThreadId()) {

                auto thread = OpenThread(THREAD_SUSPEND_RESUME, false, entry.th32ThreadID);

                if (thread) {
                    SuspendThread(thread);
                    CloseHandle(thread);
                }
            }
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

static std::string GetPrettyPointer(long ptr) {
    char buffer[MAX_PATH] = {0};

    void *base;
    if (GetImageBase(reinterpret_cast<void *>(ptr), &base)) {
        wchar_t fileName[MAX_PATH];

        if (GetModuleBaseName(GetCurrentProcess(), static_cast<HMODULE>(base), fileName,
                              ARRAYSIZE(fileName) - 1)) {

            sprintf_s(buffer, sizeof(buffer) - 1, "0x%08X (%ws+0x%X)", ptr, fileName,
                      ptr - reinterpret_cast<long>(base));

            return buffer;
        }
    }

    sprintf_s(buffer, sizeof(buffer) - 1, "%08X", ptr);
    return buffer;
}

static long WINAPI ExceptionHandler(EXCEPTION_POINTERS *exception) {
    if (IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SuspendOtherThreads();

    HWND window = Engine::GetWindow();
    if (window) {
        DestroyWindow(window);
    }

    Debug::CreateConsole();

    printf("An unhandled exception occured at:\n\t%s (0x%X)\n\n",
           GetPrettyPointer(exception->ContextRecord->Eip).c_str(),
           exception->ExceptionRecord->ExceptionCode);

    printf("Context:\n");
    printf("\tEAX=0x%08X ECX=0x%08X EDX=0x%08X EBX=0x%08X\n", exception->ContextRecord->Eax,
           exception->ContextRecord->Ecx, exception->ContextRecord->Edx,
           exception->ContextRecord->Ebx);
    printf("\tESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X\n\n", exception->ContextRecord->Esi,
           exception->ContextRecord->Edi, exception->ContextRecord->Ebp,
           exception->ContextRecord->Esp);

    printf("Stack trace:\n");
    auto esp = (exception->ContextRecord->Esp / sizeof(DWORD)) * sizeof(DWORD);
    for (auto i = 0; i < 200; ++i) {
        long ptr = 0;

        // Safe read without SEH
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(esp), &ptr,
                               sizeof(ptr), nullptr)) {
            break;
        }

        void *base;
        if (GetImageBase(reinterpret_cast<void *>(ptr), &base)) {
            printf("\t%s\n", GetPrettyPointer(ptr).c_str());
        }

        esp += sizeof(DWORD);
    }

    printf("\nIf this was unexpected, please copy this output and create an "
           "issue at:\n\thttps://github.com/Toyro98/mmultiplayer/issues\n\n");

    printf("Press any key to exit...");
    static_cast<void>(_getch());

    exit(1);

    return EXCEPTION_CONTINUE_SEARCH;
}

static void *(*SetUnhandledExceptionFilterOriginal)(void *) = nullptr;
static void *SetUnhandledExceptionFilterHook(void *callback) {
    if (callback != ExceptionHandler) {
        return nullptr;
    }

    return SetUnhandledExceptionFilterOriginal(callback);
}

void Debug::Initialize() {
    // Prevent our exception handler from being overwritten
    Hook::TrampolineHook(SetUnhandledExceptionFilterHook, SetUnhandledExceptionFilter,
                         reinterpret_cast<void **>(&SetUnhandledExceptionFilterOriginal));

    SetUnhandledExceptionFilter(ExceptionHandler);

    // Hook the game's existing exception handler
    auto ptr = Pattern::FindPattern("\xE8\x00\x00\x00\x00\x83\xC4\x04\xC3\x8B"
                                    "\x65\xE8\x8B\x8D\x00\x00\x00\x00",
                                    "x????xxxxxxxxx????");

    if (ptr) {
        Hook::TrampolineHook(ExceptionHandler, RELATIVE_ADDR(ptr, 5));
    }
}

void Debug::CreateConsole() {
    AllocConsole();

    FILE *old;
    static_cast<VOID>(freopen_s(&old, "CONIN$", "r", stdin));
    static_cast<VOID>(freopen_s(&old, "CONOUT$", "w", stdout));
    static_cast<VOID>(freopen_s(&old, "CONOUT$", "w", stderr));
}