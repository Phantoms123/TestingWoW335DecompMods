#pragma once

// CHANGED (wowdecomp): not in upstream CoAVolFog. On Windows this is <windows.h>. Elsewhere (the Linux client,
// which draws through DXVK-native) it is DXVK's Windows types plus the handful of Win32 calls CoAVolFog uses,
// implemented with the C++ standard library.

#if defined(_WIN32)
#include <windows.h>

#define VF_TRY __try
#define VF_EXCEPT(filter) __except (filter)
#else
#include <windows.h>  // DXVK-native's Windows base types

#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

#ifndef _WPARAM_DEFINED
#define _WPARAM_DEFINED
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#ifndef LOWORD
#define LOWORD(l) (static_cast<WORD>(static_cast<uintptr_t>(l) & 0xFFFF))
#define HIWORD(l) (static_cast<WORD>((static_cast<uintptr_t>(l) >> 16) & 0xFFFF))
#endif

inline DWORD GetTickCount()
{
    using namespace std::chrono;
    return static_cast<DWORD>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
    using namespace std::chrono;
    counter->QuadPart = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    return TRUE;
}

struct CRITICAL_SECTION
{
    std::recursive_mutex* mutex = nullptr;
};

inline void InitializeCriticalSection(CRITICAL_SECTION* section)
{
    section->mutex = new std::recursive_mutex();
}

inline void EnterCriticalSection(CRITICAL_SECTION* section)
{
    section->mutex->lock();
}

inline void LeaveCriticalSection(CRITICAL_SECTION* section)
{
    section->mutex->unlock();
}

inline void DeleteCriticalSection(CRITICAL_SECTION* section)
{
    delete section->mutex;
    section->mutex = nullptr;
}

struct SYSTEMTIME
{
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

inline void GetLocalTime(SYSTEMTIME* t)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&seconds, &local);
    t->wYear = static_cast<WORD>(local.tm_year + 1900);
    t->wMonth = static_cast<WORD>(local.tm_mon + 1);
    t->wDayOfWeek = static_cast<WORD>(local.tm_wday);
    t->wDay = static_cast<WORD>(local.tm_mday);
    t->wHour = static_cast<WORD>(local.tm_hour);
    t->wMinute = static_cast<WORD>(local.tm_min);
    t->wSecond = static_cast<WORD>(local.tm_sec);
    t->wMilliseconds = static_cast<WORD>(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
}

struct FILETIME
{
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};

// Only the last-write time is used (to notice an edited CoAVolFog.ini).
struct WIN32_FILE_ATTRIBUTE_DATA
{
    FILETIME ftLastWriteTime;
};

enum GET_FILEEX_INFO_LEVELS
{
    GetFileExInfoStandard
};

inline BOOL GetFileAttributesExA(const char* path, GET_FILEEX_INFO_LEVELS, WIN32_FILE_ATTRIBUTE_DATA* data)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return FALSE;
    const uint64_t stamp = static_cast<uint64_t>(st.st_mtim.tv_sec) * 10000000ULL + st.st_mtim.tv_nsec / 100;
    data->ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(stamp);
    data->ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(stamp >> 32);
    return TRUE;
}

#include <cerrno>
inline DWORD GetLastError()
{
    return static_cast<DWORD>(errno);
}

inline int MulDiv(int number, int numerator, int denominator)
{
    return denominator ? static_cast<int>(static_cast<int64_t>(number) * numerator / denominator) : -1;
}

#include <strings.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _stricmp
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#endif

// Windows INI files ("private profiles"), enough for CoAVolFog.ini: case-insensitive section and key names,
// ';' and '#' comments, whitespace around '=' ignored.
namespace vf_ini
{
inline std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline std::vector<std::string> Lines(const char* path)
{
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

inline bool IsSection(const std::string& trimmed, std::string* name)
{
    if (trimmed.size() < 2 || trimmed.front() != '[')
        return false;
    const size_t close = trimmed.find(']');
    if (close == std::string::npos)
        return false;
    if (name)
        *name = Trim(trimmed.substr(1, close - 1));
    return true;
}

inline bool KeyOf(const std::string& trimmed, std::string* key, std::string* value)
{
    if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#')
        return false;
    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos)
        return false;
    *key = Trim(trimmed.substr(0, eq));
    if (value)
    {
        *value = Trim(trimmed.substr(eq + 1));
        if (value->size() >= 2 && ((value->front() == '"' && value->back() == '"') ||
                                   (value->front() == '\'' && value->back() == '\'')))
            *value = value->substr(1, value->size() - 2);
    }
    return true;
}

inline bool Find(const char* path, const char* section, const char* key, std::string& value)
{
    bool inSection = false;
    for (const std::string& line : Lines(path))
    {
        const std::string t = Trim(line);
        std::string name, k, v;
        if (IsSection(t, &name))
            inSection = strcasecmp(name.c_str(), section) == 0;
        else if (inSection && KeyOf(t, &k, &v) && strcasecmp(k.c_str(), key) == 0)
        {
            value = v;
            return true;
        }
    }
    return false;
}
}

inline DWORD GetPrivateProfileStringA(const char* section, const char* key, const char* fallback, char* out,
                                      DWORD size, const char* path)
{
    if (!out || !size)
        return 0;
    std::string value;
    if (!vf_ini::Find(path, section, key, value))
        value = fallback ? fallback : "";
    const size_t n = value.size() < size - 1 ? value.size() : size - 1;
    std::memcpy(out, value.data(), n);
    out[n] = 0;
    return static_cast<DWORD>(n);
}

inline UINT GetPrivateProfileIntA(const char* section, const char* key, INT fallback, const char* path)
{
    std::string value;
    if (!vf_ini::Find(path, section, key, value) || value.empty())
        return static_cast<UINT>(fallback);
    return static_cast<UINT>(std::strtol(value.c_str(), nullptr, 0));
}

inline BOOL WritePrivateProfileStringA(const char* section, const char* key, const char* value, const char* path)
{
    std::vector<std::string> lines = vf_ini::Lines(path);
    bool inSection = false;
    size_t sectionEnd = std::string::npos;
    bool written = false;
    for (size_t i = 0; i < lines.size() && !written; i++)
    {
        const std::string t = vf_ini::Trim(lines[i]);
        std::string name, k;
        if (vf_ini::IsSection(t, &name))
        {
            if (inSection)
                break;
            inSection = strcasecmp(name.c_str(), section) == 0;
            if (inSection)
                sectionEnd = i + 1;
        }
        else if (inSection)
        {
            if (!t.empty())
                sectionEnd = i + 1;
            if (vf_ini::KeyOf(t, &k, nullptr) && strcasecmp(k.c_str(), key) == 0)
            {
                if (value)
                    lines[i] = std::string(key) + "=" + value;
                else
                    lines.erase(lines.begin() + i);
                written = true;
            }
        }
    }
    if (!written && value)
    {
        const std::string entry = std::string(key) + "=" + value;
        if (sectionEnd == std::string::npos)
        {
            lines.push_back("[" + std::string(section) + "]");
            lines.push_back(entry);
        }
        else
            lines.insert(lines.begin() + sectionEnd, entry);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return FALSE;
    for (const std::string& line : lines)
        out << line << "\r\n";
    return out.good() ? TRUE : FALSE;
}

// Windows virtual-key codes, which CoAVolFog's hotkey settings are written in. The Linux overlay maps SDL key
// codes onto these (see overlay.cpp).
enum : unsigned
{
    VK_CANCEL = 0x03, VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
    VK_MENU = 0x12, VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21,
    VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D,
    VK_NUMPAD0 = 0x60, VK_NUMPAD9 = 0x69, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SEPARATOR = 0x6C,
    VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F12 = 0x7B, VK_F24 = 0x87,
    VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2,
    VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB,
    VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0,
    VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE, VK_OEM_102 = 0xE2,
    VK_VOLUME_MUTE = 0xAD, VK_VOLUME_DOWN = 0xAE, VK_VOLUME_UP = 0xAF, VK_MEDIA_NEXT_TRACK = 0xB0,
    VK_MEDIA_PREV_TRACK = 0xB1, VK_MEDIA_STOP = 0xB2, VK_MEDIA_PLAY_PAUSE = 0xB3,
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04
};

#ifndef InterlockedIncrement
inline LONG InterlockedIncrement(volatile LONG* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }
inline LONG InterlockedDecrement(volatile LONG* value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }
#endif

// Structured exception handling does not exist outside MSVC. The guarded blocks run unguarded on Linux; a fault
// there crashes like any other (and Storm's handler prints the backtrace).
#define VF_TRY if (true)
#define VF_EXCEPT(filter) else

#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif
