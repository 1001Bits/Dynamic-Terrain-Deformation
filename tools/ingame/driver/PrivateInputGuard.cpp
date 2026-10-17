// Local test harness only; never packaged with the terrain mod.
// A private desktop still shares WinSta0's cursor. Suppress cursor writes inside
// this one process, including calls made through DirectInput or GetProcAddress.
#include "F4SE/F4SE.h"
#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
    constexpr std::uint64_t kSuccess = 0x9090C300000001B8ull; // x64: mov eax,1; ret; nop; nop
    bool installed = false;

    std::wstring Name(HANDLE object)
    {
        wchar_t name[256]{};
        DWORD needed = 0;
        if (!object || !GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name), &needed)) return {};
        return name;
    }

    bool Install()
    {
        if (installed) return true;
        wchar_t expected[256]{};
        const auto length = GetEnvironmentVariableW(L"NMN_TERRAIN_PRIVATE_DESKTOP", expected, 256);
        const std::wstring desktop = Name(GetThreadDesktop(GetCurrentThreadId()));
        if (!length || length >= 256 || desktop != expected || !desktop.starts_with(L"NmnTerrain_") ||
            desktop.size() != 43 || Name(GetProcessWindowStation()) != L"WinSta0") return false;
        const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        const auto inputName = Name(input);
        if (input) CloseDesktop(input);
        if (inputName.empty() || inputName == desktop) return false;

        struct Entry { const wchar_t* module; const char* name; };
        constexpr Entry entries[]{
            {L"user32.dll", "ClipCursor"}, {L"user32.dll", "SetCursorPos"},
            {L"user32.dll", "SetPhysicalCursorPos"},
            {L"win32u.dll", "NtUserClipCursor"}, {L"win32u.dll", "NtUserSetCursorPos"}
        };
        std::array<void*, std::size(entries)> addresses{};
        // Validate all targets before changing any. No trampoline is needed: these
        // BOOL APIs always return success without touching the shared cursor. The
        // aligned eight-byte replacement is atomic for other threads in this process.
        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto module = GetModuleHandleW(entries[i].module);
            auto* address = module ? reinterpret_cast<void*>(GetProcAddress(module, entries[i].name)) : nullptr;
            MEMORY_BASIC_INFORMATION info{};
            if (!address || (reinterpret_cast<std::uintptr_t>(address) & 7u) ||
                !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                !(info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
            addresses[i] = address;
        }
        for (auto* address : addresses) {
            DWORD old = 0, unused = 0;
            if (!VirtualProtect(address, sizeof(kSuccess), PAGE_EXECUTE_READWRITE, &old)) return false;
            InterlockedExchange64(static_cast<volatile LONG64*>(address), static_cast<LONG64>(kSuccess));
            const bool restored = VirtualProtect(address, sizeof(kSuccess), old, &unused) != FALSE;
            if (!FlushInstructionCache(GetCurrentProcess(), address, sizeof(kSuccess)) || !restored) return false;
        }
        installed = true;
        return true;
    }
}

extern "C" __declspec(dllexport) BOOL NmnInstallPrivateInputGuard() { return Install(); }

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* query, F4SE::PluginInfo* info)
{
    if (!query || !info) return false;
    info->infoVersion = F4SE::PluginInfo::kVersion;
    info->name = "NMN_PrivateInputGuard";
    info->version = 1;
    return !query->IsEditor();
}

extern "C" __declspec(dllexport) bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface*)
{
    wchar_t output[32768]{};
    const auto length = GetEnvironmentVariableW(L"NMN_TERRAIN_PRIVATE_GUARD_FILE", output, std::size(output));
    if (!length || length >= std::size(output)) return false; // Inert outside our launcher.
    if (!Install()) { ExitProcess(0xE001); }
    const std::filesystem::path path(output);
    if (!path.is_absolute()) { ExitProcess(0xE002); }
    std::ofstream file(path, std::ios::binary);
    file << "{\"pid\":" << GetCurrentProcessId() << ",\"cursorWritesBlocked\":true,\"apiCount\":5}\n";
    file.flush();
    if (!file) { ExitProcess(0xE003); }
    return true;
}

F4SE_EXPORT auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData data{};
    data.PluginName("NMN_PrivateInputGuard");
    data.PluginVersion(REL::Version{1, 0, 0, 0});
    data.AuthorName("NMN local validation");
    data.UsesAddressLibrary(false);
    data.IsLayoutDependent(false);
    data.CompatibleVersions({REL::Version{1, 10, 163, 0}, REL::Version{1, 11, 240, 0}, REL::Version{1, 2, 72, 0}});
    return data;
}();
