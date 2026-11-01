#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void* gOriginalOpen = nullptr;
extern "C" char* __cdecl ResolveReadOverride(const char* filename, const char* mode);
extern "C" void __cdecl LogOpenOutcome(const char* filename, int opened);
extern "C" int GameOpenHook();
extern "C" char* __cdecl ResolveProfileSavePath(const char* input);
static bool WaitForModsReadyBeforeGameOpen(const char* filename);

namespace
{
constexpr DWORD kPreferredImageBase = 0x00400000;
constexpr DWORD kOpenFileVa = 0x00697324;
constexpr DWORD kOpenFileRva = kOpenFileVa - kPreferredImageBase;
constexpr DWORD kOpenCallSiteVa = 0x00660817;
constexpr DWORD kOpenCallSiteRva = kOpenCallSiteVa - kPreferredImageBase;
constexpr size_t kPatchLength = 8;
constexpr size_t kMaxPathBuffer = 1024;
constexpr size_t kMaxIatPatches = 1024;

const BYTE kExpectedOpenPrologue[kPatchLength] = {
    0x53,                   // push ebx
    0x31, 0xDB,             // xor ebx, ebx
    0xE8, 0xB4, 0xFF, 0xFF, 0xFF // call 0x6972E0
};

const BYTE kExpectedOpenCallerSetup[] = {
    0x89, 0xC2,             // mov edx, eax (save mode returned by string accessor)
    0x89, 0xF0,             // mov eax, esi (load filename string object)
    0xE8, 0x33, 0x53, 0x03, 0x00, // call 0x695B53 (filename c-string accessor)
    0xE8, 0xFF, 0x6A, 0x03, 0x00  // call 0x697324 (file open)
};
volatile LONG gOpenTraceCount = 0;
volatile LONG gStartupReadyWaitTraceCount = 0;
volatile LONG gStartupReadyWaitTimedOut = 0;
volatile LONG gOpenResultTraceCount = 0;
volatile LONG gShopListResultTraceCount = 0;

struct ModEntry
{
    char name[MAX_PATH]{};
    int priority = 50;
};

struct IatPatch
{
    DWORD* slot{};
    DWORD original{};
};

struct SearchContext
{
    HANDLE token = INVALID_HANDLE_VALUE;
    std::vector<WIN32_FIND_DATAA> entries;
    size_t nextIndex = 0;
    SearchContext* next = nullptr;
};

using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using FindNextFileAFn = BOOL(WINAPI*)(HANDLE, LPWIN32_FIND_DATAA);
using FindCloseFn = BOOL(WINAPI*)(HANDLE);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using SetCurrentDirectoryAFn = BOOL(WINAPI*)(LPCSTR);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);

char gGameDir[MAX_PATH]{};

char gLogicalCwd[MAX_PATH]{};
char gModsDir[MAX_PATH]{};
char gProfilesDir[MAX_PATH]{};
char gProfilePath[MAX_PATH]{};
char gSelectedProfile[MAX_PATH] = "default";
char gIniPath[MAX_PATH]{};
char gLogPath[MAX_PATH]{};
std::vector<ModEntry> gMods;
size_t gModCount = 0;
volatile LONG gModsState = 0; // 0 = not started, 1 = loading, 2 = ready
HANDLE gAsiStartupEvent = nullptr;
HANDLE gModsReadyEvent = nullptr;
bool gEnabled = true;
bool gLogEnabled = false;
bool gLoadAsiPlugins = true;
bool gRootReady = false;
bool gOpenHookInstalled = false;
HMODULE gSelfModule = nullptr;
CRITICAL_SECTION gIatLock{};
bool gIatLockInitialized = false;
DWORD gIatPatchCount = 0;
IatPatch gIatPatches[kMaxIatPatches]{};
BYTE gSavedOpenBytes[kPatchLength]{};
BYTE* gOpenTarget = nullptr;
void* gOpenTrampoline = nullptr;
DWORD gPathTls = TLS_OUT_OF_INDEXES;
CRITICAL_SECTION gSearchLock{};
SearchContext* gSearchContexts = nullptr;

CRITICAL_SECTION gEditorMergeLock{};
bool gEditorMergeLockInitialized = false;
std::vector<char> gMergedEditorCategoryData[3];
bool gMergedEditorCategoryReady[3]{};

FindFirstFileAFn gRealFindFirstFileA = nullptr;
FindNextFileAFn gRealFindNextFileA = nullptr;
FindCloseFn gRealFindClose = nullptr;
GetFileAttributesAFn gRealGetFileAttributesA = nullptr;
SetCurrentDirectoryAFn gRealSetCurrentDirectoryA = nullptr;
CreateFileAFn gRealCreateFileA = nullptr;
CreateFileWFn gRealCreateFileW = nullptr;
LoadLibraryAFn gRealLoadLibraryA = nullptr;
LoadLibraryWFn gRealLoadLibraryW = nullptr;
LoadLibraryExAFn gRealLoadLibraryExA = nullptr;
LoadLibraryExWFn gRealLoadLibraryExW = nullptr;
GetProcAddressFn gRealGetProcAddress = nullptr;
FARPROC gGetProcAddressEntry = nullptr;
BYTE* gGetProcAddressTarget = nullptr;
BYTE* gGetProcAddressTrampoline = nullptr;
BYTE gSavedGetProcAddressBytes[5]{};
bool gGetProcAddressHookInstalled = false;
volatile LONG gGetProcRedirectTraceCount = 0;
volatile LONG gAttributeTraceCount = 0;
volatile LONG gPathMapTraceCount = 0;
volatile LONG gOpenMissTraceCount = 0;
volatile LONG gDirectoryMapTraceCount = 0;
volatile LONG gCwdTraceCount = 0;
volatile LONG gCreateFileTraceCount = 0;
volatile LONG gIatModuleTraceCount = 0;
volatile LONG gShuttingDown = 0;

void ForceLog(const char* format, ...);
void LogMessage(const char* format, ...);
void EnsureModsLoaded();
void LoadModAsiPlugins();

inline bool ModsReady()
{
    return InterlockedCompareExchange(&gModsState, 2, 2) == 2;
}

bool IsAbsolutePathA(const char* path)
{
    if (!path || !path[0])
        return false;
    if (((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
        return true;
    return path[0] == '\\' && path[1] == '\\';
}

void NormalizeSlashes(char* path)
{
    for (char* p = path; *p; ++p)
    {
        if (*p == '/')
            *p = '\\';
    }
}

bool IsSameOrChildPath(const char* path, const char* root)
{
    if (!path || !root)
        return false;
    const size_t rootLen = std::strlen(root);
    if (_strnicmp(path, root, rootLen) != 0)
        return false;
    return path[rootLen] == '\0' || path[rootLen] == '\\';
}

bool CopyString(char* destination, size_t destinationSize, const char* source);
bool BuildFullPath(const char* input, char* output, size_t outputSize);

bool DirectoryExistsInGameOrMods(const char* absolutePath);
bool RootRelativeResourceExists(const char* absolutePath);
bool IsModsRelativePath(const char* relative);
bool MakeModPhysicalPathRelative(const char* absolutePath, char* relative,
                                 size_t relativeSize, const char** matchedMod = nullptr);
bool TryMapRootShopListToLogicalCarDirectory(const char* absolutePath,
                                             char* relative, size_t relativeSize);
DWORD PatchModuleImports(HMODULE module, bool patchDirectoryApis);
DWORD WINAPI PatchLoadedModulesThread(LPVOID);
HANDLE WINAPI HookCreateFileA(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
HANDLE WINAPI HookCreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
HMODULE WINAPI HookLoadLibraryA(LPCSTR);
HMODULE WINAPI HookLoadLibraryW(LPCWSTR);
HMODULE WINAPI HookLoadLibraryExA(LPCSTR, HANDLE, DWORD);
HMODULE WINAPI HookLoadLibraryExW(LPCWSTR, HANDLE, DWORD);

bool BuildLogicalAbsolutePath(const char* input, char* output, size_t outputSize)
{
    if (!input || !input[0] || !output || outputSize == 0)
        return false;
    char raw[kMaxPathBuffer]{};
    if (!CopyString(raw, sizeof(raw), input))
        return false;
    NormalizeSlashes(raw);
    if (IsAbsolutePathA(raw) || raw[0] == '\\')
    {
        if (!BuildFullPath(raw, output, outputSize))
            return false;
        char modRelative[kMaxPathBuffer]{};
        if (MakeModPhysicalPathRelative(output, modRelative, sizeof(modRelative)))
        {
            char logical[kMaxPathBuffer]{};
            if (_snprintf_s(logical, sizeof(logical), _TRUNCATE, "%s\\%s", gGameDir, modRelative) < 0)
                return false;
            return BuildFullPath(logical, output, outputSize);
        }
        return true;
    }

    const bool hasPathSeparator = std::strchr(raw, '\\') != nullptr;
    const bool startsWithDot = raw[0] == '.';
    if (hasPathSeparator && !startsWithDot)
    {
        char rooted[kMaxPathBuffer]{};
        if (_snprintf_s(rooted, sizeof(rooted), _TRUNCATE, "%s\\%s", gGameDir, raw) >= 0)
        {
            char rootedAbsolute[kMaxPathBuffer]{};
            if (BuildFullPath(rooted, rootedAbsolute, sizeof(rootedAbsolute)) &&
                IsSameOrChildPath(rootedAbsolute, gGameDir) &&
                DirectoryExistsInGameOrMods(rootedAbsolute))
                return CopyString(output, outputSize, rootedAbsolute);
        }
    }

    const char* base = gLogicalCwd[0] ? gLogicalCwd : gGameDir;
    char joined[kMaxPathBuffer]{};
    if (_snprintf_s(joined, sizeof(joined), _TRUNCATE, "%s\\%s", base, raw) < 0)
        return false;
    return BuildFullPath(joined, output, outputSize);
}

bool PhysicalCwdIsInsideMods()
{
    char cwd[kMaxPathBuffer]{};
    DWORD length = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(cwd)), cwd);
    if (length == 0 || length >= sizeof(cwd))
        return false;
    NormalizeSlashes(cwd);
    return gModsDir[0] && IsSameOrChildPath(cwd, gModsDir);
}

bool GetGameRelativeTail(const char* absolutePath, char* tail, size_t tailSize, bool allowRoot = false)
{
    if (!absolutePath || !IsSameOrChildPath(absolutePath, gGameDir))
        return false;
    const size_t rootLength = std::strlen(gGameDir);
    const char* result = absolutePath + rootLength;
    while (*result == '\\')
        ++result;
    if (!result[0] && !allowRoot)
        return false;
    return CopyString(tail, tailSize, result);
}

bool IsModsRelativePath(const char* relative)
{
    if (!relative || !relative[0])
        return false;
    return _stricmp(relative, "modloader") == 0 || _strnicmp(relative, "modloader\\", 10) == 0;
}

bool MakeModPhysicalPathRelative(const char* absolutePath, char* relative,
                                 size_t relativeSize, const char** matchedMod)
{
    if (matchedMod)
        *matchedMod = nullptr;
    if (!absolutePath || !relative || relativeSize == 0 || !gModsDir[0] ||
        !IsSameOrChildPath(absolutePath, gModsDir))
        return false;

    const char* modTail = absolutePath + std::strlen(gModsDir);
    while (*modTail == '\\')
        ++modTail;
    const char* separator = std::strchr(modTail, '\\');
    if (!separator || !separator[1])
        return false;

    const size_t modNameLength = static_cast<size_t>(separator - modTail);
    const char* foundMod = nullptr;
    for (size_t i = 0; i < gModCount; ++i)
    {
        if (std::strlen(gMods[i].name) == modNameLength &&
            _strnicmp(gMods[i].name, modTail, modNameLength) == 0)
        {
            foundMod = gMods[i].name;
            break;
        }
    }
    const char* modRelative = separator + 1;
    if (!foundMod || !modRelative[0] || IsModsRelativePath(modRelative) ||
        !CopyString(relative, relativeSize, modRelative))
        return false;
    if (matchedMod)
        *matchedMod = foundMod;
    return true;
}

bool CopyString(char* destination, size_t destinationSize, const char* source)
{
    if (!destination || !destinationSize || !source)
        return false;
    const size_t n = std::strlen(source);
    if (n >= destinationSize)
    {
        destination[0] = '\0';
        return false;
    }
    std::memcpy(destination, source, n + 1);
    return true;
}

bool BuildFullPath(const char* input, char* output, size_t outputSize)
{
    if (!input || !input[0] || !output || outputSize == 0)
        return false;
    DWORD n = GetFullPathNameA(input, static_cast<DWORD>(outputSize), output, nullptr);
    if (n == 0 || n >= outputSize)
    {
        output[0] = '\0';
        return false;
    }
    NormalizeSlashes(output);
    return true;
}

bool MakeRelativeFilePath(const char* requestedPath, char* relative, size_t relativeSize)
{
    if (!gRootReady || !requestedPath || !requestedPath[0])
        return false;

    char raw[kMaxPathBuffer]{};
    if (!CopyString(raw, sizeof(raw), requestedPath))
        return false;
    NormalizeSlashes(raw);

    char absolute[kMaxPathBuffer]{};
    if (IsAbsolutePathA(raw))
    {
        if (!BuildFullPath(raw, absolute, sizeof(absolute)))
            return false;

        const char* physicalMod = nullptr;
        if (MakeModPhysicalPathRelative(absolute, relative, relativeSize, &physicalMod))
        {
            if (gLogEnabled)
                LogMessage("[OPEN-MOD-PATH-MAP] request=%s relative=%s mod=%s",
                           raw, relative, physicalMod ? physicalMod : "<unknown>");
            return true;
        }
        if (IsSameOrChildPath(absolute, gModsDir))
            return false;

        if (!IsSameOrChildPath(absolute, gGameDir))
            return false;

        if (TryMapRootShopListToLogicalCarDirectory(absolute, relative, relativeSize))
            return true;
    }
    else
    {
        bool usedRootRelativePath = false;
        const bool hasSeparator = std::strchr(raw, '\\') != nullptr;
        const bool startsWithDot = raw[0] == '.';
        if (hasSeparator && !startsWithDot)
        {
            char rooted[kMaxPathBuffer]{};
            char rootedAbsolute[kMaxPathBuffer]{};
            if (_snprintf_s(rooted, sizeof(rooted), _TRUNCATE, "%s\\%s", gGameDir, raw) >= 0 &&
                BuildFullPath(rooted, rootedAbsolute, sizeof(rootedAbsolute)) &&
                IsSameOrChildPath(rootedAbsolute, gGameDir) &&
                RootRelativeResourceExists(rootedAbsolute))
            {
                char cwdBased[kMaxPathBuffer]{};
                bool cwdDiffers = true;
                if (PhysicalCwdIsInsideMods() && gLogicalCwd[0])
                {
                    char logicalJoined[kMaxPathBuffer]{};
                    if (_snprintf_s(logicalJoined, sizeof(logicalJoined), _TRUNCATE,
                                    "%s\\%s", gLogicalCwd, raw) >= 0 &&
                        BuildFullPath(logicalJoined, cwdBased, sizeof(cwdBased)))
                        cwdDiffers = _stricmp(cwdBased, rootedAbsolute) != 0;
                }
                else if (BuildFullPath(raw, cwdBased, sizeof(cwdBased)))
                {
                    cwdDiffers = _stricmp(cwdBased, rootedAbsolute) != 0;
                }
                CopyString(absolute, sizeof(absolute), rootedAbsolute);
                usedRootRelativePath = true;
                if (cwdDiffers)
                    LogMessage("[OPEN-ROOT-MAP] request=%s cwdBased=%s rootBased=%s",
                               raw, cwdBased[0] ? cwdBased : "<unresolved>", rootedAbsolute);
            }
        }

        if (!usedRootRelativePath)
        {
            if (PhysicalCwdIsInsideMods() && gLogicalCwd[0])
            {
                char logicalJoined[kMaxPathBuffer]{};
                if (_snprintf_s(logicalJoined, sizeof(logicalJoined), _TRUNCATE,
                                "%s\\%s", gLogicalCwd, raw) < 0 ||
                    !BuildFullPath(logicalJoined, absolute, sizeof(absolute)) ||
                    !IsSameOrChildPath(absolute, gGameDir))
                    return false;
            }
            else if (!BuildFullPath(raw, absolute, sizeof(absolute)) ||
                     !IsSameOrChildPath(absolute, gGameDir))
            {
                char rooted[kMaxPathBuffer]{};
                if (_snprintf_s(rooted, sizeof(rooted), _TRUNCATE, "%s\\%s", gGameDir, raw) < 0 ||
                    !BuildFullPath(rooted, absolute, sizeof(absolute)) ||
                    !IsSameOrChildPath(absolute, gGameDir))
                    return false;
            }
        }
    }

    const size_t rootLen = std::strlen(gGameDir);
    const char* tail = absolute + rootLen;
    while (*tail == '\\')
        ++tail;
    if (!tail[0] || IsModsRelativePath(tail))
        return false;
    return CopyString(relative, relativeSize, tail);
}

bool MakeRelativeSearchPattern(const char* requestedPattern, char* relativePattern, size_t relativeSize)
{
    if (!gRootReady || !requestedPattern || !requestedPattern[0])
        return false;

    char raw[kMaxPathBuffer]{};
    if (!CopyString(raw, sizeof(raw), requestedPattern))
        return false;
    NormalizeSlashes(raw);

    char* lastSlash = std::strrchr(raw, '\\');
    char patternName[MAX_PATH]{};
    char directoryPart[kMaxPathBuffer]{};
    if (lastSlash)
    {
        if (!CopyString(patternName, sizeof(patternName), lastSlash + 1))
            return false;
        const size_t dirLen = static_cast<size_t>(lastSlash - raw);
        if (dirLen >= sizeof(directoryPart))
            return false;
        std::memcpy(directoryPart, raw, dirLen);
        directoryPart[dirLen] = '\0';
    }
    else
    {
        if (!CopyString(patternName, sizeof(patternName), raw))
            return false;
        directoryPart[0] = '\0';
    }

    if (!patternName[0] || std::strpbrk(patternName, "*?") == nullptr)
        return false;

    char absoluteDirectory[kMaxPathBuffer]{};
    if (!directoryPart[0])
    {
        if (PhysicalCwdIsInsideMods() && gLogicalCwd[0])
        {
            if (!CopyString(absoluteDirectory, sizeof(absoluteDirectory), gLogicalCwd))
                return false;
        }
        else
        {
            DWORD cwdLength = GetCurrentDirectoryA(static_cast<DWORD>(sizeof(absoluteDirectory)), absoluteDirectory);
            if (cwdLength == 0 || cwdLength >= sizeof(absoluteDirectory))
                return false;
            NormalizeSlashes(absoluteDirectory);
            if (!IsSameOrChildPath(absoluteDirectory, gGameDir))
            {
                if (!CopyString(absoluteDirectory, sizeof(absoluteDirectory), gGameDir))
                    return false;
            }
        }
    }
    else if (IsAbsolutePathA(directoryPart))
    {
        if (!BuildFullPath(directoryPart, absoluteDirectory, sizeof(absoluteDirectory)))
            return false;
    }
    else
    {
        if (PhysicalCwdIsInsideMods() && gLogicalCwd[0])
        {
            char joined[kMaxPathBuffer]{};
            if (_snprintf_s(joined, sizeof(joined), _TRUNCATE,
                            "%s\\%s", gLogicalCwd, directoryPart) < 0 ||
                !BuildFullPath(joined, absoluteDirectory, sizeof(absoluteDirectory)))
                return false;
        }
        else if (!BuildFullPath(directoryPart, absoluteDirectory, sizeof(absoluteDirectory)) ||
                 !IsSameOrChildPath(absoluteDirectory, gGameDir))
        {
            char rooted[kMaxPathBuffer]{};
            if (_snprintf_s(rooted, sizeof(rooted), _TRUNCATE, "%s\\%s", gGameDir, directoryPart) < 0 ||
                !BuildFullPath(rooted, absoluteDirectory, sizeof(absoluteDirectory)))
                return false;
        }
    }

    char physicalModDirectory[kMaxPathBuffer]{};
    const char* physicalMod = nullptr;
    if (MakeModPhysicalPathRelative(absoluteDirectory, physicalModDirectory,
                                    sizeof(physicalModDirectory), &physicalMod))
    {
        char combinedFromMod[kMaxPathBuffer]{};
        if (_snprintf_s(combinedFromMod, sizeof(combinedFromMod), _TRUNCATE,
                        "%s\\%s", physicalModDirectory, patternName) < 0 ||
            IsModsRelativePath(combinedFromMod))
            return false;
        if (gLogEnabled)
            LogMessage("[DIR-MOD-PATH-MAP] directory=%s relative=%s mod=%s",
                       absoluteDirectory, combinedFromMod, physicalMod ? physicalMod : "<unknown>");
        return CopyString(relativePattern, relativeSize, combinedFromMod);
    }

    if (IsSameOrChildPath(absoluteDirectory, gModsDir))
        return false;

    if (!IsSameOrChildPath(absoluteDirectory, gGameDir))
    {
        if (!gModsDir[0] || !IsSameOrChildPath(absoluteDirectory, gModsDir))
            return false;
        const char* modTail = absoluteDirectory + std::strlen(gModsDir);
        while (*modTail == '\\')
            ++modTail;
        const char* modSeparator = std::strchr(modTail, '\\');
        if (!modSeparator)
            return false;
        const size_t modNameLength = static_cast<size_t>(modSeparator - modTail);
        bool knownMod = false;
        for (size_t i = 0; i < gModCount; ++i)
        {
            if (std::strlen(gMods[i].name) == modNameLength &&
                _strnicmp(gMods[i].name, modTail, modNameLength) == 0)
            {
                knownMod = true;
                break;
            }
        }
        if (!knownMod)
            return false;
        const char* relativeDirectory = modSeparator + 1;
        char combinedFromMod[kMaxPathBuffer]{};
        if (relativeDirectory[0])
        {
            if (_snprintf_s(combinedFromMod, sizeof(combinedFromMod), _TRUNCATE,
                            "%s\\%s", relativeDirectory, patternName) < 0)
                return false;
        }
        else if (!CopyString(combinedFromMod, sizeof(combinedFromMod), patternName))
        {
            return false;
        }
        if (IsModsRelativePath(combinedFromMod))
            return false;
        return CopyString(relativePattern, relativeSize, combinedFromMod);
    }

    const size_t rootLen = std::strlen(gGameDir);
    const char* directoryTail = absoluteDirectory + rootLen;
    while (*directoryTail == '\\')
        ++directoryTail;

    char combined[kMaxPathBuffer]{};
    if (directoryTail[0])
    {
        if (_snprintf_s(combined, sizeof(combined), _TRUNCATE, "%s\\%s", directoryTail, patternName) < 0)
            return false;
    }
    else if (!CopyString(combined, sizeof(combined), patternName))
    {
        return false;
    }

    if (IsModsRelativePath(combined))
        return false;
    return CopyString(relativePattern, relativeSize, combined);
}

bool IsReadOnlyOpenMode(const char* mode)
{
    if (!mode || (mode[0] != 'r' && mode[0] != 'R'))
        return false;

    return std::strchr(mode, '+') == nullptr;
}

char* GetThreadPathBuffer()
{
    if (gPathTls == TLS_OUT_OF_INDEXES)
        return nullptr;
    char* buffer = static_cast<char*>(TlsGetValue(gPathTls));
    if (!buffer)
    {
        buffer = static_cast<char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kMaxPathBuffer));
        if (!buffer)
            return nullptr;
        if (!TlsSetValue(gPathTls, buffer))
        {
            HeapFree(GetProcessHeap(), 0, buffer);
            return nullptr;
        }
    }
    return buffer;
}

char* ResolveProfileSavePathInternal(const char* input)
{
    if (!input || !input[0] || !gRootReady || !gSelectedProfile[0])
        return nullptr;

    if (_stricmp(gSelectedProfile, "default") == 0)
        return nullptr;

    const char* match = nullptr;
    for (const char* p = input; *p; ++p)
    {
        if ((p == input || p[-1] == '\\' || p[-1] == '/') &&
            _strnicmp(p, "SAVEGAME", 8) == 0 &&
            (p[8] == '\\' || p[8] == '/' || p[8] == '\0'))
        {
            match = p;
            break;
        }
    }
    if (!match)
        return nullptr;
    if (!ModsReady() && !WaitForModsReadyBeforeGameOpen(input))
        return nullptr;

    char* output = GetThreadPathBuffer();
    if (!output)
        return nullptr;

    const char* tail = match + 8;
    while (*tail == '\\' || *tail == '/')
        ++tail;

    const int written = _snprintf_s(output, kMaxPathBuffer, _TRUNCATE,
        "%s\\.savegames\\%s%s%s",
        gModsDir, gSelectedProfile, tail[0] ? "\\" : "", tail);
    if (written < 0)
        return nullptr;

    NormalizeSlashes(output);
    return output;
}

bool BuildModFilePath(const ModEntry& mod, const char* relative, char* output, size_t outputSize)
{
    char modRoot[kMaxPathBuffer]{};
    if (_snprintf_s(modRoot, sizeof(modRoot), _TRUNCATE, "%s\\%s", gModsDir, mod.name) < 0)
        return false;
    return _snprintf_s(output, outputSize, _TRUNCATE, "%s\\%s", modRoot, relative) >= 0;
}

constexpr size_t kMaxEditorCategorySourceBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxMergedEditorCategoryBytes = 8u * 1024u * 1024u;

int ResolveEditorCategoryRelative(const char* relative, char* canonical, size_t canonicalSize)
{
    if (!relative || !canonical || canonicalSize == 0)
        return -1;

    char normalized[kMaxPathBuffer]{};
    if (!CopyString(normalized, sizeof(normalized), relative))
        return -1;
    NormalizeSlashes(normalized);

    static const char* kNames[] = { "fields.cat", "cps.cat", "dynobj.cat" };
    for (int i = 0; i < 3; ++i)
    {
        const size_t nameLength = std::strlen(kNames[i]);
        const size_t pathLength = std::strlen(normalized);
        if (pathLength < nameLength || _stricmp(normalized + pathLength - nameLength, kNames[i]) != 0)
            continue;

        const char* suffix = normalized + pathLength - nameLength;
        bool isEditorCatalog = false;
        if (suffix == normalized)
        {
            isEditorCatalog = true;
        }
        else if (suffix > normalized && suffix[-1] == '\\')
        {
            const size_t prefixLength = static_cast<size_t>(suffix - normalized - 1);
            const char* editorMarker = "EDITOR";
            const size_t markerLength = std::strlen(editorMarker);
            if (prefixLength >= markerLength &&
                _strnicmp(normalized + prefixLength - markerLength, editorMarker, markerLength) == 0 &&
                (prefixLength == markerLength || normalized[prefixLength - markerLength - 1] == '\\'))
                isEditorCatalog = true;
        }

        if (!isEditorCatalog)
            continue;
        if (_snprintf_s(canonical, canonicalSize, _TRUNCATE,
                        "TRKDATA\\EDITOR\\%s", kNames[i]) < 0)
            return -1;
        return i;
    }
    return -1;
}

int GetEditorCategoryIndex(const char* relative)
{
    char canonical[MAX_PATH]{};
    return ResolveEditorCategoryRelative(relative, canonical, sizeof(canonical));
}

bool ReadNativeFile(const char* path, std::vector<char>& contents)
{
    if (!path || !path[0] || !gRealCreateFileA)
        return false;
    HANDLE file = gRealCreateFileA(path, GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
        static_cast<unsigned long long>(fileSize.QuadPart) > kMaxEditorCategorySourceBytes)
    {
        CloseHandle(file);
        return false;
    }

    contents.clear();
    contents.resize(static_cast<size_t>(fileSize.QuadPart));
    size_t totalRead = 0;
    while (totalRead < contents.size())
    {
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(contents.size() - totalRead, 1024u * 1024u));
        DWORD bytesRead = 0;
        if (!ReadFile(file, contents.data() + totalRead, remaining, &bytesRead, nullptr) || bytesRead == 0)
        {
            CloseHandle(file);
            contents.clear();
            return false;
        }
        totalRead += bytesRead;
    }
    CloseHandle(file);
    return true;
}

bool ParseEditorCategoryDocument(const std::vector<char>& source,
                                 unsigned short& setCount,
                                 size_t& countLineStart,
                                 size_t& bodyOffset,
                                 const char** failureReason = nullptr)
{
    setCount = 0;
    countLineStart = 0;
    bodyOffset = 0;
    if (failureReason)
        *failureReason = "unknown";
    if (source.empty())
    {
        if (failureReason) *failureReason = "empty file";
        return false;
    }

    size_t cursor = 0;

    if (source.size() >= 3 &&
        static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF)
        cursor = 3;

    constexpr size_t kMaxHeaderLines = 64;
    size_t linesScanned = 0;
    while (cursor < source.size() && linesScanned < kMaxHeaderLines)
    {
        const size_t lineStart = cursor;
        size_t lineEnd = lineStart;
        while (lineEnd < source.size() && source[lineEnd] != '\r' && source[lineEnd] != '\n')
            ++lineEnd;

        size_t contentStart = lineStart;
        while (contentStart < lineEnd && (source[contentStart] == ' ' || source[contentStart] == '\t'))
            ++contentStart;
        size_t contentEnd = lineEnd;
        while (contentEnd > contentStart && (source[contentEnd - 1] == ' ' || source[contentEnd - 1] == '\t'))
            --contentEnd;

        if (contentEnd > contentStart &&
            contentEnd - contentStart >= 5 &&
            _strnicmp(source.data() + contentStart, "Next:", 5) == 0)
        {
            if (failureReason) *failureReason = "Next: encountered before numeric set count";
            return false;
        }

        bool hasDigits = contentStart < contentEnd;
        size_t digitCursor = contentStart;
        unsigned long parsed = 0;
        size_t digits = 0;
        while (digitCursor < contentEnd && source[digitCursor] >= '0' && source[digitCursor] <= '9')
        {
            const unsigned int digit = static_cast<unsigned int>(source[digitCursor] - '0');
            if (parsed > (32767UL - digit) / 10UL)
            {
                hasDigits = false;
                break;
            }
            parsed = parsed * 10UL + digit;
            ++digitCursor;
            ++digits;
        }
        hasDigits = hasDigits && digits > 0;

        if (hasDigits)
        {
            size_t tail = digitCursor;
            while (tail < contentEnd && (source[tail] == ' ' || source[tail] == '\t'))
                ++tail;
            if (tail < contentEnd && source[tail] != '#')
                hasDigits = false;
        }

        if (hasDigits && parsed > 0 && parsed <= 32767UL)
        {
            size_t next = lineEnd;
            if (next < source.size() && source[next] == '\r') ++next;
            if (next < source.size() && source[next] == '\n') ++next;

            while (next < source.size())
            {
                size_t nextEnd = next;
                while (nextEnd < source.size() && source[nextEnd] != '\r' && source[nextEnd] != '\n')
                    ++nextEnd;
                size_t first = next;
                while (first < nextEnd && (source[first] == ' ' || source[first] == '\t'))
                    ++first;

                const bool blankLine = first == nextEnd;
                const bool commentOnlyLine = first < nextEnd && source[first] == '#';
                if (!blankLine && !commentOnlyLine)
                {
                    if (nextEnd - first >= 5 && _strnicmp(source.data() + first, "Next:", 5) == 0)
                    {
                        countLineStart = lineStart;
                        bodyOffset = next;
                        setCount = static_cast<unsigned short>(parsed);
                        if (failureReason) *failureReason = nullptr;
                        return true;
                    }

                    break;
                }

                next = nextEnd;
                if (next < source.size() && source[next] == '\r') ++next;
                if (next < source.size() && source[next] == '\n') ++next;
            }
        }

        if (lineEnd >= source.size())
            break;
        cursor = lineEnd;
        if (cursor < source.size() && source[cursor] == '\r') ++cursor;
        if (cursor < source.size() && source[cursor] == '\n') ++cursor;
        ++linesScanned;
    }

    if (failureReason)
        *failureReason = linesScanned >= kMaxHeaderLines ? "set count not found within first 64 header lines" :
                         "no numeric set count followed by Next: record";
    return false;
}

void BuildEditorCategoryPreview(const std::vector<char>& source, char* output, size_t outputSize)
{
    if (!output || outputSize == 0)
        return;
    output[0] = '\0';
    size_t used = 0;
    const size_t limit = std::min<size_t>(source.size(), 48);
    for (size_t i = 0; i < limit && used + 2 < outputSize; ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(source[i]);
        char out = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '.';
        if (out == '\\')
        {
            if (used + 2 >= outputSize) break;
            output[used++] = '\\';
            output[used++] = '\\';
        }
        else
            output[used++] = out;
    }
    output[used] = '\0';
}

bool AppendEditorCategoryBody(std::vector<char>& mergedBody,
                              const std::vector<char>& source,
                              size_t bodyOffset)
{
    if (bodyOffset >= source.size())
        return false;

    size_t sourceStart = bodyOffset;
    size_t sourceEnd = source.size();

    while (sourceStart < sourceEnd &&
           (source[sourceStart] == '\r' || source[sourceStart] == '\n'))
        ++sourceStart;
    while (sourceEnd > sourceStart &&
           (source[sourceEnd - 1] == '\r' || source[sourceEnd - 1] == '\n'))
        --sourceEnd;
    if (sourceStart >= sourceEnd)
        return false;

    const bool needsSeparator = !mergedBody.empty();
    const size_t sourceBytes = sourceEnd - sourceStart;

    const size_t separatorBytes = needsSeparator ? 4u : 0u;
    if (mergedBody.size() > kMaxMergedEditorCategoryBytes ||
        sourceBytes > kMaxMergedEditorCategoryBytes - mergedBody.size() ||
        separatorBytes > kMaxMergedEditorCategoryBytes - mergedBody.size() - sourceBytes)
        return false;

    if (needsSeparator)
    {
        mergedBody.push_back('\r');
        mergedBody.push_back('\n');
        mergedBody.push_back('\r');
        mergedBody.push_back('\n');
    }
    mergedBody.insert(mergedBody.end(), source.begin() + sourceStart, source.begin() + sourceEnd);
    return true;
}

bool ReadEditorCategoryLine(const std::vector<char>& source, size_t& cursor, std::string& line)
{
    line.clear();
    while (cursor < source.size())
    {
        const size_t start = cursor;
        while (cursor < source.size() && source[cursor] != '\r' && source[cursor] != '\n')
            ++cursor;
        const size_t end = cursor;
        if (cursor < source.size() && source[cursor] == '\r') ++cursor;
        if (cursor < source.size() && source[cursor] == '\n') ++cursor;

        size_t first = start;
        size_t last = end;
        while (first < last && (source[first] == ' ' || source[first] == '\t')) ++first;
        while (last > first && (source[last - 1] == ' ' || source[last - 1] == '\t')) --last;
        if (first == last || source[first] == '#')
            continue;
        line.assign(source.data() + first, last - first);
        return true;
    }
    return false;
}

bool ParseEditorCategoryUnsigned(const std::string& line, unsigned long& value)
{
    value = 0;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const size_t digitStart = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
    {
        const unsigned long digit = static_cast<unsigned long>(line[i] - '0');
        if (value > (32767UL - digit) / 10UL)
            return false;
        value = value * 10UL + digit;
        ++i;
    }
    if (i == digitStart)
        return false;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i == line.size() || line[i] == '#';
}

bool ValidateEditorCategoryStructure(const std::vector<char>& source,
                                     unsigned short expectedSets,
                                     const char** failureReason = nullptr)
{
    if (failureReason) *failureReason = "unknown CAT structure error";
    unsigned short declaredSets = 0;
    size_t countLineStart = 0;
    size_t bodyOffset = 0;
    const char* parseReason = nullptr;
    if (!ParseEditorCategoryDocument(source, declaredSets, countLineStart, bodyOffset, &parseReason))
    {
        if (failureReason) *failureReason = parseReason ? parseReason : "count header parse failed";
        return false;
    }
    if (declaredSets != expectedSets)
    {
        if (failureReason) *failureReason = "declared set count mismatch";
        return false;
    }

    size_t cursor = bodyOffset;
    std::string line;
    for (unsigned short setIndex = 0; setIndex < declaredSets; ++setIndex)
    {
        if (!ReadEditorCategoryLine(source, cursor, line))
        {
            if (failureReason) *failureReason = "missing Next marker";
            return false;
        }
        const size_t comment = line.find('#');
        std::string marker = line.substr(0, comment);
        while (!marker.empty() && (marker.back() == ' ' || marker.back() == '\t')) marker.pop_back();
        if (marker != "Next:")
        {
            if (failureReason) *failureReason = "expected Next marker";
            return false;
        }

        if (!ReadEditorCategoryLine(source, cursor, line))
        {
            if (failureReason) *failureReason = "missing category label";
            return false;
        }
        if (!ReadEditorCategoryLine(source, cursor, line))
        {
            if (failureReason) *failureReason = "missing object count";
            return false;
        }
        unsigned long objectCount = 0;
        if (!ParseEditorCategoryUnsigned(line, objectCount))
        {
            if (failureReason) *failureReason = "invalid object count";
            return false;
        }

        for (unsigned long objectIndex = 0; objectIndex < objectCount; ++objectIndex)
        {
            if (!ReadEditorCategoryLine(source, cursor, line))
            {
                if (failureReason) *failureReason = "object count exceeds available rows";
                return false;
            }
            const size_t rowComment = line.find('#');
            std::string rowHead = line.substr(0, rowComment);
            while (!rowHead.empty() && (rowHead.back() == ' ' || rowHead.back() == '\t')) rowHead.pop_back();
            if (rowHead == "Next:")
            {
                if (failureReason) *failureReason = "Next marker encountered before object count completed";
                return false;
            }
        }
    }

    while (ReadEditorCategoryLine(source, cursor, line))
    {
        if (failureReason) *failureReason = "unexpected non-comment data after declared CAT sets";
        return false;
    }
    if (failureReason) *failureReason = nullptr;
    return true;
}

const char* GetEditorCategoryMemoryPath(int categoryIndex)
{
    static const char* const kVirtualPaths[3] = {
        "__CDML_INMEM_EDITOR_FIELDS__.cat",
        "__CDML_INMEM_EDITOR_CPS__.cat",
        "__CDML_INMEM_EDITOR_DYNOBJ__.cat"
    };
    return categoryIndex >= 0 && categoryIndex < 3 ? kVirtualPaths[categoryIndex] : nullptr;
}

int GetEditorCategoryMemoryIndexFromPath(const char* path)
{
    if (!path || !path[0])
        return -1;
    const char* leaf = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/')
            leaf = p + 1;
    for (int i = 0; i < 3; ++i)
    {
        const char* virtualPath = GetEditorCategoryMemoryPath(i);
        if (virtualPath && _stricmp(leaf, virtualPath) == 0)
            return i;
    }
    return -1;
}

struct EditorCategoryPipeWriterContext
{
    HANDLE writeHandle = INVALID_HANDLE_VALUE;
    BYTE* bytes = nullptr;
    DWORD byteCount = 0;
};

DWORD WINAPI EditorCategoryPipeWriterThread(LPVOID parameter)
{
    EditorCategoryPipeWriterContext* context = static_cast<EditorCategoryPipeWriterContext*>(parameter);
    if (!context)
        return 0;

    DWORD offset = 0;
    while (offset < context->byteCount)
    {
        const DWORD chunk = std::min<DWORD>(context->byteCount - offset, 64u * 1024u);
        DWORD bytesWritten = 0;
        if (!WriteFile(context->writeHandle, context->bytes + offset, chunk, &bytesWritten, nullptr) ||
            bytesWritten == 0)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA &&
                InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
                LogMessage("[EDITOR-CAT-MEMORY-WRITE-FAILED] error=%lu offset=%lu total=%lu",
                           error, offset, context->byteCount);
            break;
        }
        offset += bytesWritten;
    }

    if (context->writeHandle != INVALID_HANDLE_VALUE)
        CloseHandle(context->writeHandle);
    if (context->bytes)
        HeapFree(GetProcessHeap(), 0, context->bytes);
    delete context;
    return 0;
}

HANDLE OpenEditorCategoryMemoryPipe(int categoryIndex)
{
    if (categoryIndex < 0 || categoryIndex >= 3 || !gEditorMergeLockInitialized)
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    BYTE* dataCopy = nullptr;
    DWORD dataSize = 0;
    EnterCriticalSection(&gEditorMergeLock);
    if (gMergedEditorCategoryReady[categoryIndex] &&
        !gMergedEditorCategoryData[categoryIndex].empty() &&
        gMergedEditorCategoryData[categoryIndex].size() <= MAXDWORD)
    {
        dataSize = static_cast<DWORD>(gMergedEditorCategoryData[categoryIndex].size());
        dataCopy = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, dataSize));
        if (dataCopy)
            std::memcpy(dataCopy, gMergedEditorCategoryData[categoryIndex].data(), dataSize);
    }
    LeaveCriticalSection(&gEditorMergeLock);

    if (!dataCopy || dataSize == 0)
    {
        if (dataCopy)
            HeapFree(GetProcessHeap(), 0, dataCopy);
        SetLastError(ERROR_FILE_NOT_FOUND);
        LogMessage("[EDITOR-CAT-MEMORY-OPEN-FAILED] index=%d (no cached bytes or allocation failed)", categoryIndex);
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = FALSE;
    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0))
    {
        const DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, dataCopy);
        SetLastError(error);
        LogMessage("[EDITOR-CAT-MEMORY-OPEN-FAILED] CreatePipe error=%lu", error);
        return INVALID_HANDLE_VALUE;
    }

    EditorCategoryPipeWriterContext* context = new (std::nothrow) EditorCategoryPipeWriterContext();
    if (!context)
    {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        HeapFree(GetProcessHeap(), 0, dataCopy);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    context->writeHandle = writePipe;
    context->bytes = dataCopy;
    context->byteCount = dataSize;

    HANDLE writer = CreateThread(nullptr, 0, EditorCategoryPipeWriterThread, context, 0, nullptr);
    if (!writer)
    {
        const DWORD error = GetLastError();
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        HeapFree(GetProcessHeap(), 0, dataCopy);
        delete context;
        SetLastError(error);
        LogMessage("[EDITOR-CAT-MEMORY-OPEN-FAILED] CreateThread error=%lu", error);
        return INVALID_HANDLE_VALUE;
    }
    CloseHandle(writer);

    static const char* const kNames[3] = { "fields.cat", "cps.cat", "dynobj.cat" };
    LogMessage("[EDITOR-CAT-MEMORY-OPEN] %s bytes=%lu backing=anonymous-pipe", kNames[categoryIndex], dataSize);
    return readPipe;
}

int BuildMergedEditorCategory(const char* relative, char* output, size_t outputSize)
{
    const int categoryIndex = GetEditorCategoryIndex(relative);
    if (categoryIndex < 0)
        return 0;
    if (!gEditorMergeLockInitialized)
        return -1;

    EnterCriticalSection(&gEditorMergeLock);
    if (gMergedEditorCategoryReady[categoryIndex])
    {
        const bool copied = CopyString(output, outputSize, GetEditorCategoryMemoryPath(categoryIndex));
        LeaveCriticalSection(&gEditorMergeLock);
        return copied ? 1 : -1;
    }

    std::vector<std::string> modFiles;
    modFiles.reserve(gModCount);
    for (size_t i = 0; i < gModCount; ++i)
    {
        char candidate[kMaxPathBuffer]{};
        if (!BuildModFilePath(gMods[i], relative, candidate, sizeof(candidate)))
            continue;
        const DWORD attributes = gRealGetFileAttributesA
            ? gRealGetFileAttributesA(candidate) : INVALID_FILE_ATTRIBUTES;
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        modFiles.emplace_back(candidate);
    }

    if (modFiles.empty())
    {
        LogMessage("[EDITOR-CAT-NO-MOD] %s profile=%s activeMods=%u",
                   relative, gSelectedProfile, static_cast<unsigned>(gModCount));
        LeaveCriticalSection(&gEditorMergeLock);
        return 0;
    }

    char stockPath[kMaxPathBuffer]{};
    if (_snprintf_s(stockPath, sizeof(stockPath), _TRUNCATE, "%s\\%s", gGameDir, relative) < 0)
    {
        LeaveCriticalSection(&gEditorMergeLock);
        return -1;
    }

    std::vector<char> mergedBody;
    std::vector<char> outputPrefix;
    bool haveOutputPrefix = false;
    unsigned long totalSets = 0;
    unsigned long stockSetCount = 0;
    unsigned long modSetCount = 0;
    size_t appendedModCount = 0;
    const DWORD stockAttributes = gRealGetFileAttributesA
        ? gRealGetFileAttributesA(stockPath) : INVALID_FILE_ATTRIBUTES;
    if (stockAttributes != INVALID_FILE_ATTRIBUTES && !(stockAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        std::vector<char> stockContents;
        unsigned short stockSets = 0;
        size_t stockCountLineStart = 0;
        size_t stockBodyOffset = 0;
        const char* parseReason = nullptr;
        if (!ReadNativeFile(stockPath, stockContents))
        {
            LogMessage("[EDITOR-CAT-MERGE-FAILED] could not read stock CAT path=%s error=%lu; using original",
                       stockPath, GetLastError());
            LeaveCriticalSection(&gEditorMergeLock);
            return -1;
        }
        if (!ParseEditorCategoryDocument(stockContents, stockSets, stockCountLineStart,
                                         stockBodyOffset, &parseReason) ||
            !ValidateEditorCategoryStructure(stockContents, stockSets, &parseReason) ||
            !AppendEditorCategoryBody(mergedBody, stockContents, stockBodyOffset))
        {
            char preview[96]{};
            BuildEditorCategoryPreview(stockContents, preview, sizeof(preview));
            LogMessage("[EDITOR-CAT-MERGE-FAILED] invalid stock CAT path=%s reason=%s bytes=%u prefix=%s; using original",
                       stockPath, parseReason ? parseReason : "empty body",
                       static_cast<unsigned>(stockContents.size()), preview);
            LeaveCriticalSection(&gEditorMergeLock);
            return -1;
        }
        outputPrefix.assign(stockContents.begin(), stockContents.begin() + stockCountLineStart);
        haveOutputPrefix = true;
        totalSets = stockSets;
        stockSetCount = stockSets;
        LogMessage("[EDITOR-CAT-STOCK] path=%s sets=%u countLineOffset=%u bodyOffset=%u prefixBytes=%u bytes=%u",
                   stockPath, static_cast<unsigned>(stockSets),
                   static_cast<unsigned>(stockCountLineStart), static_cast<unsigned>(stockBodyOffset),
                   static_cast<unsigned>(stockCountLineStart), static_cast<unsigned>(stockContents.size()));
    }
    else
    {
        LogMessage("[EDITOR-CAT-STOCK-MISSING] path=%s; building from active mod CAT files only", stockPath);
    }

    for (size_t i = 0; i < modFiles.size(); ++i)
    {
        std::vector<char> modContents;
        if (!ReadNativeFile(modFiles[i].c_str(), modContents))
        {
            LogMessage("[EDITOR-CAT-MERGE-SKIP] could not read mod catalog %s", modFiles[i].c_str());
            continue;
        }

        unsigned short setsInMod = 0;
        size_t modCountLineStart = 0;
        size_t modBodyOffset = 0;
        const char* modParseReason = nullptr;
        if (!ParseEditorCategoryDocument(modContents, setsInMod, modCountLineStart,
                                         modBodyOffset, &modParseReason) ||
            !ValidateEditorCategoryStructure(modContents, setsInMod, &modParseReason))
        {
            char preview[96]{};
            BuildEditorCategoryPreview(modContents, preview, sizeof(preview));
            LogMessage("[EDITOR-CAT-MERGE-SKIP] invalid CAT %s reason=%s bytes=%u prefix=%s",
                       modFiles[i].c_str(), modParseReason ? modParseReason : "unknown",
                       static_cast<unsigned>(modContents.size()), preview);
            continue;
        }
        if (!haveOutputPrefix)
        {
            outputPrefix.assign(modContents.begin(), modContents.begin() + modCountLineStart);
            haveOutputPrefix = true;
        }
        if (totalSets + setsInMod > 32767UL)
        {
            LogMessage("[EDITOR-CAT-MERGE-FAILED] set count exceeds signed-short limit (32767) for %s",
                       relative);
            LeaveCriticalSection(&gEditorMergeLock);
            return -1;
        }
        if (!AppendEditorCategoryBody(mergedBody, modContents, modBodyOffset))
        {
            LogMessage("[EDITOR-CAT-MERGE-FAILED] merged body exceeds %u bytes for %s",
                       static_cast<unsigned>(kMaxMergedEditorCategoryBytes), relative);
            LeaveCriticalSection(&gEditorMergeLock);
            return -1;
        }

        totalSets += setsInMod;
        modSetCount += setsInMod;
        ++appendedModCount;
        LogMessage("[EDITOR-CAT-MOD] path=%s sets=%u bodyOffset=%u bytes=%u cumulativeSets=%lu",
                   modFiles[i].c_str(), static_cast<unsigned>(setsInMod),
                   static_cast<unsigned>(modBodyOffset), static_cast<unsigned>(modContents.size()), totalSets);
    }

    if (appendedModCount == 0 || totalSets == 0 || mergedBody.empty())
    {
        LogMessage("[EDITOR-CAT-MERGE-FAILED] no valid mod CAT fragments for %s; using original",
                   relative);
        LeaveCriticalSection(&gEditorMergeLock);
        return -1;
    }

    char countLine[128]{};
    const int countLength = _snprintf_s(countLine, sizeof(countLine), _TRUNCATE,
                                        "%lu\t\t\t\t\t\t# Number of categories\r\n\r\n", totalSets);
    if (countLength < 0 || mergedBody.size() > kMaxMergedEditorCategoryBytes - static_cast<size_t>(countLength))
    {
        LogMessage("[EDITOR-CAT-MERGE-FAILED] complete CAT exceeds %u bytes for %s",
                   static_cast<unsigned>(kMaxMergedEditorCategoryBytes), relative);
        LeaveCriticalSection(&gEditorMergeLock);
        return -1;
    }

    if (!haveOutputPrefix)
    {
        LogMessage("[EDITOR-CAT-MERGE-FAILED] no valid CAT preamble for %s; using original", relative);
        LeaveCriticalSection(&gEditorMergeLock);
        return -1;
    }

    std::vector<char> merged;
    merged.reserve(outputPrefix.size() + static_cast<size_t>(countLength) + mergedBody.size());
    merged.insert(merged.end(), outputPrefix.begin(), outputPrefix.end());
    merged.insert(merged.end(), countLine, countLine + countLength);
    merged.insert(merged.end(), mergedBody.begin(), mergedBody.end());

    const char* mergedValidationReason = nullptr;
    if (totalSets > 32767UL ||
        !ValidateEditorCategoryStructure(merged, static_cast<unsigned short>(totalSets), &mergedValidationReason))
    {
        LogMessage("[EDITOR-CAT-MERGE-FAILED] generated CAT failed structural validation path=%s reason=%s totalSets=%lu bytes=%u; using original",
                   relative, mergedValidationReason ? mergedValidationReason : "set count exceeds signed-short range",
                   totalSets, static_cast<unsigned>(merged.size()));
        LeaveCriticalSection(&gEditorMergeLock);
        return -1;
    }
    LogMessage("[EDITOR-CAT-MERGE-VALIDATED] path=%s declaredSets=%lu bytes=%u",
               relative, totalSets, static_cast<unsigned>(merged.size()));

    gMergedEditorCategoryData[categoryIndex].swap(merged);
    gMergedEditorCategoryReady[categoryIndex] = true;
    const char* memoryPath = GetEditorCategoryMemoryPath(categoryIndex);
    const bool copied = CopyString(output, outputSize, memoryPath);
    static const char* const kCategoryNames[3] = { "fields.cat", "cps.cat", "dynobj.cat" };
    LogMessage("[EDITOR-CAT-MERGED] %s stock=%d stockSets=%lu modFiles=%u appended=%u modSets=%lu totalSets=%lu prefixBytes=%u bytes=%u storage=process-memory virtualPath=%s",
               relative, (stockAttributes != INVALID_FILE_ATTRIBUTES &&
                          !(stockAttributes & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0,
               stockSetCount, static_cast<unsigned>(modFiles.size()),
               static_cast<unsigned>(appendedModCount), modSetCount, totalSets,
               static_cast<unsigned>(outputPrefix.size()),
               static_cast<unsigned>(gMergedEditorCategoryData[categoryIndex].size()),
               memoryPath ? memoryPath : "<invalid>");
    LogMessage("[EDITOR-CAT-MEMORY-CACHED] %s bytes=%u", kCategoryNames[categoryIndex],
               static_cast<unsigned>(gMergedEditorCategoryData[categoryIndex].size()));
    LeaveCriticalSection(&gEditorMergeLock);
    return copied ? 1 : -1;
}

void CleanupMergedEditorCategories()
{
    if (!gEditorMergeLockInitialized)
        return;
    EnterCriticalSection(&gEditorMergeLock);
    for (int i = 0; i < 3; ++i)
    {
        std::vector<char>().swap(gMergedEditorCategoryData[i]);
        gMergedEditorCategoryReady[i] = false;
    }
    LeaveCriticalSection(&gEditorMergeLock);
    LogMessage("[EDITOR-CAT-MEMORY-CLEANUP] in-memory merged catalogs released");
}

bool RootRelativeResourceExists(const char* absolutePath)
{
    if (!absolutePath || !IsSameOrChildPath(absolutePath, gGameDir))
        return false;
    if (GetFileAttributesA(absolutePath) != INVALID_FILE_ATTRIBUTES)
        return true;

    char relative[kMaxPathBuffer]{};
    if (!GetGameRelativeTail(absolutePath, relative, sizeof(relative)) || !relative[0] ||
        IsModsRelativePath(relative))
        return false;

    char candidate[kMaxPathBuffer]{};
    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        if (!BuildModFilePath(gMods[i], relative, candidate, sizeof(candidate)))
            continue;
        if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES)
            return true;
    }
    return false;
}

bool ContainsInsensitive(const char* value, const char* token)
{
    if (!value || !token || !token[0])
        return false;
    const size_t tokenLength = std::strlen(token);
    for (const char* cursor = value; *cursor; ++cursor)
    {
        if (_strnicmp(cursor, token, tokenLength) == 0)
            return true;
    }
    return false;
}

bool TryMapRootShopListToLogicalCarDirectory(const char* absolutePath,
                                             char* relative, size_t relativeSize)
{
    if (!absolutePath || !relative || relativeSize == 0 || !gEnabled || gModCount == 0 ||
        !PhysicalCwdIsInsideMods() || !gLogicalCwd[0] ||
        !IsSameOrChildPath(gLogicalCwd, gGameDir))
        return false;

    char requestedTail[kMaxPathBuffer]{};
    if (!GetGameRelativeTail(absolutePath, requestedTail, sizeof(requestedTail)) ||
        _stricmp(requestedTail, "shop.lst") != 0)
        return false;

    if (GetFileAttributesA(absolutePath) != INVALID_FILE_ATTRIBUTES)
        return false;

    char rootCandidate[kMaxPathBuffer]{};
    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        if (!BuildModFilePath(gMods[i], "shop.lst", rootCandidate, sizeof(rootCandidate)))
            continue;
        const DWORD attributes = GetFileAttributesA(rootCandidate);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
    }

    char logicalTail[kMaxPathBuffer]{};
    if (!GetGameRelativeTail(gLogicalCwd, logicalTail, sizeof(logicalTail)))
        return false;
    static const char kCarDirectoryPrefix[] = "TRKDATA\\CARS\\";
    constexpr size_t kCarDirectoryPrefixLength = sizeof(kCarDirectoryPrefix) - 1;
    if (_strnicmp(logicalTail, kCarDirectoryPrefix, kCarDirectoryPrefixLength) != 0)
        return false;
    const char* carName = logicalTail + kCarDirectoryPrefixLength;
    if (!carName[0] || std::strchr(carName, '\\') != nullptr ||
        _stricmp(carName, ".") == 0 || _stricmp(carName, "..") == 0)
        return false;

    char proposedRelative[kMaxPathBuffer]{};
    if (_snprintf_s(proposedRelative, sizeof(proposedRelative), _TRUNCATE,
                    "%s\\shop.lst", logicalTail) < 0)
        return false;

    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        char candidate[kMaxPathBuffer]{};
        if (!BuildModFilePath(gMods[i], proposedRelative, candidate, sizeof(candidate)))
            continue;
        const DWORD attributes = GetFileAttributesA(candidate);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (!CopyString(relative, relativeSize, proposedRelative))
            return false;
        LogMessage("[SHOP-LIST-CWD-REMAP] original=%s logical=%s relative=%s mod=%s",
                   absolutePath, gLogicalCwd, proposedRelative, gMods[i].name);
        return true;
    }
    return false;
}

bool DirectoryExistsInGameOrMods(const char* absolutePath)
{
    if (!absolutePath)
        return false;
    DWORD attributes = GetFileAttributesA(absolutePath);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return true;

    char relative[kMaxPathBuffer]{};
    if (!GetGameRelativeTail(absolutePath, relative, sizeof(relative)) || !relative[0] ||
        IsModsRelativePath(relative))
        return false;
    char candidate[kMaxPathBuffer]{};
    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        if (!BuildModFilePath(gMods[i], relative, candidate, sizeof(candidate)))
            continue;
        attributes = GetFileAttributesA(candidate);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return true;
    }
    return false;
}

bool CompareMods(const ModEntry& a, const ModEntry& b)
{
    if (a.priority != b.priority)
        return a.priority < b.priority;
    return _stricmp(a.name, b.name) < 0;
}

void SetGamePaths()
{
    char exePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameA(GetModuleHandleA(nullptr), exePath, MAX_PATH);
    if (!n || n >= MAX_PATH)
        return;
    NormalizeSlashes(exePath);
    char* slash = std::strrchr(exePath, '\\');
    if (!slash)
        return;
    *slash = '\0';
    if (!CopyString(gGameDir, sizeof(gGameDir), exePath))
        return;
    CopyString(gLogicalCwd, sizeof(gLogicalCwd), gGameDir);
    if (_snprintf_s(gModsDir, sizeof(gModsDir), _TRUNCATE, "%s\\modloader", gGameDir) < 0 ||
        _snprintf_s(gProfilesDir, sizeof(gProfilesDir), _TRUNCATE, "%s\\.profiles", gModsDir) < 0 ||
        _snprintf_s(gIniPath, sizeof(gIniPath), _TRUNCATE, "%s\\modloader.ini", gGameDir) < 0 ||
        _snprintf_s(gLogPath, sizeof(gLogPath), _TRUNCATE, "%s\\CDModLoader.log", gGameDir) < 0)
        return;
    gRootReady = true;
}

void WriteLogLine(bool force, const char* format, va_list args)
{
    (void)force;
    if (!gRootReady || !gLogEnabled)
        return;
    char message[2048]{};
    int n = _vsnprintf_s(message, sizeof(message) - 3, _TRUNCATE, format, args);
    if (n < 0)
        n = static_cast<int>(std::strlen(message));
    message[n++] = '\r';
    message[n++] = '\n';
    message[n] = '\0';

    HANDLE file = gRealCreateFileA
        ? gRealCreateFileA(
            gLogPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        )
        : CreateFileA(
            gLogPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(file);
}

void ForceLog(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteLogLine(true, format, args);
    va_end(args);
}

void LogMessage(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    WriteLogLine(false, format, args);
    va_end(args);
}

bool IsSafeProfileName(const char* name)
{
    if (!name || !name[0] || _stricmp(name, ".") == 0 || _stricmp(name, "..") == 0)
        return false;
    const size_t length = std::strlen(name);
    if (length >= MAX_PATH || name[0] == '.' || name[length - 1] == '.' || name[length - 1] == ' ')
        return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    {
        if (*p < 32 || *p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' ||
            *p == '"' || *p == '<' || *p == '>' || *p == '|')
            return false;
    }
    return true;
}

bool NextCommandLineArgument(const char*& cursor, char* output, size_t outputSize)
{
    if (!cursor || !output || outputSize == 0)
        return false;
    while (*cursor == ' ' || *cursor == '\t')
        ++cursor;
    if (!*cursor)
        return false;

    size_t written = 0;
    bool inQuotes = false;
    bool overflow = false;
    auto append = [&](char ch)
    {
        if (written + 1 < outputSize)
            output[written++] = ch;
        else
            overflow = true;
    };

    while (*cursor)
    {
        if (!inQuotes && (*cursor == ' ' || *cursor == '\t'))
            break;

        size_t backslashes = 0;
        while (*cursor == '\\')
        {
            ++backslashes;
            ++cursor;
        }

        if (*cursor == '"')
        {
            for (size_t i = 0; i < backslashes / 2; ++i)
                append('\\');
            if ((backslashes & 1u) != 0)
            {
                append('"');
                ++cursor;
            }
            else
            {
                ++cursor;

                if (inQuotes && *cursor == '"')
                {
                    append('"');
                    ++cursor;
                }
                else
                {
                    inQuotes = !inQuotes;
                }
            }
            continue;
        }

        for (size_t i = 0; i < backslashes; ++i)
            append('\\');
        if (!*cursor || (!inQuotes && (*cursor == ' ' || *cursor == '\t')))
            break;
        append(*cursor++);
    }

    output[written] = '\0';
    return !overflow;
}

bool ReadCommandLineProfile(char* output, size_t outputSize, bool& optionSeen)
{
    optionSeen = false;
    if (!output || outputSize == 0)
        return false;
    output[0] = '\0';

    const char* cursor = GetCommandLineA();
    if (!cursor)
        return false;

    char argument[32768]{};
    while (NextCommandLineArgument(cursor, argument, sizeof(argument)))
    {
        if (_stricmp(argument, "-modprof") == 0)
        {
            optionSeen = true;
            char value[4096]{};
            if (!NextCommandLineArgument(cursor, value, sizeof(value)) || !value[0] || value[0] == '-')
                return false;
            if (!CopyString(output, outputSize, value))
                return false;
            return true;
        }
        if (_strnicmp(argument, "-modprof=", 9) == 0)
        {
            optionSeen = true;
            const char* value = argument + 9;
            if (!value[0] || value[0] == '-')
                return false;
            if (!CopyString(output, outputSize, value))
                return false;
            return true;
        }
    }
    return false;
}

void StripProfileIniSuffix(char* name)
{
    if (!name)
        return;
    const size_t length = std::strlen(name);
    if (length > 4 && _stricmp(name + length - 4, ".ini") == 0)
        name[length - 4] = '\0';
}

bool ConfigureActiveProfile()
{
    char requested[MAX_PATH]{};
    char commandLineProfile[MAX_PATH]{};
    bool commandLineOptionSeen = false;
    const bool hasCommandLineProfile =
        ReadCommandLineProfile(commandLineProfile, sizeof(commandLineProfile), commandLineOptionSeen);
    const char* profileSource = "modloader.ini";

    if (commandLineOptionSeen && hasCommandLineProfile)
    {
        StripProfileIniSuffix(commandLineProfile);
        if (IsSafeProfileName(commandLineProfile) &&
            CopyString(requested, sizeof(requested), commandLineProfile))
        {
            profileSource = "command-line";
        }
        else
        {
            ForceLog("[PROFILE-CLI-INVALID] value='%s'; ignoring -modprof and using modloader.ini",
                     commandLineProfile);
        }
    }
    else if (commandLineOptionSeen)
    {
        ForceLog("[PROFILE-CLI-MISSING-VALUE] expected -modprof <profile-name>; using modloader.ini");
    }

    if (!requested[0])
    {
        GetPrivateProfileStringA("ModLoader", "Profile", "", requested,
                                 static_cast<DWORD>(sizeof(requested)), gIniPath);
        if (!requested[0])
            GetPrivateProfileStringA("ModLoader", "SelectedProfile", "default", requested,
                                     static_cast<DWORD>(sizeof(requested)), gIniPath);
        if (!requested[0])
            CopyString(requested, sizeof(requested), "default");
    }

    StripProfileIniSuffix(requested);
    if (!IsSafeProfileName(requested))
    {
        ForceLog("[PROFILE-INVALID-NAME] configured='%s'; falling back to \"default\"", requested);
        CopyString(requested, sizeof(requested), "default");
        profileSource = "fallback";
    }
    CopyString(gSelectedProfile, sizeof(gSelectedProfile), requested);
    
    if (!CreateDirectoryA(gModsDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        LogMessage("[PROFILE-MODS-DIR-FAILED] path=%s error=%lu", gModsDir, GetLastError());
    if (!CreateDirectoryA(gProfilesDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        LogMessage("[PROFILE-DIR-FAILED] path=%s error=%lu", gProfilesDir, GetLastError());

    if (_snprintf_s(gProfilePath, sizeof(gProfilePath), _TRUNCATE, "%s\\%s.ini",
                    gProfilesDir, gSelectedProfile) < 0)
    {
        CopyString(gSelectedProfile, sizeof(gSelectedProfile), "default");
        if (_snprintf_s(gProfilePath, sizeof(gProfilePath), _TRUNCATE, "%s\\default.ini", gProfilesDir) < 0)
            return false;
    }
    if (_stricmp(gSelectedProfile, "default") != 0)
    {
        char saveRoot[MAX_PATH]{};
        char profileSaveDir[MAX_PATH]{};

        if (_snprintf_s(saveRoot, sizeof(saveRoot), _TRUNCATE,
            "%s\\.savegames", gModsDir) < 0 ||
            _snprintf_s(profileSaveDir, sizeof(profileSaveDir), _TRUNCATE,
                "%s\\%s", saveRoot, gSelectedProfile) < 0)
            return false;

        if (!CreateDirectoryA(saveRoot, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            LogMessage("[PROFILE-SAVE-ROOT-FAILED] path=%s error=%lu",
                saveRoot, GetLastError());

        if (!CreateDirectoryA(profileSaveDir, nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            LogMessage("[PROFILE-SAVE-DIR-FAILED] path=%s error=%lu",
                profileSaveDir, GetLastError());

        LogMessage("[PROFILE-SAVE-DIR] profile=%s path=%s",
            gSelectedProfile, profileSaveDir);
    }

    LogMessage("[PROFILE-ACTIVE] name=%s source=%s path=%s defaultNewModState=%s defaultNewModPriority=%d",
               gSelectedProfile, profileSource, gProfilePath,
               _stricmp(gSelectedProfile, "default") == 0 ? "true" : "false",
               1);
    return true;
}

bool WriteProfileState(const char* modName, bool enabled)
{
    const char* value = enabled ? "true" : "false";
    if (!WritePrivateProfileStringA("States", modName, value, gProfilePath))
    {
        LogMessage("[PROFILE-STATE-WRITE-FAILED] profile=%s mod=%s state=%s error=%lu",
                   gSelectedProfile, modName, value, GetLastError());
        return false;
    }
    return true;
}

bool WriteProfilePriority(const char* modName, int priority)
{
    char value[32]{};
    _snprintf_s(value, sizeof(value), _TRUNCATE, "%d", priority);
    if (!WritePrivateProfileStringA("Priorities", modName, value, gProfilePath))
    {
        LogMessage("[PROFILE-PRIORITY-WRITE-FAILED] profile=%s mod=%s priority=%d error=%lu",
                   gSelectedProfile, modName, priority, GetLastError());
        return false;
    }
    return true;
}

bool ParseNonNegativePriority(const char* value, int& priority)
{
    if (!value || !value[0])
        return false;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    while (end && *end && std::isspace(static_cast<unsigned char>(*end)))
        ++end;
    if (end == value || (end && *end) || parsed < 0)
        return false;
    priority = parsed > 0x7fffffffL ? 0x7fffffff : static_cast<int>(parsed);
    return true;
}

bool ParseProfileStateValue(const char* value, bool& enabled)
{
    if (!value)
        return false;
    if (_stricmp(value, "true") == 0 || _stricmp(value, "yes") == 0 ||
        _stricmp(value, "on") == 0 || std::strcmp(value, "1") == 0)
    {
        enabled = true;
        return true;
    }
    if (_stricmp(value, "false") == 0 || _stricmp(value, "no") == 0 ||
        _stricmp(value, "off") == 0 || std::strcmp(value, "0") == 0)
    {
        enabled = false;
        return true;
    }
    return false;
}

bool ResolveProfileState(const char* modName, bool& enabled, bool& wasAdded)
{
    static const char kMissing[] = "__CDML_PROFILE_STATE_MISSING_7A31__";
    char value[64]{};
    wasAdded = false;
    GetPrivateProfileStringA("States", modName, kMissing, value,
                             static_cast<DWORD>(sizeof(value)), gProfilePath);
    if (std::strcmp(value, kMissing) != 0)
    {
        if (ParseProfileStateValue(value, enabled))
            return true;

        enabled = _stricmp(gSelectedProfile, "default") == 0;
        LogMessage("[PROFILE-INVALID-STATE] profile=%s mod=%s value='%s'; using %s",
                   gSelectedProfile, modName, value, enabled ? "true" : "false");
        WriteProfileState(modName, enabled);
        wasAdded = true;
        return true;
    }

    int legacyPriority = 0;

    enabled = _stricmp(gSelectedProfile, "default") == 0;
    LogMessage("[PROFILE-STATE-ADDED] profile=%s mod=%s state=%s",
        gSelectedProfile, modName, enabled ? "true" : "false");

    WriteProfileState(modName, enabled);
    wasAdded = true;
    return true;
}

bool ResolveProfilePriority(const char* modName, int& priority, bool& wasAdded)
{
    static const char kMissing[] = "__CDML_PROFILE_PRIORITY_MISSING_7A31__";
    char value[64]{};
    wasAdded = false;
    GetPrivateProfileStringA("Priorities", modName, kMissing, value,
                             static_cast<DWORD>(sizeof(value)), gProfilePath);
    if (std::strcmp(value, kMissing) != 0)
    {
        if (ParseNonNegativePriority(value, priority))
            return true;
        LogMessage("[PROFILE-INVALID-PRIORITY] profile=%s mod=%s value='%s'; using 0",
                   gSelectedProfile, modName, value);
        priority = 0;
        WriteProfilePriority(modName, priority);
        return true;
    }

    priority = 50;

    WriteProfilePriority(modName, priority);
    wasAdded = true;
    return true;
}

bool EnsureDefaultModLoaderIni()
{
    if (!gRootReady || !gIniPath[0])
        return false;

    HANDLE file = CreateFileA(gIniPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
            return true;
        ForceLog("[CONFIG-CREATE-FAILED] path='%s' error=%lu", gIniPath, error);
        return false;
    }

    static const char kDefaultIni[] =
        "; === Crashday ModLoader by St1ngLeR ===\r\n"
        "; 1 - enabled, 0 - disabled.\r\n"
        "[ModLoader]\r\n"
        "Enabled=1\r\n"
        "Log=0\r\n"
        "LoadAsiPlugins=1\r\n"
        "; Active profile: modloader\\.profiles\\<Profile_Name>.ini\r\n"
        "; The command-line argument \"-modprof <Profile_Name>\" overrides this for this launch.\r\n"
        "Profile=default\r\n";

    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(sizeof(kDefaultIni) - 1);
    BOOL ok = WriteFile(file, kDefaultIni, bytes, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes)
    {
        ForceLog("[CONFIG-CREATE-FAILED] unable to write default configuration '%s'", gIniPath);
        return false;
    }

    ForceLog("[CONFIG-CREATED] path='%s'", gIniPath);
    return true;
}

void EnsureModsLoaded()
{
    LONG state = InterlockedCompareExchange(&gModsState, 1, 0);
    if (state == 0)
    {
        EnsureDefaultModLoaderIni();
        gEnabled = GetPrivateProfileIntA("ModLoader", "Enabled", 1, gIniPath) != 0;
        gLogEnabled = GetPrivateProfileIntA("ModLoader", "Log", 0, gIniPath) != 0;
        gLoadAsiPlugins = GetPrivateProfileIntA("ModLoader", "LoadAsiPlugins", 1, gIniPath) != 0;
        ConfigureActiveProfile();

        size_t discoveredModCount = 0;
        size_t profileAddedCount = 0;
        if (gEnabled)
        {
            char searchMask[MAX_PATH]{};
            if (_snprintf_s(searchMask, sizeof(searchMask), _TRUNCATE, "%s\\*", gModsDir) >= 0)
            {
                WIN32_FIND_DATAA data{};
                HANDLE search = ::FindFirstFileA(searchMask, &data);
                if (search != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                            continue;
                        if (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0)
                            continue;
                        if (data.cFileName[0] == '.' || _stricmp(data.cFileName, ".profiles") == 0)
                            continue;

                        ++discoveredModCount;
                        bool profileEnabled = false;
                        int priority = 0;
                        bool stateWasAdded = false;
                        bool priorityWasAdded = false;
                        ResolveProfileState(data.cFileName, profileEnabled, stateWasAdded);
                        ResolveProfilePriority(data.cFileName, priority, priorityWasAdded);
                        if (stateWasAdded || priorityWasAdded)
                        {
                            ++profileAddedCount;
                            LogMessage("[PROFILE-MOD-SETTINGS] profile=%s mod=%s state=%s priority=%d stateAdded=%d priorityAdded=%d",
                                       gSelectedProfile, data.cFileName, profileEnabled ? "true" : "false",
                                       priority, stateWasAdded ? 1 : 0, priorityWasAdded ? 1 : 0);
                        }

                        char modIni[MAX_PATH]{};
                        if (_snprintf_s(modIni, sizeof(modIni), _TRUNCATE, "%s\\%s\\mod.ini",
                                        gModsDir, data.cFileName) < 0)
                            continue;
                        if (GetPrivateProfileIntA("Mod", "Enabled", 1, modIni) == 0)
                        {
                            LogMessage("[MOD] disabled by mod.ini: %s", data.cFileName);
                            continue;
                        }
                        if (!profileEnabled)
                        {
                            LogMessage("[MOD] disabled by profile: %s (profile=%s state=false priority=%d)",
                                       data.cFileName, gSelectedProfile, priority);
                            continue;
                        }
                        ModEntry mod{};
                        if (!CopyString(mod.name, sizeof(mod.name), data.cFileName))
                            continue;
                        mod.priority = priority;
                        try
                        {
                            gMods.push_back(mod);
                            ++gModCount;
                        }
                        catch (...)
                        {
                            LogMessage("[MOD-ADD-FAILED] not enough memory to add mod: %s",
                                       data.cFileName);
                        }
                    } while (::FindNextFileA(search, &data));
                    ::FindClose(search);
                }
                else
                {
                    LogMessage("[PROFILE-SCAN-FAILED] modsDir=%s error=%lu", gModsDir, GetLastError());
                }
            }
            std::sort(gMods.begin(), gMods.end(), CompareMods);
        }

        InterlockedExchange(&gModsState, 2);
        if (gModsReadyEvent)
            SetEvent(gModsReadyEvent);
        LogMessage("Crashday ModLoader 1.0.0 initialised. GameDir=%s Enabled=%d OpenHook=%d LoadAsiPlugins=%d",
                   gGameDir, gEnabled ? 1 : 0, gOpenHookInstalled ? 1 : 0, gLoadAsiPlugins ? 1 : 0);
        LogMessage("Mods directory: %s", gModsDir);
        LogMessage("[PROFILE-SUMMARY] active=%s discovered=%u addedToProfile=%u activeMods=%u",
                   gSelectedProfile, static_cast<unsigned>(discoveredModCount),
                   static_cast<unsigned>(profileAddedCount), static_cast<unsigned>(gModCount));
        LogMessage("[PROFILE-STATE-RULE] States true/false controls enablement; Priorities controls ordering only.");
        LogMessage("[PROFILE-PRIORITY-RULE] zero is valid; higher numeric priority wins; ties use alphabetical order.");
        if (!gEnabled)
            LogMessage("ModLoader disabled by modloader.ini.");
        if (gEnabled && discoveredModCount == 0)
            LogMessage("No mod folders found; place each mod in its own subfolder under \"modloader\\\".");
        for (size_t i = 0; i < gModCount; ++i)
            LogMessage("[MOD] %s priority=%d", gMods[i].name, gMods[i].priority);
    }
    else
    {
        while (InterlockedCompareExchange(&gModsState, 2, 2) != 2)
            Sleep(0);
    }
}

bool IsAsiFileName(const char* filename)
{
    if (!filename)
        return false;
    const size_t length = std::strlen(filename);
    return length >= 4 && _stricmp(filename + length - 4, ".asi") == 0;
}

bool PathAlreadySeen(const std::vector<std::string>& paths, const char* path)
{
    for (const std::string& existing : paths)
    {
        if (_stricmp(existing.c_str(), path) == 0)
            return true;
    }
    return false;
}

void CollectAsiFilesInDirectory(const char* modName, const char* directory,
                                std::vector<std::string>& output)
{
    if (!directory || !directory[0])
        return;

    char mask[MAX_PATH]{};
    if (_snprintf_s(mask, sizeof(mask), _TRUNCATE, "%s\\*.asi", directory) < 0)
        return;

    WIN32_FIND_DATAA data{};
    HANDLE search = ::FindFirstFileA(mask, &data);
    if (search == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            ForceLog("[ASI-PLUGIN-SCAN-FAILED] mod=%s directory=%s error=%lu",
                     modName ? modName : "<unknown>", directory, GetLastError());
        return;
    }

    do
    {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !IsAsiFileName(data.cFileName))
            continue;

        char fullPath[MAX_PATH]{};
        if (_snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE, "%s\\%s", directory, data.cFileName) < 0)
        {
            LogMessage("[ASI-PLUGIN-SKIP-PATH-LONG] mod=%s directory=%s file=%s",
                       modName ? modName : "<unknown>", directory, data.cFileName);
            continue;
        }
        output.emplace_back(fullPath);
    } while (::FindNextFileA(search, &data));

    ::FindClose(search);
}

bool IsThisLoaderModulePath(const char* candidatePath)
{
    if (!gSelfModule || !candidatePath || !candidatePath[0])
        return false;
    char selfPath[MAX_PATH]{};
    DWORD length = GetModuleFileNameA(gSelfModule, selfPath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return false;
    NormalizeSlashes(selfPath);
    char candidate[MAX_PATH]{};
    if (!CopyString(candidate, sizeof(candidate), candidatePath))
        return false;
    NormalizeSlashes(candidate);
    return _stricmp(selfPath, candidate) == 0;
}

void LoadAsiPluginsForMod(const ModEntry& mod, std::vector<std::string>& attemptedPaths)
{
    if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return;

    char modDirectory[MAX_PATH]{};
    char modIni[MAX_PATH]{};
    if (_snprintf_s(modDirectory, sizeof(modDirectory), _TRUNCATE,
                    "%s\\%s", gModsDir, mod.name) < 0 ||
        _snprintf_s(modIni, sizeof(modIni), _TRUNCATE,
                    "%s\\%s\\mod.ini", gModsDir, mod.name) < 0)
    {
        LogMessage("[ASI-PLUGIN-SKIP-PATH-LONG] mod=%s", mod.name);
        return;
    }

    if (GetPrivateProfileIntA("Mod", "LoadAsiPlugins", 1, modIni) == 0)
    {
        LogMessage("[ASI-PLUGIN-MOD-DISABLED] mod=%s key=LoadAsiPlugins", mod.name);
        return;
    }

    std::vector<std::string> plugins;
    CollectAsiFilesInDirectory(mod.name, modDirectory, plugins);

    char pluginDirectory[MAX_PATH]{};
    if (_snprintf_s(pluginDirectory, sizeof(pluginDirectory), _TRUNCATE,
                    "%s\\plugins", modDirectory) >= 0)
        CollectAsiFilesInDirectory(mod.name, pluginDirectory, plugins);

    std::sort(plugins.begin(), plugins.end(), [](const std::string& left, const std::string& right)
    {
        return _stricmp(left.c_str(), right.c_str()) < 0;
    });

    LogMessage("[ASI-PLUGIN-SCAN] mod=%s candidates=%u (root and plugins\\)",
               mod.name, static_cast<unsigned>(plugins.size()));

    for (const std::string& pluginPath : plugins)
    {
        if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
            break;
        if (PathAlreadySeen(attemptedPaths, pluginPath.c_str()))
        {
            LogMessage("[ASI-PLUGIN-SKIP-DUPLICATE] mod=%s path=%s", mod.name, pluginPath.c_str());
            continue;
        }
        attemptedPaths.push_back(pluginPath);

        if (IsThisLoaderModulePath(pluginPath.c_str()))
        {
            LogMessage("[ASI-PLUGIN-SKIP-SELF] mod=%s path=%s", mod.name, pluginPath.c_str());
            continue;
        }

        const char* fileName = pluginPath.c_str();
        for (const char* cursor = pluginPath.c_str(); *cursor; ++cursor)
            if (*cursor == '\\' || *cursor == '/')
                fileName = cursor + 1;

        if (GetModuleHandleA(fileName) != nullptr)
        {
            LogMessage("[ASI-PLUGIN-ALREADY-LOADED] mod=%s path=%s basename=%s",
                       mod.name, pluginPath.c_str(), fileName);
            continue;
        }

        SetLastError(ERROR_SUCCESS);
        HMODULE pluginModule = gRealLoadLibraryA
            ? gRealLoadLibraryA(pluginPath.c_str())
            : ::LoadLibraryA(pluginPath.c_str());
        if (!pluginModule)
        {
            const DWORD error = GetLastError();
            ForceLog("[ASI-PLUGIN-FAILED] mod=%s path=%s error=%lu",
                     mod.name, pluginPath.c_str(), error);
            continue;
        }

        const DWORD patched = PatchModuleImports(pluginModule, false);
        LogMessage("[ASI-PLUGIN-LOADED] mod=%s path=%s handle=%p patchedImports=%lu",
                   mod.name, pluginPath.c_str(), pluginModule, patched);
    }
}

void LoadModAsiPlugins()
{
    EnsureModsLoaded();
    if (!gEnabled || !gLoadAsiPlugins || gModCount == 0)
    {
        LogMessage("[ASI-PLUGIN-SKIP] Enabled=%d LoadAsiPlugins=%d mods=%u",
                   gEnabled ? 1 : 0, gLoadAsiPlugins ? 1 : 0,
                   static_cast<unsigned>(gModCount));
        return;
    }

    std::vector<std::string> attemptedPaths;
    ForceLog("[ASI-PLUGIN-BEGIN] scanning enabled mod folders for .asi plugins");
    for (size_t i = 0; i < gModCount; ++i)
    {
        if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
            break;
        LoadAsiPluginsForMod(gMods[i], attemptedPaths);
    }
    ForceLog("[ASI-PLUGIN-END] attempted=%u", static_cast<unsigned>(attemptedPaths.size()));
}

struct CiHash
{
    size_t operator()(const std::string& value) const noexcept
    {
        size_t hash = static_cast<size_t>(2166136261u);
        for (unsigned char c : value)
        {
            hash ^= static_cast<unsigned char>(std::tolower(c));
            hash *= static_cast<size_t>(16777619u);
        }
        return hash;
    }
};

struct CiEqual
{
    bool operator()(const std::string& a, const std::string& b) const noexcept
    {
        return _stricmp(a.c_str(), b.c_str()) == 0;
    }
};

using EntryIndex = std::unordered_map<std::string, size_t, CiHash, CiEqual>;

bool IsModMetadataAtRoot(const char* name)
{
    if (!name || !name[0])
        return false;
    const char* extension = std::strrchr(name, '.');
    const bool isAsiPlugin = extension && _stricmp(extension, ".asi") == 0;
    return _stricmp(name, "plugins") == 0 ||
           isAsiPlugin;
}

bool AddOrReplaceEntry(SearchContext* context, EntryIndex& indices, const WIN32_FIND_DATAA& data,
                       bool modEntry, bool skipRootMetadata)
{
    if (modEntry && (std::strcmp(data.cFileName, ".") == 0 || std::strcmp(data.cFileName, "..") == 0))
        return false;
    if (modEntry && skipRootMetadata && IsModMetadataAtRoot(data.cFileName))
        return false;
    const std::string key(data.cFileName);
    auto found = indices.find(key);
    if (found == indices.end())
    {
        const size_t index = context->entries.size();
        context->entries.push_back(data);
        indices.emplace(key, index);
    }
    else if (modEntry)
    {
        context->entries[found->second] = data;
    }
    return true;
}

size_t CollectDirectory(const char* pattern, SearchContext* context, EntryIndex& indices,
                        bool modEntry, bool skipRootMetadata)
{
    WIN32_FIND_DATAA data{};
    HANDLE search = gRealFindFirstFileA(pattern, &data);
    if (search == INVALID_HANDLE_VALUE)
        return 0;
    size_t collected = 0;
    do
    {
        if (AddOrReplaceEntry(context, indices, data, modEntry, skipRootMetadata))
            ++collected;
    } while (gRealFindNextFileA(search, &data));
    gRealFindClose(search);
    return collected;
}

void AddSearchContext(SearchContext* context)
{
    EnterCriticalSection(&gSearchLock);
    context->next = gSearchContexts;
    gSearchContexts = context;
    LeaveCriticalSection(&gSearchLock);
}

SearchContext* FindSearchContextLocked(HANDLE token)
{
    for (SearchContext* current = gSearchContexts; current; current = current->next)
    {
        if (current->token == token)
            return current;
    }
    return nullptr;
}

SearchContext* RemoveSearchContext(HANDLE token)
{
    EnterCriticalSection(&gSearchLock);
    SearchContext** link = &gSearchContexts;
    while (*link)
    {
        if ((*link)->token == token)
        {
            SearchContext* removed = *link;
            *link = removed->next;
            removed->next = nullptr;
            LeaveCriticalSection(&gSearchLock);
            return removed;
        }
        link = &(*link)->next;
    }
    LeaveCriticalSection(&gSearchLock);
    return nullptr;
}

HANDLE WINAPI HookFindFirstFileA(LPCSTR fileName, LPWIN32_FIND_DATAA findData)
{
    if (!gRealFindFirstFileA || !gRealFindNextFileA || !gRealFindClose || !findData)
        return gRealFindFirstFileA ? gRealFindFirstFileA(fileName, findData) : INVALID_HANDLE_VALUE;

    if (!fileName)
        return gRealFindFirstFileA(fileName, findData);

    char* profileSavePath = ResolveProfileSavePath(fileName);
    if (profileSavePath)
    {
        HANDLE redirected = gRealFindFirstFileA(profileSavePath, findData);
        if (gLogEnabled)
            LogMessage("[PROFILE-SAVE-FIND] %s -> %s result=%d error=%lu",
                       fileName, profileSavePath,
                       redirected != INVALID_HANDLE_VALUE ? 1 : 0,
                       redirected != INVALID_HANDLE_VALUE ? 0UL : GetLastError());
        return redirected;
    }

    if (!ModsReady())
        return gRealFindFirstFileA(fileName, findData);

    if (!gEnabled || gModCount == 0)
        return gRealFindFirstFileA(fileName, findData);

    const char* lastComponent = fileName;
    for (const char* p = fileName; *p; ++p)
    {
        if (*p == '\\' || *p == '/')
            lastComponent = p + 1;
    }
    if (std::strpbrk(lastComponent, "*?") == nullptr)
    {
        char* candidate = ResolveReadOverride(fileName, "r");
        if (candidate && _stricmp(candidate, fileName) != 0)
        {
            HANDLE redirected = gRealFindFirstFileA(candidate, findData);
            if (redirected != INVALID_HANDLE_VALUE)
            {
                LogMessage("[FIND-FIRST-REDIRECT-OK] %s -> %s", fileName, candidate);
                return redirected;
            }
            const DWORD redirectError = GetLastError();
            LogMessage("[FIND-FIRST-REDIRECT-FAILED] %s -> %s error=%lu; falling back to original",
                       fileName, candidate, redirectError);
            SetLastError(redirectError);
        }
        return gRealFindFirstFileA(fileName, findData);
    }

    char relativePattern[kMaxPathBuffer]{};
    if (!MakeRelativeSearchPattern(fileName, relativePattern, sizeof(relativePattern)))
    {
        if (gLogEnabled)
        {
            char cwd[MAX_PATH]{};
            DWORD cwdLength = GetCurrentDirectoryA(MAX_PATH, cwd);
            if (cwdLength == 0 || cwdLength >= MAX_PATH)
                CopyString(cwd, sizeof(cwd), "<cwd-unavailable>");
            NormalizeSlashes(cwd);
            LogMessage("[DIR-MAP-FAIL] search=%s cwd=%s logical=%s rootReady=%d",
                       fileName, cwd, gLogicalCwd[0] ? gLogicalCwd : "<unset>",
                       gRootReady ? 1 : 0);
        }
        return gRealFindFirstFileA(fileName, findData);
    }

    const LONG directoryTrace = InterlockedIncrement(&gDirectoryMapTraceCount);
    if (gLogEnabled && directoryTrace <= 128)
    {
        char cwd[MAX_PATH]{};
        DWORD cwdLength = GetCurrentDirectoryA(MAX_PATH, cwd);
        if (cwdLength == 0 || cwdLength >= MAX_PATH)
            CopyString(cwd, sizeof(cwd), "<cwd-unavailable>");
        NormalizeSlashes(cwd);
        LogMessage("[DIR-MAP %03ld] search=%s cwd=%s -> %s", directoryTrace,
                   fileName, cwd, relativePattern);
    }

    SearchContext* context = new (std::nothrow) SearchContext();
    if (!context)
        return gRealFindFirstFileA(fileName, findData);

    size_t modEntryCount = 0;
    try
    {
        EntryIndex indices;
        CollectDirectory(fileName, context, indices, false, false);
        const bool rootDirectorySearch = std::strchr(relativePattern, '\\') == nullptr;

        for (size_t i = 0; i < gModCount; ++i)
        {
            char modRoot[MAX_PATH * 2]{};
            char modPattern[kMaxPathBuffer]{};
            if (_snprintf_s(modRoot, sizeof(modRoot), _TRUNCATE, "%s\\%s", gModsDir, gMods[i].name) < 0 ||
                _snprintf_s(modPattern, sizeof(modPattern), _TRUNCATE, "%s\\%s", modRoot, relativePattern) < 0)
                continue;
            modEntryCount += CollectDirectory(modPattern, context, indices, true, rootDirectorySearch);
        }
    }
    catch (...)
    {
        delete context;
        return gRealFindFirstFileA(fileName, findData);
    }

    if (context->entries.empty())
    {
        delete context;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }

    char tokenPattern[MAX_PATH * 2]{};
    if (_snprintf_s(tokenPattern, sizeof(tokenPattern), _TRUNCATE, "%s\\*", gModsDir) < 0)
    {
        delete context;
        return gRealFindFirstFileA(fileName, findData);
    }
    WIN32_FIND_DATAA tokenData{};
    context->token = gRealFindFirstFileA(tokenPattern, &tokenData);
    if (context->token == INVALID_HANDLE_VALUE)
    {
        delete context;
        return gRealFindFirstFileA(fileName, findData);
    }

    *findData = context->entries[0];
    context->nextIndex = 1;
    HANDLE token = context->token;
    AddSearchContext(context);
    SetLastError(ERROR_SUCCESS);
    if (modEntryCount > 0)
        LogMessage("[DIR] mod entries merged for %s (matching entries seen: %u)",
                   fileName, static_cast<unsigned>(modEntryCount));
    return token;
}

BOOL WINAPI HookFindNextFileA(HANDLE findHandle, LPWIN32_FIND_DATAA findData)
{
    if (!findData)
        return FALSE;

    EnterCriticalSection(&gSearchLock);
    SearchContext* context = FindSearchContextLocked(findHandle);
    if (context)
    {
        if (context->nextIndex < context->entries.size())
        {
            *findData = context->entries[context->nextIndex++];
            LeaveCriticalSection(&gSearchLock);
            SetLastError(ERROR_SUCCESS);
            return TRUE;
        }
        LeaveCriticalSection(&gSearchLock);
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    LeaveCriticalSection(&gSearchLock);

    return gRealFindNextFileA ? gRealFindNextFileA(findHandle, findData) : FALSE;
}

BOOL WINAPI HookFindClose(HANDLE findHandle)
{
    SearchContext* context = RemoveSearchContext(findHandle);
    if (context)
    {
        HANDLE token = context->token;
        delete context;
        return (token != INVALID_HANDLE_VALUE && gRealFindClose)
            ? gRealFindClose(token) : FALSE;
    }
    return gRealFindClose ? gRealFindClose(findHandle) : FALSE;
}


DWORD WINAPI HookGetFileAttributesA(LPCSTR fileName)
{
    if (!gRealGetFileAttributesA)
        return INVALID_FILE_ATTRIBUTES;

    char* profileSavePath = ResolveProfileSavePath(fileName);
    if (profileSavePath)
    {
        DWORD attrs = gRealGetFileAttributesA(profileSavePath);
        if (gLogEnabled)
            LogMessage("[PROFILE-SAVE-ATTR] %s -> %s attrs=0x%08lX",
                       fileName, profileSavePath, attrs);
        return attrs;
    }

    if (!ModsReady() || !gEnabled || gModCount == 0 || !fileName)
        return gRealGetFileAttributesA(fileName);

    char relative[kMaxPathBuffer]{};
    if (!MakeRelativeFilePath(fileName, relative, sizeof(relative)))
        return gRealGetFileAttributesA(fileName);

    if (std::strchr(relative, '\\') == nullptr && IsModMetadataAtRoot(relative))
        return gRealGetFileAttributesA(fileName);

    char candidate[kMaxPathBuffer]{};
    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        if (!BuildModFilePath(gMods[i], relative, candidate, sizeof(candidate)))
            continue;
        const DWORD attributes = gRealGetFileAttributesA(candidate);
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            const LONG traceIndex = InterlockedIncrement(&gAttributeTraceCount);
            if (gLogEnabled && traceIndex <= 256)
                LogMessage("[ATTR-REDIRECT] %s -> %s (mod=%s attrs=0x%08lX)",
                           fileName, candidate, gMods[i].name, attributes);
            return attributes;
        }
    }

    const LONG traceIndex = InterlockedIncrement(&gAttributeTraceCount);
    if (gLogEnabled && traceIndex <= 64)
        LogMessage("[ATTR-STOCK] %s (relative=%s)", fileName, relative);
    return gRealGetFileAttributesA(fileName);
}

BOOL WINAPI HookSetCurrentDirectoryA(LPCSTR path)
{
    if (!gRealSetCurrentDirectoryA)
        return FALSE;
    if (!path || !path[0])
        return gRealSetCurrentDirectoryA(path);

    char* profileSavePath = ResolveProfileSavePath(path);
    if (profileSavePath)
    {
        BOOL redirected = gRealSetCurrentDirectoryA(profileSavePath);
        DWORD error = redirected ? ERROR_SUCCESS : GetLastError();
        if (gLogEnabled)
            LogMessage("[PROFILE-SAVE-CWD] %s -> %s result=%d error=%lu",
                       path, profileSavePath, redirected ? 1 : 0, error);
        if (redirected)
            CopyString(gLogicalCwd, sizeof(gLogicalCwd), profileSavePath);
        SetLastError(error);
        return redirected;
    }

    char logicalTarget[kMaxPathBuffer]{};
    const bool targetResolved = BuildLogicalAbsolutePath(path, logicalTarget, sizeof(logicalTarget));

    if (!ModsReady())
    {
        const BOOL earlyResult = gRealSetCurrentDirectoryA(path);
        if (earlyResult && targetResolved && IsSameOrChildPath(logicalTarget, gGameDir))
            CopyString(gLogicalCwd, sizeof(gLogicalCwd), logicalTarget);
        return earlyResult;
    }

    BOOL result = FALSE;
    DWORD savedError = ERROR_SUCCESS;
    bool redirected = false;
    char selectedMod[MAX_PATH]{};
    char selectedPath[kMaxPathBuffer]{};

    if (targetResolved && IsSameOrChildPath(logicalTarget, gGameDir))
    {
        char relative[kMaxPathBuffer]{};
        const bool hasRelative = GetGameRelativeTail(logicalTarget, relative, sizeof(relative), true);
        const DWORD stockAttributes = gRealGetFileAttributesA
            ? gRealGetFileAttributesA(logicalTarget) : INVALID_FILE_ATTRIBUTES;

        if (stockAttributes != INVALID_FILE_ATTRIBUTES)
        {
            if (stockAttributes & FILE_ATTRIBUTE_DIRECTORY)
                result = gRealSetCurrentDirectoryA(logicalTarget);
            else
                result = gRealSetCurrentDirectoryA(logicalTarget);
        }
        else if (gEnabled && gModCount > 0 && hasRelative && relative[0] && !IsModsRelativePath(relative))
        {
            for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
            {
                char candidate[kMaxPathBuffer]{};
                if (!BuildModFilePath(gMods[i], relative, candidate, sizeof(candidate)))
                    continue;
                const DWORD attributes = gRealGetFileAttributesA(candidate);
                if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (gRealSetCurrentDirectoryA(candidate))
                {
                    result = TRUE;
                    savedError = ERROR_SUCCESS;
                    redirected = true;
                    CopyString(selectedMod, sizeof(selectedMod), gMods[i].name);
                    CopyString(selectedPath, sizeof(selectedPath), candidate);
                    break;
                }
            }
            if (!result)
                result = gRealSetCurrentDirectoryA(logicalTarget);
        }
        else
        {
            result = gRealSetCurrentDirectoryA(logicalTarget);
        }

        if (redirected)
            savedError = ERROR_SUCCESS;
        else
            savedError = GetLastError();
        if (result)
            CopyString(gLogicalCwd, sizeof(gLogicalCwd), logicalTarget);
    }
    else
    {
        if (targetResolved)
            result = gRealSetCurrentDirectoryA(logicalTarget);
        else
            result = gRealSetCurrentDirectoryA(path);
        savedError = GetLastError();
        if (result)
        {
            char actual[MAX_PATH]{};
            DWORD length = GetCurrentDirectoryA(MAX_PATH, actual);
            if (length && length < MAX_PATH)
            {
                NormalizeSlashes(actual);
                CopyString(gLogicalCwd, sizeof(gLogicalCwd), actual);
            }
        }
    }

    if (redirected)
    {
        LogMessage("[CWD-MOD-REDIRECT] requested=%s logical=%s physical=%s mod=%s",
                   path, logicalTarget, selectedPath, selectedMod);
    }
    else
    {
        const LONG trace = InterlockedIncrement(&gCwdTraceCount);
        if (gLogEnabled && trace <= 256)
        {
            char cwd[MAX_PATH]{};
            const DWORD n = GetCurrentDirectoryA(MAX_PATH, cwd);
            if (n == 0 || n >= MAX_PATH)
                CopyString(cwd, sizeof(cwd), "<cwd-unavailable>");
            NormalizeSlashes(cwd);
            LogMessage("[CWD %03ld] requested=%s result=%d error=%lu cwd=%s logical=%s", trace,
                       path, result ? 1 : 0, result ? 0UL : savedError,
                       cwd, gLogicalCwd[0] ? gLogicalCwd : "<unset>");
        }
    }

    SetLastError(savedError);
    return result;
}

bool IsReadOnlyCreateFileRequest(DWORD desiredAccess, DWORD creationDisposition)
{
    if (creationDisposition != OPEN_EXISTING || desiredAccess == 0)
        return false;
    const DWORD writeAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA |
                              FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | DELETE | WRITE_DAC | WRITE_OWNER;
    if ((desiredAccess & writeAccess) != 0)
        return false;
    const DWORD readAccess = GENERIC_READ | FILE_READ_DATA | FILE_READ_ATTRIBUTES | FILE_READ_EA;
    return (desiredAccess & readAccess) != 0;
}

HANDLE WINAPI HookCreateFileA(LPCSTR fileName, DWORD desiredAccess, DWORD shareMode,
                              LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
                              DWORD flagsAndAttributes, HANDLE templateFile)
{
    char* profileSavePath = ResolveProfileSavePath(fileName);
    if (profileSavePath && gRealCreateFileA)
    {
        HANDLE redirected = gRealCreateFileA(profileSavePath, desiredAccess, shareMode,
            securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
        const DWORD error = redirected == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        LogMessage(redirected == INVALID_HANDLE_VALUE
            ? "[PROFILE-SAVE-CREATEFILE-A-FAILED] %s -> %s access=0x%08lX disposition=%lu error=%lu"
            : "[PROFILE-SAVE-CREATEFILE-A-OK] %s -> %s access=0x%08lX disposition=%lu",
            fileName ? fileName : "<null>", profileSavePath, desiredAccess,
            creationDisposition, error);
        return redirected;
    }

    const int memoryCategoryIndex = GetEditorCategoryMemoryIndexFromPath(fileName);
    if (memoryCategoryIndex >= 0)
    {
        if (!IsReadOnlyCreateFileRequest(desiredAccess, creationDisposition))
        {
            SetLastError(ERROR_ACCESS_DENIED);
            LogMessage("[EDITOR-CAT-MEMORY-OPEN-FAILED] non-read-only request path=%s access=0x%08lX disposition=%lu",
                       fileName ? fileName : "<null>", desiredAccess, creationDisposition);
            return INVALID_HANDLE_VALUE;
        }
        return OpenEditorCategoryMemoryPipe(memoryCategoryIndex);
    }
    if (!gRealCreateFileA)
        return INVALID_HANDLE_VALUE;
    if (!fileName || !gEnabled ||
        !IsReadOnlyCreateFileRequest(desiredAccess, creationDisposition) ||
        InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return gRealCreateFileA(fileName, desiredAccess, shareMode, securityAttributes,
                                creationDisposition, flagsAndAttributes, templateFile);
    if (!ModsReady() || gModCount == 0)
        return gRealCreateFileA(fileName, desiredAccess, shareMode, securityAttributes,
                                creationDisposition, flagsAndAttributes, templateFile);

    const LONG trace = InterlockedIncrement(&gCreateFileTraceCount);
    if (gLogEnabled && trace <= 256)
        LogMessage("[CREATEFILE-A-CHECK %03ld] file=%s access=0x%08lX disposition=%lu",
                   trace, fileName, desiredAccess, creationDisposition);

    char* candidate = ResolveReadOverride(fileName, "r");
    if (candidate && _stricmp(candidate, fileName) != 0)
    {
        HANDLE opened = gRealCreateFileA(candidate, desiredAccess, shareMode, securityAttributes,
                                          creationDisposition, flagsAndAttributes, templateFile);
        if (opened != INVALID_HANDLE_VALUE)
        {
            LogMessage("[CREATEFILE-A-REDIRECT-OK] %s -> %s", fileName, candidate);
            return opened;
        }
        const DWORD openError = GetLastError();
        LogMessage("[CREATEFILE-A-REDIRECT-FAILED] %s -> %s error=%lu; falling back to original",
                   fileName, candidate, openError);
    }
    else if (gLogEnabled)
    {
        LogMessage("[CREATEFILE-A-ORIGINAL] %s (no different overlay candidate)", fileName);
    }

    return gRealCreateFileA(fileName, desiredAccess, shareMode, securityAttributes,
                            creationDisposition, flagsAndAttributes, templateFile);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR fileName, DWORD desiredAccess, DWORD shareMode,
                              LPSECURITY_ATTRIBUTES securityAttributes, DWORD creationDisposition,
                              DWORD flagsAndAttributes, HANDLE templateFile)
{
    if (fileName && gRealCreateFileW)
    {
        char inputAnsi[kMaxPathBuffer]{};
        if (WideCharToMultiByte(CP_ACP, 0, fileName, -1, inputAnsi,
                                static_cast<int>(sizeof(inputAnsi)), nullptr, nullptr) > 0)
        {
            char* profileSavePath = ResolveProfileSavePath(inputAnsi);
            if (profileSavePath)
            {
                wchar_t wideTarget[kMaxPathBuffer]{};
                if (MultiByteToWideChar(CP_ACP, 0, profileSavePath, -1, wideTarget,
                                        static_cast<int>(sizeof(wideTarget) / sizeof(wideTarget[0]))) > 0)
                {
                    HANDLE redirected = gRealCreateFileW(wideTarget, desiredAccess, shareMode,
                        securityAttributes, creationDisposition, flagsAndAttributes, templateFile);
                    const DWORD error = redirected == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                    LogMessage(redirected == INVALID_HANDLE_VALUE
                        ? "[PROFILE-SAVE-CREATEFILE-W-FAILED] %s -> %s error=%lu"
                        : "[PROFILE-SAVE-CREATEFILE-W-OK] %s -> %s",
                        inputAnsi, profileSavePath, error);
                    return redirected;
                }
            }
        }
    }
    if (fileName)
    {
        char virtualPath[kMaxPathBuffer]{};
        if (WideCharToMultiByte(CP_ACP, 0, fileName, -1, virtualPath,
                                static_cast<int>(sizeof(virtualPath)), nullptr, nullptr) > 0)
        {
            const int memoryCategoryIndex = GetEditorCategoryMemoryIndexFromPath(virtualPath);
            if (memoryCategoryIndex >= 0)
            {
                if (!IsReadOnlyCreateFileRequest(desiredAccess, creationDisposition))
                {
                    SetLastError(ERROR_ACCESS_DENIED);
                    LogMessage("[EDITOR-CAT-MEMORY-OPEN-FAILED] non-read-only wide request path=%s access=0x%08lX disposition=%lu",
                               virtualPath, desiredAccess, creationDisposition);
                    return INVALID_HANDLE_VALUE;
                }
                return OpenEditorCategoryMemoryPipe(memoryCategoryIndex);
            }
        }
    }
    if (!gRealCreateFileW)
        return INVALID_HANDLE_VALUE;
    if (!fileName || !gEnabled ||
        !IsReadOnlyCreateFileRequest(desiredAccess, creationDisposition) ||
        InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return gRealCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                creationDisposition, flagsAndAttributes, templateFile);
    if (!ModsReady() || gModCount == 0)
        return gRealCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                creationDisposition, flagsAndAttributes, templateFile);

    char fileAnsi[kMaxPathBuffer]{};
    if (WideCharToMultiByte(CP_ACP, 0, fileName, -1, fileAnsi, static_cast<int>(sizeof(fileAnsi)), nullptr, nullptr) <= 0)
        return gRealCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                                creationDisposition, flagsAndAttributes, templateFile);

    const LONG trace = InterlockedIncrement(&gCreateFileTraceCount);
    if (gLogEnabled && trace <= 256)
        LogMessage("[CREATEFILE-W-CHECK %03ld] file=%s access=0x%08lX disposition=%lu",
                   trace, fileAnsi, desiredAccess, creationDisposition);

    char* candidate = ResolveReadOverride(fileAnsi, "r");
    if (candidate && _stricmp(candidate, fileAnsi) != 0)
    {
        wchar_t candidateWide[kMaxPathBuffer]{};
        if (MultiByteToWideChar(CP_ACP, 0, candidate, -1, candidateWide,
                                static_cast<int>(sizeof(candidateWide) / sizeof(candidateWide[0]))) > 0)
        {
            HANDLE opened = gRealCreateFileW(candidateWide, desiredAccess, shareMode, securityAttributes,
                                              creationDisposition, flagsAndAttributes, templateFile);
            if (opened != INVALID_HANDLE_VALUE)
            {
                LogMessage("[CREATEFILE-W-REDIRECT-OK] %s -> %s", fileAnsi, candidate);
                return opened;
            }
            const DWORD openError = GetLastError();
            LogMessage("[CREATEFILE-W-REDIRECT-FAILED] %s -> %s error=%lu; falling back to original",
                       fileAnsi, candidate, openError);
        }
    }
    else if (gLogEnabled)
    {
        LogMessage("[CREATEFILE-W-ORIGINAL] %s (no different overlay candidate)", fileAnsi);
    }

    return gRealCreateFileW(fileName, desiredAccess, shareMode, securityAttributes,
                            creationDisposition, flagsAndAttributes, templateFile);
}

FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR procName)
{
    if (!gRealGetProcAddress)
        return nullptr;

    FARPROC resolved = gRealGetProcAddress(module, procName);
    if (!resolved || !procName || reinterpret_cast<ULONG_PTR>(procName) <= 0xFFFFu ||
        InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return resolved;

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    HMODULE kernelbase = GetModuleHandleA("kernelbase.dll");
    const bool coreApiModule = module == kernel32 || module == kernelbase;
    FARPROC replacement = nullptr;
    const char* hookName = nullptr;

    if (_stricmp(procName, "CreateFileA") == 0 &&
        (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealCreateFileA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookCreateFileA);
        hookName = "HookCreateFileA";
    }
    else if (_stricmp(procName, "CreateFileW") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealCreateFileW)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookCreateFileW);
        hookName = "HookCreateFileW";
    }
    else if (_stricmp(procName, "GetFileAttributesA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealGetFileAttributesA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookGetFileAttributesA);
        hookName = "HookGetFileAttributesA";
    }
    else if (_stricmp(procName, "SetCurrentDirectoryA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealSetCurrentDirectoryA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookSetCurrentDirectoryA);
        hookName = "HookSetCurrentDirectoryA";
    }
    else if (_stricmp(procName, "FindFirstFileA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealFindFirstFileA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookFindFirstFileA);
        hookName = "HookFindFirstFileA";
    }
    else if (_stricmp(procName, "FindNextFileA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealFindNextFileA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookFindNextFileA);
        hookName = "HookFindNextFileA";
    }
    else if (_stricmp(procName, "FindClose") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealFindClose)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookFindClose);
        hookName = "HookFindClose";
    }
    else if (_stricmp(procName, "LoadLibraryA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealLoadLibraryA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookLoadLibraryA);
        hookName = "HookLoadLibraryA";
    }
    else if (_stricmp(procName, "LoadLibraryW") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealLoadLibraryW)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookLoadLibraryW);
        hookName = "HookLoadLibraryW";
    }
    else if (_stricmp(procName, "LoadLibraryExA") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealLoadLibraryExA)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookLoadLibraryExA);
        hookName = "HookLoadLibraryExA";
    }
    else if (_stricmp(procName, "LoadLibraryExW") == 0 &&
             (coreApiModule || resolved == reinterpret_cast<FARPROC>(gRealLoadLibraryExW)))
    {
        replacement = reinterpret_cast<FARPROC>(&HookLoadLibraryExW);
        hookName = "HookLoadLibraryExW";
    }

    if (replacement)
    {
        const LONG trace = InterlockedIncrement(&gGetProcRedirectTraceCount);
        if (gLogEnabled && trace <= 128)
            ForceLog("[GETPROC-REDIRECT %03ld] %s module=%p -> %s",
                     trace, procName, module, hookName ? hookName : "<hook>");
        return replacement;
    }
    return resolved;
}

bool InstallGetProcAddressHook()
{
    if (!gRealGetProcAddress || gGetProcAddressHookInstalled)
        return gGetProcAddressHookInstalled;

    BYTE* target = reinterpret_cast<BYTE*>(gRealGetProcAddress);
    const BYTE expected[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    if (!target || std::memcmp(target, expected, sizeof(expected)) != 0)
    {
        const BYTE* b = target;
        ForceLog("[GETPROC-HOOK-FAILED] unsupported GetProcAddress entry bytes=%02X %02X %02X %02X %02X; using IAT-only hooks",
                 b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0,
                 b ? b[3] : 0, b ? b[4] : 0);
        return false;
    }

    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(
        nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline)
    {
        ForceLog("[GETPROC-HOOK-FAILED] trampoline allocation error=%lu", GetLastError());
        return false;
    }

    std::memcpy(trampoline, target, sizeof(expected));
    trampoline[5] = 0xE9;
    const int32_t backDisplacement = static_cast<int32_t>((target + sizeof(expected)) - (trampoline + 10));
    std::memcpy(trampoline + 6, &backDisplacement, sizeof(backDisplacement));

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(expected), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        ForceLog("[GETPROC-HOOK-FAILED] VirtualProtect error=%lu", GetLastError());
        return false;
    }

    std::memcpy(gSavedGetProcAddressBytes, target, sizeof(gSavedGetProcAddressBytes));
    gGetProcAddressEntry = reinterpret_cast<FARPROC>(target);
    gGetProcAddressTarget = target;
    gGetProcAddressTrampoline = trampoline;
    gRealGetProcAddress = reinterpret_cast<GetProcAddressFn>(trampoline);

    target[0] = 0xE9;
    const int32_t hookDisplacement = static_cast<int32_t>(
        reinterpret_cast<BYTE*>(&HookGetProcAddress) - (target + 5));
    std::memcpy(target + 1, &hookDisplacement, sizeof(hookDisplacement));
    FlushInstructionCache(GetCurrentProcess(), trampoline, 10);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(expected));
    DWORD ignored = 0;
    VirtualProtect(target, sizeof(expected), oldProtect, &ignored);

    gGetProcAddressHookInstalled = true;
    ForceLog("[GETPROC-HOOK] installed target=%p trampoline=%p; dynamic file API lookups will use ModLoader hooks",
             target, trampoline);
    return true;
}

void RestoreGetProcAddressHook()
{
    if (!gGetProcAddressHookInstalled || !gGetProcAddressTarget)
        return;

    DWORD oldProtect = 0;
    if (VirtualProtect(gGetProcAddressTarget, sizeof(gSavedGetProcAddressBytes),
                       PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        std::memcpy(gGetProcAddressTarget, gSavedGetProcAddressBytes,
                    sizeof(gSavedGetProcAddressBytes));
        FlushInstructionCache(GetCurrentProcess(), gGetProcAddressTarget,
                              sizeof(gSavedGetProcAddressBytes));
        DWORD ignored = 0;
        VirtualProtect(gGetProcAddressTarget, sizeof(gSavedGetProcAddressBytes), oldProtect, &ignored);
        gRealGetProcAddress = reinterpret_cast<GetProcAddressFn>(gGetProcAddressEntry);
        if (gGetProcAddressTrampoline)
            VirtualFree(gGetProcAddressTrampoline, 0, MEM_RELEASE);
        gGetProcAddressTrampoline = nullptr;
        gGetProcAddressTarget = nullptr;
        gGetProcAddressEntry = nullptr;
        gGetProcAddressHookInstalled = false;
    }
    else
    {
        ForceLog("[GETPROC-HOOK-RESTORE-FAILED] VirtualProtect error=%lu", GetLastError());
    }
}

HMODULE WINAPI HookLoadLibraryA(LPCSTR fileName)
{
    HMODULE module = gRealLoadLibraryA ? gRealLoadLibraryA(fileName) : nullptr;
    if (module && InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
    {
        const DWORD patched = PatchModuleImports(module, false);
        if (patched && gLogEnabled)
            ForceLog("[IAT-LATE-MODULE] LoadLibraryA %s: patched %lu imports", fileName ? fileName : "<null>", patched);
    }
    return module;
}

HMODULE WINAPI HookLoadLibraryW(LPCWSTR fileName)
{
    HMODULE module = gRealLoadLibraryW ? gRealLoadLibraryW(fileName) : nullptr;
    if (module && InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
    {
        const DWORD patched = PatchModuleImports(module, false);
        if (patched && gLogEnabled)
            ForceLog("[IAT-LATE-MODULE] LoadLibraryW: patched %lu imports", patched);
    }
    return module;
}

HMODULE WINAPI HookLoadLibraryExA(LPCSTR fileName, HANDLE file, DWORD flags)
{
    HMODULE module = gRealLoadLibraryExA ? gRealLoadLibraryExA(fileName, file, flags) : nullptr;
    if (module && InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
    {
        const DWORD patched = PatchModuleImports(module, false);
        if (patched && gLogEnabled)
            ForceLog("[IAT-LATE-MODULE] LoadLibraryExA %s: patched %lu imports", fileName ? fileName : "<null>", patched);
    }
    return module;
}

HMODULE WINAPI HookLoadLibraryExW(LPCWSTR fileName, HANDLE file, DWORD flags)
{
    HMODULE module = gRealLoadLibraryExW ? gRealLoadLibraryExW(fileName, file, flags) : nullptr;
    if (module && InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
    {
        const DWORD patched = PatchModuleImports(module, false);
        if (patched && gLogEnabled)
            ForceLog("[IAT-LATE-MODULE] LoadLibraryExW: patched %lu imports", patched);
    }
    return module;
}

bool InstallGameOpenHook(HMODULE exeModule)
{
    if (!exeModule)
        return false;

    auto* base = reinterpret_cast<BYTE*>(exeModule);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
    {
        ForceLog("[ERROR] Main module has an invalid DOS header; no code patch applied.");
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.SizeOfImage < kOpenFileRva + kPatchLength + 2)
    {
        ForceLog("[ERROR] Main module is not the expected PE32/x86 image or is too small; no code patch applied.");
        return false;
    }

    gOpenTarget = base + kOpenFileRva;
    if (std::memcmp(gOpenTarget, kExpectedOpenPrologue, sizeof(kExpectedOpenPrologue)) != 0 ||
        gOpenTarget[kPatchLength] != 0x5B || gOpenTarget[kPatchLength + 1] != 0xC3)
    {
        ForceLog("[ERROR] Game file-open signature/epilogue mismatch at RVA 0x%08X; no code patch applied.", kOpenFileRva);
        return false;
    }
    if (nt->OptionalHeader.SizeOfImage < kOpenCallSiteRva + sizeof(kExpectedOpenCallerSetup) ||
        std::memcmp(base + kOpenCallSiteRva, kExpectedOpenCallerSetup, sizeof(kExpectedOpenCallerSetup)) != 0)
    {
        ForceLog("[ERROR] File-open caller ABI check failed at VA 0x%08X; refusing to hook an unverified build.", kOpenCallSiteVa);
        return false;
    }

    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline)
    {
        ForceLog("[ERROR] VirtualAlloc for game file-open trampoline failed (%lu).", GetLastError());
        return false;
    }

    std::memcpy(gSavedOpenBytes, gOpenTarget, kPatchLength);
    std::memcpy(trampoline, gOpenTarget, kPatchLength);

    int32_t oldCallDisplacement = 0;
    std::memcpy(&oldCallDisplacement, gOpenTarget + 4, sizeof(oldCallDisplacement));
    BYTE* callDestination = gOpenTarget + 8 + oldCallDisplacement;
    int32_t newCallDisplacement = static_cast<int32_t>(callDestination - (trampoline + 8));
    std::memcpy(trampoline + 4, &newCallDisplacement, sizeof(newCallDisplacement));

    trampoline[8] = 0xE9;
    const int32_t returnDisplacement = static_cast<int32_t>((gOpenTarget + kPatchLength) - (trampoline + 13));
    std::memcpy(trampoline + 9, &returnDisplacement, sizeof(returnDisplacement));
    gOriginalOpen = trampoline;
    gOpenTrampoline = trampoline;

    DWORD oldProtect = 0;
    if (!VirtualProtect(gOpenTarget, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        gOriginalOpen = nullptr;
        gOpenTrampoline = nullptr;
        ForceLog("[ERROR] VirtualProtect for game file-open hook failed (%lu).", GetLastError());
        return false;
    }

    gOpenTarget[0] = 0xE9;
    const int32_t hookDisplacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(&GameOpenHook) - (gOpenTarget + 5));
    std::memcpy(gOpenTarget + 1, &hookDisplacement, sizeof(hookDisplacement));
    for (size_t i = 5; i < kPatchLength; ++i)
        gOpenTarget[i] = 0x90;

    FlushInstructionCache(GetCurrentProcess(), trampoline, 13);
    FlushInstructionCache(GetCurrentProcess(), gOpenTarget, kPatchLength);
    DWORD ignored = 0;
    VirtualProtect(gOpenTarget, kPatchLength, oldProtect, &ignored);
    gOpenHookInstalled = true;
    return true;
}

void RestoreGameOpenHook()
{
    if (!gOpenHookInstalled || !gOpenTarget)
        return;
    DWORD oldProtect = 0;
    if (VirtualProtect(gOpenTarget, kPatchLength, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        std::memcpy(gOpenTarget, gSavedOpenBytes, kPatchLength);
        FlushInstructionCache(GetCurrentProcess(), gOpenTarget, kPatchLength);
        DWORD ignored = 0;
        VirtualProtect(gOpenTarget, kPatchLength, oldProtect, &ignored);
    }
    gOpenHookInstalled = false;
    if (gOpenTrampoline)
    {
        VirtualFree(gOpenTrampoline, 0, MEM_RELEASE);
        gOpenTrampoline = nullptr;
        gOriginalOpen = nullptr;
    }
}

bool PatchIatSlot(DWORD* slot, DWORD replacement)
{
    if (!slot || !replacement)
        return false;
    if (gIatLockInitialized)
        EnterCriticalSection(&gIatLock);

    bool patched = false;
    if (*slot != replacement && gIatPatchCount < kMaxIatPatches &&
        InterlockedCompareExchange(&gShuttingDown, 0, 0) == 0)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &oldProtect))
        {
            const DWORD oldValue = *slot;
            *slot = replacement;
            gIatPatches[gIatPatchCount].slot = slot;
            gIatPatches[gIatPatchCount].original = oldValue;
            ++gIatPatchCount;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(DWORD), oldProtect, &ignored);
            patched = true;
        }
    }
    if (gIatLockInitialized)
        LeaveCriticalSection(&gIatLock);
    return patched;
}

bool ShouldSkipImportPatchModule(HMODULE module)
{
    if (!module || module == gSelfModule)
        return true;

    return module == GetModuleHandleA("kernel32.dll") ||
           module == GetModuleHandleA("kernelbase.dll") ||
           module == GetModuleHandleA("ntdll.dll");
}

DWORD PatchModuleImports(HMODULE module, bool patchDirectoryApis)
{
    if (ShouldSkipImportPatchModule(module) ||
        InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return 0;

    auto* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
        return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 0;

    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || !imports.Size)
        return 0;

    DWORD patched = 0;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk)
            : nullptr;

        for (size_t index = 0; firstThunk[index].u1.Function != 0; ++index)
        {
            const char* importName = nullptr;
            if (names)
            {
                if (IMAGE_SNAP_BY_ORDINAL32(names[index].u1.Ordinal))
                    continue;
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[index].u1.AddressOfData);
                importName = reinterpret_cast<const char*>(byName->Name);
            }
            else
            {
                const DWORD current = firstThunk[index].u1.Function;
                if (current == reinterpret_cast<DWORD>(gRealFindFirstFileA)) importName = "FindFirstFileA";
                else if (current == reinterpret_cast<DWORD>(gRealFindNextFileA)) importName = "FindNextFileA";
                else if (current == reinterpret_cast<DWORD>(gRealFindClose)) importName = "FindClose";
                else if (current == reinterpret_cast<DWORD>(gRealGetFileAttributesA)) importName = "GetFileAttributesA";
                else if (current == reinterpret_cast<DWORD>(gRealSetCurrentDirectoryA)) importName = "SetCurrentDirectoryA";
                else if (current == reinterpret_cast<DWORD>(gRealCreateFileA)) importName = "CreateFileA";
                else if (current == reinterpret_cast<DWORD>(gRealCreateFileW)) importName = "CreateFileW";
                else if (current == reinterpret_cast<DWORD>(gRealLoadLibraryA)) importName = "LoadLibraryA";
                else if (current == reinterpret_cast<DWORD>(gRealLoadLibraryW)) importName = "LoadLibraryW";
                else if (current == reinterpret_cast<DWORD>(gRealLoadLibraryExA)) importName = "LoadLibraryExA";
                else if (current == reinterpret_cast<DWORD>(gRealLoadLibraryExW)) importName = "LoadLibraryExW";
            }
            if (!importName)
                continue;

            DWORD replacement = 0;
            if (std::strcmp(importName, "CreateFileA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookCreateFileA);
            else if (std::strcmp(importName, "CreateFileW") == 0)
                replacement = reinterpret_cast<DWORD>(&HookCreateFileW);
            else if (std::strcmp(importName, "LoadLibraryA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookLoadLibraryA);
            else if (std::strcmp(importName, "LoadLibraryW") == 0)
                replacement = reinterpret_cast<DWORD>(&HookLoadLibraryW);
            else if (std::strcmp(importName, "LoadLibraryExA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookLoadLibraryExA);
            else if (std::strcmp(importName, "LoadLibraryExW") == 0)
                replacement = reinterpret_cast<DWORD>(&HookLoadLibraryExW);
            else if (patchDirectoryApis && std::strcmp(importName, "FindFirstFileA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookFindFirstFileA);
            else if (patchDirectoryApis && std::strcmp(importName, "FindNextFileA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookFindNextFileA);
            else if (patchDirectoryApis && std::strcmp(importName, "FindClose") == 0)
                replacement = reinterpret_cast<DWORD>(&HookFindClose);
            else if (patchDirectoryApis && std::strcmp(importName, "GetFileAttributesA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookGetFileAttributesA);
            else if (patchDirectoryApis && std::strcmp(importName, "SetCurrentDirectoryA") == 0)
                replacement = reinterpret_cast<DWORD>(&HookSetCurrentDirectoryA);

            if (replacement && PatchIatSlot(reinterpret_cast<DWORD*>(&firstThunk[index].u1.Function), replacement))
            {
                ++patched;
                if (gLogEnabled)
                    ForceLog("[IAT] patched %s in module=%p", importName, module);
            }
        }
    }
    return patched;
}

DWORD PatchGameImports(HMODULE exeModule)
{
    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    if (!kernel32)
        return 0;
    gRealFindFirstFileA = reinterpret_cast<FindFirstFileAFn>(GetProcAddress(kernel32, "FindFirstFileA"));
    gRealFindNextFileA = reinterpret_cast<FindNextFileAFn>(GetProcAddress(kernel32, "FindNextFileA"));
    gRealFindClose = reinterpret_cast<FindCloseFn>(GetProcAddress(kernel32, "FindClose"));
    gRealGetFileAttributesA = reinterpret_cast<GetFileAttributesAFn>(GetProcAddress(kernel32, "GetFileAttributesA"));
    gRealSetCurrentDirectoryA = reinterpret_cast<SetCurrentDirectoryAFn>(GetProcAddress(kernel32, "SetCurrentDirectoryA"));
    gRealCreateFileA = reinterpret_cast<CreateFileAFn>(GetProcAddress(kernel32, "CreateFileA"));
    gRealCreateFileW = reinterpret_cast<CreateFileWFn>(GetProcAddress(kernel32, "CreateFileW"));
    gRealLoadLibraryA = reinterpret_cast<LoadLibraryAFn>(GetProcAddress(kernel32, "LoadLibraryA"));
    gRealLoadLibraryW = reinterpret_cast<LoadLibraryWFn>(GetProcAddress(kernel32, "LoadLibraryW"));
    gRealLoadLibraryExA = reinterpret_cast<LoadLibraryExAFn>(GetProcAddress(kernel32, "LoadLibraryExA"));
    gRealLoadLibraryExW = reinterpret_cast<LoadLibraryExWFn>(GetProcAddress(kernel32, "LoadLibraryExW"));
    gRealGetProcAddress = reinterpret_cast<GetProcAddressFn>(GetProcAddress(kernel32, "GetProcAddress"));
    if (!gRealFindFirstFileA || !gRealFindNextFileA || !gRealFindClose || !gRealGetFileAttributesA ||
        !gRealSetCurrentDirectoryA || !gRealCreateFileA || !gRealCreateFileW || !gRealLoadLibraryA ||
        !gRealLoadLibraryW || !gRealLoadLibraryExA || !gRealLoadLibraryExW || !gRealGetProcAddress)
        return 0;
    return PatchModuleImports(exeModule, true);
}

DWORD WINAPI PatchLoadedModulesThread(LPVOID)
{
    if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
        return 0;

    EnsureModsLoaded();

    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH)
            break;
    }
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        ForceLog("[IAT-EXPAND-FAILED] CreateToolhelp32Snapshot error=%lu; continuing with ASI plugin loading",
                 GetLastError());
        LoadModAsiPlugins();
        if (gAsiStartupEvent) SetEvent(gAsiStartupEvent);
        return 0;
    }

    DWORD scanned = 0;
    DWORD patched = 0;
    MODULEENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Module32First(snapshot, &entry))
    {
        do
        {
            if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
                break;
            ++scanned;
            const DWORD modulePatched = PatchModuleImports(entry.hModule, entry.hModule == GetModuleHandleA(nullptr));
            patched += modulePatched;
            if (modulePatched && gLogEnabled)
                ForceLog("[IAT-MODULE] %s: patched %lu imports", entry.szModule, modulePatched);
        } while (Module32Next(snapshot, &entry));
    }
    else
    {
        ForceLog("[IAT-EXPAND-FAILED] Module32First error=%lu", GetLastError());
    }
    CloseHandle(snapshot);
    ForceLog("[IAT-EXPAND] loaded modules scanned=%lu additional imports patched=%lu", scanned, patched);

    LoadModAsiPlugins();

    if (gAsiStartupEvent) SetEvent(gAsiStartupEvent);

    HANDLE postLoadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                        GetCurrentProcessId());
    if (postLoadSnapshot != INVALID_HANDLE_VALUE)
    {
        DWORD postLoadScanned = 0;
        DWORD postLoadPatched = 0;
        MODULEENTRY32 postLoadEntry{};
        postLoadEntry.dwSize = sizeof(postLoadEntry);
        if (Module32First(postLoadSnapshot, &postLoadEntry))
        {
            do
            {
                if (InterlockedCompareExchange(&gShuttingDown, 0, 0) != 0)
                    break;
                ++postLoadScanned;
                postLoadPatched += PatchModuleImports(postLoadEntry.hModule,
                    postLoadEntry.hModule == GetModuleHandleA(nullptr));
            } while (Module32Next(postLoadSnapshot, &postLoadEntry));
        }
        CloseHandle(postLoadSnapshot);
        ForceLog("[IAT-POST-ASI-SCAN] loaded modules scanned=%lu additional imports patched=%lu",
                 postLoadScanned, postLoadPatched);
    }
    else
    {
        ForceLog("[IAT-POST-ASI-SCAN-FAILED] CreateToolhelp32Snapshot error=%lu", GetLastError());
    }
    return 0;
}

void RestoreIatPatches()
{
    if (gIatLockInitialized)
        EnterCriticalSection(&gIatLock);
    for (DWORD i = gIatPatchCount; i > 0; --i)
    {
        IatPatch& patch = gIatPatches[i - 1];
        if (!patch.slot)
            continue;
        DWORD oldProtect = 0;
        if (VirtualProtect(patch.slot, sizeof(DWORD), PAGE_READWRITE, &oldProtect))
        {
            *patch.slot = patch.original;
            DWORD ignored = 0;
            VirtualProtect(patch.slot, sizeof(DWORD), oldProtect, &ignored);
        }
    }
    gIatPatchCount = 0;
    if (gIatLockInitialized)
        LeaveCriticalSection(&gIatLock);
}

void FreeSearchContexts()
{
    SearchContext* current = gSearchContexts;
    gSearchContexts = nullptr;
    while (current)
    {
        SearchContext* next = current->next;
        if (current->token != INVALID_HANDLE_VALUE && gRealFindClose)
            gRealFindClose(current->token);
        delete current;
        current = next;
    }
}

}

extern "C" char* __cdecl ResolveProfileSavePath(const char* input)
{
    return ResolveProfileSavePathInternal(input);
}


static bool WaitForModsReadyBeforeGameOpen(const char* filename)
{
    if (ModsReady())
        return true;
    if (InterlockedCompareExchange(&gStartupReadyWaitTimedOut, 0, 0) != 0)
        return false;

    if (!gModsReadyEvent)
    {
        const DWORD start = GetTickCount();
        while (!ModsReady() && static_cast<DWORD>(GetTickCount() - start) < 15000)
            Sleep(1);
        const bool ready = ModsReady();
        if (!ready)
            InterlockedExchange(&gStartupReadyWaitTimedOut, 1);
        if (ready)
            LogMessage("[STARTUP-READY] request=%s fallback=poll",
                       filename ? filename : "<null>");
        else
            LogMessage("[STARTUP-READY-TIMEOUT] request=%s fallback=poll state=%ld",
                       filename ? filename : "<null>",
                       InterlockedCompareExchange(&gModsState, 0, 0));
        return ready;
    }

    const LONG waitTrace = InterlockedIncrement(&gStartupReadyWaitTraceCount);
    if (gLogEnabled && waitTrace <= 32)
        LogMessage("[STARTUP-READY-WAIT] request=%s state=%ld",
                   filename ? filename : "<null>",
                   InterlockedCompareExchange(&gModsState, 0, 0));

    const DWORD waitResult = WaitForSingleObject(gModsReadyEvent, 15000);
    const bool ready = waitResult == WAIT_OBJECT_0 && ModsReady();
    if (!ready)
        InterlockedExchange(&gStartupReadyWaitTimedOut, 1);
    if (gLogEnabled && waitTrace <= 32)
    {
        if (ready)
            LogMessage("[STARTUP-READY] request=%s wait=%lu",
                       filename ? filename : "<null>", waitResult);
        else
            LogMessage("[STARTUP-READY-TIMEOUT] request=%s wait=%lu state=%ld error=%lu",
                       filename ? filename : "<null>", waitResult,
                       InterlockedCompareExchange(&gModsState, 0, 0), GetLastError());
    }
    return ready;
}

extern "C" char* __cdecl ResolveReadOverride(const char* filename, const char* mode)
{
    if (!ModsReady())
    {
        if (!WaitForModsReadyBeforeGameOpen(filename))
            return nullptr;
    }

    if (gEnabled && gLoadAsiPlugins && gAsiStartupEvent && filename)
    {
        const char* leaf = filename;
        for (const char* q = filename; *q; ++q)
            if (*q == '\\' || *q == '/') leaf = q + 1;
        if (_stricmp(leaf, "crshpath.ini") == 0)
        {
            DWORD result = WaitForSingleObject(gAsiStartupEvent, 15000);
            if (result != WAIT_OBJECT_0)
                ForceLog("[ASI-STARTUP-WAIT-FAILED] result=%lu", result);
        }
    }

    if (gLogEnabled)
    {
        const LONG traceIndex = InterlockedIncrement(&gOpenTraceCount);
        if (traceIndex <= 256)
            LogMessage("[OPEN-CHECK %03ld] file=%s mode=%s", traceIndex,
                       filename ? filename : "<null>", mode ? mode : "<null>");
    }
    if (!gEnabled || gModCount == 0 || !IsReadOnlyOpenMode(mode))
        return nullptr;

    char relative[kMaxPathBuffer]{};
    if (!MakeRelativeFilePath(filename, relative, sizeof(relative)))
    {
        const LONG traceIndex = InterlockedIncrement(&gPathMapTraceCount);
        if (gLogEnabled && traceIndex <= 128)
            LogMessage("[OPEN-MAP-FAIL %03ld] file=%s gameRoot=%s", traceIndex,
                       filename ? filename : "<null>", gGameDir);
        return nullptr;
    }

    const LONG mapTraceIndex = InterlockedIncrement(&gPathMapTraceCount);
    if (gLogEnabled && mapTraceIndex <= 256)
        LogMessage("[OPEN-MAP %03ld] %s -> %s", mapTraceIndex, filename, relative);

    if (std::strchr(relative, '\\') == nullptr && IsModMetadataAtRoot(relative))
        return nullptr;

    char* output = GetThreadPathBuffer();
    if (!output)
        return nullptr;

    char editorCategoryRelative[MAX_PATH]{};
    const int editorCategoryIndex = ResolveEditorCategoryRelative(relative,
        editorCategoryRelative, sizeof(editorCategoryRelative));
    if (editorCategoryIndex >= 0)
    {
        LogMessage("[EDITOR-CAT-REQUEST] file=%s relative=%s canonical=%s cwd=%s index=%d",
                   filename, relative, editorCategoryRelative,
                   gLogicalCwd[0] ? gLogicalCwd : "<unset>", editorCategoryIndex);
        const int mergeResult = BuildMergedEditorCategory(editorCategoryRelative, output, kMaxPathBuffer);
        if (mergeResult == 1)
        {
            LogMessage("[EDITOR-CAT-REDIRECT] %s -> %s", filename, output);
            return output;
        }

        if (mergeResult <= 0)
            return nullptr;
    }

    for (int i = static_cast<int>(gModCount) - 1; i >= 0; --i)
    {
        if (!BuildModFilePath(gMods[i], relative, output, kMaxPathBuffer))
            continue;
        const DWORD attributes = GetFileAttributesA(output);
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            LogMessage("[OPEN-CANDIDATE] %s -> %s (mod=%s)", filename, output, gMods[i].name);
            return output;
        }
    }
    const LONG missIndex = InterlockedIncrement(&gOpenMissTraceCount);
    if (gLogEnabled && missIndex <= 256)
        LogMessage("[OPEN-NO-OVERRIDE %03ld] request=%s relative=%s mods=%u",
                   missIndex, filename, relative, static_cast<unsigned>(gModCount));
    return nullptr;
}


extern "C" void __cdecl LogOpenOutcome(const char* filename, int opened)
{
    const LONG trace = InterlockedIncrement(&gOpenResultTraceCount);
    if (!gLogEnabled)
        return;

    if (trace > 256)
        return;
    LogMessage(opened ? "[OK] %s" : "[FAILED] %s",
               filename ? filename : "<null>");
}

extern "C" __declspec(naked) int GameOpenHook()
{
    __asm
    {
        push ebp
        mov ebp, esp
        sub esp, 10h
        mov dword ptr [ebp-4], eax 
        mov dword ptr [ebp-8], edx 

        push eax
        call ResolveProfileSavePath
        add esp, 4
        test eax, eax
        jz no_profile_save_redirect
        mov dword ptr [ebp-4], eax

        mov eax, dword ptr [ebp-4]
        mov edx, dword ptr [ebp-8]
        call dword ptr [gOriginalOpen]
        mov dword ptr [ebp-10h], eax
        push eax
        push dword ptr [ebp-4]
        call LogOpenOutcome
        add esp, 8
        mov eax, dword ptr [ebp-10h]
        jmp hook_done

    no_profile_save_redirect:
        push dword ptr [ebp-8]
        push dword ptr [ebp-4]
        call ResolveReadOverride
        add esp, 8
        mov dword ptr [ebp-0Ch], eax
        test eax, eax
        jz use_original_path

        mov eax, dword ptr [ebp-0Ch]
        mov edx, dword ptr [ebp-8]
        call dword ptr [gOriginalOpen]
        mov dword ptr [ebp-10h], eax
        push eax
        push dword ptr [ebp-0Ch]
        call LogOpenOutcome
        add esp, 8
        mov eax, dword ptr [ebp-10h]
        test eax, eax
        jnz hook_done

    use_original_path:
        mov eax, dword ptr [ebp-4]
        mov edx, dword ptr [ebp-8]
        call dword ptr [gOriginalOpen]
        mov dword ptr [ebp-10h], eax
        push eax
        push dword ptr [ebp-4]
        call LogOpenOutcome
        add esp, 8
        mov eax, dword ptr [ebp-10h]

    hook_done:
        mov esp, ebp
        pop ebp
        ret
    }
}


BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        gSelfModule = instance;
        InitializeCriticalSection(&gSearchLock);
        InitializeCriticalSection(&gIatLock);
        InitializeCriticalSection(&gEditorMergeLock);
        gEditorMergeLockInitialized = true;
        gIatLockInitialized = true;
        gPathTls = TlsAlloc();
        SetGamePaths();

        if (!gRootReady)
        {
            OutputDebugStringA("CDModLoader: unable to determine game directory.\n");
            return TRUE;
        }

        gLogEnabled = GetPrivateProfileIntA(
            "ModLoader", "Log", 0, gIniPath
        ) != 0;

        if (gLogEnabled)
        {
            HANDLE logFile = CreateFileA(
                gLogPath,
                GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            );

            if (logFile != INVALID_HANDLE_VALUE)
            {
                CloseHandle(logFile);
            }
            else
            {
                OutputDebugStringA("CDModLoader: failed to clear log file.\n");
            }
        }

        gAsiStartupEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!gAsiStartupEvent)
            ForceLog("[ERROR] %lu", GetLastError());

        HMODULE exeModule = GetModuleHandleA(nullptr);
        gOpenHookInstalled = InstallGameOpenHook(exeModule);

        if (!gOpenHookInstalled)
            ForceLog("[ERROR] Game file-open hook was not installed. This executable does not match the Crashday 1.2.");

        gIatPatchCount = 0;
        const DWORD patched = PatchGameImports(exeModule);
        ForceLog("Crashday ModLoader 1.0.0: OpenHook=%d at VA 0x%08X (RVA 0x%08X); main executable IAT entries patched: %lu.",
                 gOpenHookInstalled ? 1 : 0, kOpenFileVa, kOpenFileRva, patched);

        if (patched == 0)
            ForceLog("[WARN] No executable filesystem/load-library imports were patched.");

        if (!InstallGetProcAddressHook())
            ForceLog("[WARN] Dynamic GetProcAddress interception is unavailable.");

        gModsReadyEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        if (!gModsReadyEvent)
            ForceLog("[ERROR] %lu; early opens may use stock files", GetLastError());

        HANDLE scanThread = CreateThread(nullptr, 0, PatchLoadedModulesThread, nullptr, 0, nullptr);
        if (scanThread)
            CloseHandle(scanThread);
        else
            ForceLog("[ERROR] unable to start module import scan, error=%lu", GetLastError());
    }
    else if (reason == DLL_THREAD_DETACH)
    {
        if (gPathTls != TLS_OUT_OF_INDEXES)
        {
            void* buffer = TlsGetValue(gPathTls);
            if (buffer)
                HeapFree(GetProcessHeap(), 0, buffer);
            TlsSetValue(gPathTls, nullptr);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange(&gShuttingDown, 1);
        RestoreIatPatches();
        RestoreGameOpenHook();
        RestoreGetProcAddressHook();
        CleanupMergedEditorCategories();
        FreeSearchContexts();
        if (gAsiStartupEvent)
        {
            SetEvent(gAsiStartupEvent);
            CloseHandle(gAsiStartupEvent);
            gAsiStartupEvent = nullptr;
        }
        if (gPathTls != TLS_OUT_OF_INDEXES)
            TlsFree(gPathTls);
        DeleteCriticalSection(&gSearchLock);
        if (gEditorMergeLockInitialized)
        {
            DeleteCriticalSection(&gEditorMergeLock);
            gEditorMergeLockInitialized = false;
        }
        if (gIatLockInitialized)
        {
            DeleteCriticalSection(&gIatLock);
            gIatLockInitialized = false;
        }
        (void)reserved;
    }
    return TRUE;
}
