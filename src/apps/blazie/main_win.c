/* main_win.c -- the Windows shell of the Blazie emulator: one window, a menu, the keyboard, the sound card.
 *
 * While the window is in front, the braille keys (default S D F J K L = dots 3 2 1 4 5 6, the space bar, and A or ;
 * for the advance bar) go to the unit as chords (chords.h); every other key goes to Windows as usual, so Alt opens
 * the menu and Alt+F4 closes it.  Sound: waveOut, four blocks of 20 ms, each rendered by the unit when the card gives
 * one back -- the card's clock paces the unit.  Settings: blazie_emu.ini beside the program.
 *
 * Firmware: firmware\ beside the program (a release carries it), or firmware_dir= in the settings.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "chords.h"
#include "emu_unit.h"

#define RATE 44100
#define BLOCK (RATE / 50)
#define NBLOCKS 4

enum { ID_EN = 100, ID_ES, ID_TNS, ID_HISS = 200, ID_WHINE, ID_QUIET, ID_KEYS = 300, ID_ABOUT };

typedef struct { const char *name, *firmware, *state; } unit_kind;
static const unit_kind KINDS[] = {
    {"Braille Lite 2000 (English)", "BL2ENG.BNS", "bl2_2003_warm.state"},
    {"Braille Lite 2000 (Spanish)", "spanish\\BL2SPA.BNS", "spanish\\bl2spa_warm.state"},
};

static HWND g_wnd;
static CRITICAL_SECTION g_lock;
static emu_unit *g_unit;
static int g_kind, g_whine = 1;
static chord_state g_chord;
static HWAVEOUT g_wave;
static WAVEHDR g_hdr[NBLOCKS];
static short g_buf[NBLOCKS][BLOCK];
static HANDLE g_wave_event, g_thread;
static volatile LONG g_quit;
static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_fw_dir[MAX_PATH];
static int g_keymap[256];                  /* virtual key -> chord bit */

static void status(const char *text)
{
    char title[256];
    snprintf(title, sizeof title, "%s - Blazie emulator", text);
    SetWindowTextA(g_wnd, title);
}

/* ---- the keyboard map --------------------------------------------------------------------------------------- */
static int vk_of(const char *name)
{
    if (!strcmp(name, "space")) return VK_SPACE;
    if (!strcmp(name, ";")) return VK_OEM_1;
    if (!strcmp(name, ",")) return VK_OEM_COMMA;
    if (!strcmp(name, ".")) return VK_OEM_PERIOD;
    if (!strcmp(name, "/")) return VK_OEM_2;
    if (strlen(name) == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];
    if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z')
        return name[0] - 'a' + 'A';
    return 0;
}

static void map_keys(const char *setting, const char *dflt, int bit)
{
    char v[128], *tok;
    GetPrivateProfileStringA("keys", setting, dflt, v, sizeof v, g_ini);
    for (tok = strtok(v, " ,"); tok; tok = strtok(NULL, " ,")) {
        int vk = vk_of(tok);
        if (vk)
            g_keymap[vk] = bit;
    }
}

static void load_keymap(void)
{
    memset(g_keymap, 0, sizeof g_keymap);
    map_keys("dot1", "F", CHORD_DOT(1));
    map_keys("dot2", "D", CHORD_DOT(2));
    map_keys("dot3", "S", CHORD_DOT(3));
    map_keys("dot4", "J", CHORD_DOT(4));
    map_keys("dot5", "K", CHORD_DOT(5));
    map_keys("dot6", "L", CHORD_DOT(6));
    map_keys("space", "space", CHORD_SPACE);
    map_keys("advance", "A ;", CHORD_ADVANCE);
}

/* ---- the unit ------------------------------------------------------------------------------------------------ */
static int start_unit(int kind)
{
    char fw[MAX_PATH], st[MAX_PATH], err[256];
    emu_unit *u;
    snprintf(fw, sizeof fw, "%s\\%s", g_fw_dir, KINDS[kind].firmware);
    snprintf(st, sizeof st, "%s\\%s", g_fw_dir, KINDS[kind].state);
    status("Starting");
    u = emu_create(fw, st, RATE, g_whine, err, sizeof err);
    if (!u) {
        char msg[700];
        snprintf(msg, sizeof msg, "Could not start the %s.\n\n%s\n\nFirmware looked for in:\n%s", KINDS[kind].name,
                 err, g_fw_dir);
        MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONERROR);
        return 0;
    }
    EnterCriticalSection(&g_lock);
    emu_destroy(g_unit);
    g_unit = u;
    g_kind = kind;
    chord_reset(&g_chord);
    LeaveCriticalSection(&g_lock);
    status(KINDS[kind].name);
    CheckMenuRadioItem(GetMenu(g_wnd), ID_EN, ID_TNS, ID_EN + kind, MF_BYCOMMAND);
    WritePrivateProfileStringA("unit", "kind", kind ? "spanish" : "english", g_ini);
    return 1;
}

/* ---- the sound card: the unit renders each block as the card hands it back ---------------------------------- */
static DWORD WINAPI audio_thread(LPVOID arg)
{
    int i;
    (void)arg;
    while (!g_quit) {
        WaitForSingleObject(g_wave_event, 100);
        for (i = 0; i < NBLOCKS && !g_quit; i++) {
            if (!(g_hdr[i].dwFlags & WHDR_DONE))
                continue;
            EnterCriticalSection(&g_lock);
            if (g_unit)
                emu_render(g_unit, g_buf[i], BLOCK);
            else
                memset(g_buf[i], 0, sizeof g_buf[i]);
            LeaveCriticalSection(&g_lock);
            g_hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        }
    }
    return 0;
}

static int open_audio(void)
{
    WAVEFORMATEX f;
    int i;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = RATE * 2;
    g_wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &f, (DWORD_PTR)g_wave_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return 0;
    for (i = 0; i < NBLOCKS; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof g_buf[i];
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        g_hdr[i].dwFlags |= WHDR_DONE;     /* all free: the thread fills them */
    }
    g_thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    SetThreadPriority(g_thread, THREAD_PRIORITY_TIME_CRITICAL);
    SetEvent(g_wave_event);
    return 1;
}

static void close_audio(void)
{
    int i;
    InterlockedExchange(&g_quit, 1);
    SetEvent(g_wave_event);
    WaitForSingleObject(g_thread, 2000);
    waveOutReset(g_wave);
    for (i = 0; i < NBLOCKS; i++)
        waveOutUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
}

/* ---- the window ------------------------------------------------------------------------------------------------ */
static void set_whine(int w)
{
    g_whine = w;
    EnterCriticalSection(&g_lock);
    if (g_unit)
        emu_set_whine(g_unit, w);
    LeaveCriticalSection(&g_lock);
    CheckMenuRadioItem(GetMenu(g_wnd), ID_HISS, ID_QUIET, w == 1 ? ID_HISS : w == 2 ? ID_WHINE : ID_QUIET,
                       MF_BYCOMMAND);
    WritePrivateProfileStringA("sound", "idle", w == 1 ? "hiss" : w == 2 ? "whine" : "off", g_ini);
}

static HMENU make_menu(void)
{
    HMENU bar = CreateMenu(), unit = CreatePopupMenu(), sound = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuA(unit, MF_STRING, ID_EN, "Braille Lite 2000, &English");
    AppendMenuA(unit, MF_STRING, ID_ES, "Braille Lite 2000, &Spanish");
    AppendMenuA(unit, MF_STRING | MF_GRAYED, ID_TNS, "&Type 'n Speak (not yet)");
    AppendMenuA(sound, MF_STRING, ID_HISS, "Idle channel: &hiss");
    AppendMenuA(sound, MF_STRING, ID_WHINE, "Idle channel: &whine");
    AppendMenuA(sound, MF_STRING, ID_QUIET, "Idle channel: &silent");
    AppendMenuA(help, MF_STRING, ID_KEYS, "&Keys");
    AppendMenuA(help, MF_STRING, ID_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)unit, "&Unit");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)sound, "&Sound");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)help, "&Help");
    return bar;
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp < 256 && g_keymap[wp]) {
            EnterCriticalSection(&g_lock);
            chord_down(&g_chord, g_keymap[wp]);
            LeaveCriticalSection(&g_lock);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wp < 256 && g_keymap[wp]) {
            int chord;
            EnterCriticalSection(&g_lock);
            chord = chord_up(&g_chord, g_keymap[wp]);
            if (chord && g_unit)
                emu_key(g_unit, chord);
            LeaveCriticalSection(&g_lock);
            return 0;
        }
        break;
    case WM_CHAR:
        return 0;                           /* no beeps for the braille keys */
    case WM_KILLFOCUS:
        EnterCriticalSection(&g_lock);
        chord_reset(&g_chord);              /* a chord half-pressed when the window lost the keyboard is dropped */
        LeaveCriticalSection(&g_lock);
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_EN: case ID_ES: start_unit(LOWORD(wp) - ID_EN); return 0;
        case ID_HISS: set_whine(1); return 0;
        case ID_WHINE: set_whine(2); return 0;
        case ID_QUIET: set_whine(0); return 0;
        case ID_KEYS:
            MessageBoxA(w, "Braille keys, while this window is in front:\n\n"
                        "F D S = dots 1 2 3\nJ K L = dots 4 5 6\nSpace bar = space\nA or ; = advance bar\n\n"
                        "Press the keys of a chord together; it goes to the unit when you let go of them.\n"
                        "The keys can be changed in blazie_emu.ini, section [keys].\n\n"
                        "Alt opens this program's menu; Alt+F4 closes it.", "Keys", MB_OK);
            return 0;
        case ID_ABOUT:
            MessageBoxA(w, "Blazie emulator, part of ssi263-speech.\n\n"
                        "It runs Blazie Engineering's own firmware on an emulated board with an emulated SSI-263 "
                        "speech chip. The firmware is shared with permission. This program is not a product of "
                        "Blazie Engineering.\n\n"
                        "Z180 core: z180emu (GPL-2.0-or-later), so this program is GPL.", "About", MB_OK);
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* firmware_dir= in the settings; else firmware\ beside the program; else the source tree's firmware\blazie\ when
   the program runs from nvda\dist\blazie-emu\ */
static void find_firmware(void)
{
    char probe[MAX_PATH];
    GetPrivateProfileStringA("unit", "firmware_dir", "", g_fw_dir, sizeof g_fw_dir, g_ini);
    if (g_fw_dir[0])
        return;
    snprintf(g_fw_dir, sizeof g_fw_dir, "%s\\firmware", g_dir);
    snprintf(probe, sizeof probe, "%s\\BL2ENG.BNS", g_fw_dir);
    if (exists(probe))
        return;
    snprintf(probe, sizeof probe, "%s\\..\\..\\..\\firmware\\blazie\\BL2ENG.BNS", g_dir);
    if (exists(probe))
        snprintf(g_fw_dir, sizeof g_fw_dir, "%s\\..\\..\\..\\firmware\\blazie", g_dir);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSA wc;
    MSG m;
    char *slash, v[32];
    (void)prev; (void)cmd;
    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    if ((slash = strrchr(g_dir, '\\')) != NULL)
        *slash = 0;
    snprintf(g_ini, sizeof g_ini, "%s\\blazie_emu.ini", g_dir);
    find_firmware();
    load_keymap();
    InitializeCriticalSection(&g_lock);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "BlazieEmulator";
    RegisterClassA(&wc);
    g_wnd = CreateWindowA("BlazieEmulator", "Blazie emulator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          480, 240, NULL, make_menu(), inst, NULL);
    ShowWindow(g_wnd, show);
    GetPrivateProfileStringA("sound", "idle", "hiss", v, sizeof v, g_ini);
    g_whine = !strcmp(v, "whine") ? 2 : !strcmp(v, "off") ? 0 : 1;
    CheckMenuRadioItem(GetMenu(g_wnd), ID_HISS, ID_QUIET, g_whine == 1 ? ID_HISS : g_whine == 2 ? ID_WHINE : ID_QUIET,
                       MF_BYCOMMAND);
    if (!open_audio())
        MessageBoxA(g_wnd, "Could not open the sound card.", "Blazie emulator", MB_OK | MB_ICONERROR);
    GetPrivateProfileStringA("unit", "kind", "english", v, sizeof v, g_ini);
    start_unit(!strcmp(v, "spanish") ? 1 : 0);
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    close_audio();
    emu_destroy(g_unit);
    return 0;
}
