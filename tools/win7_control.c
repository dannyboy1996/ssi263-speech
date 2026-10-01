/* The must-fail controls of tools/check_win7_imports.py (Tomi: Windows 7 support): each build imports one thing
 * Windows 7 SP1 lacks, directly, so the check must name it.  Built with w64devkit's gcc by
 * check_win7_imports.py --control win8|ucrt|subsystem; never shipped.  No headers: the declarations are spelled out,
 * so no SDK version guard can hide them. */
typedef int BOOL;
typedef void *HANDLE;
typedef unsigned long DWORD;
#define WINAPI __stdcall

#if defined(WIN7_CONTROL_WIN8)
/* Windows 8 (processthreadsapi.h, _WIN32_WINNT >= 0x0602) */
__declspec(dllimport) BOOL WINAPI SetProcessInformation(HANDLE process, int infoClass, void *info, DWORD size);
__declspec(dllimport) HANDLE WINAPI GetCurrentProcess(void);

BOOL WINAPI DllMain(HANDLE module, DWORD reason, void *reserved)
{
    (void)module; (void)reserved;
    if (reason == 0xFFFF)
        return SetProcessInformation(GetCurrentProcess(), 0, 0, 0);
    return 1;
}

#elif defined(WIN7_CONTROL_UCRT)
/* the Universal CRT only (ucrtbase.dll): msvcrt.dll has no __stdio_common_vsprintf */
__declspec(dllimport) int __cdecl __stdio_common_vsprintf(unsigned long long options, char *buf, unsigned long long n,
                                                         const char *format, void *locale, char *args);

BOOL WINAPI DllMain(HANDLE module, DWORD reason, void *reserved)
{
    char buf[8];
    (void)module; (void)reserved;
    if (reason == 0xFFFF)
        return __stdio_common_vsprintf(0, buf, sizeof buf, "x", 0, 0);
    return 1;
}

#elif defined(WIN7_CONTROL_SUBSYSTEM)
/* plain Windows 7 imports; only the PE header's subsystem version (6.2) is wrong */
__declspec(dllimport) void WINAPI ExitProcess(unsigned int code);

void start(void)
{
    ExitProcess(0);
}

#else
#error "define WIN7_CONTROL_WIN8, WIN7_CONTROL_UCRT or WIN7_CONTROL_SUBSYSTEM"
#endif
