/* main_win.c -- the Windows shell of the Blazie emulator: one window, a menu, the keyboard, the sound card.
 *
 * While the window is in front:
 *   Braille Lite: the braille keys (default S D F J K L = dots 3 2 1 4 5 6, the space bar, and A or ; for the
 *   advance bar) go to the unit as chords (chords.h); every other key goes to Windows as usual, so Alt opens the
 *   menu and Alt+F4 closes it.
 *   Type 'n Speak: the whole keyboard is the unit's (tns_keymap_win.c) -- Alt and F10 included -- except F11, which
 *   opens this program's menu.
 * Sound: waveOut, four blocks of 20 ms, each rendered by the unit when the card gives one back -- the card's clock
 * paces the unit.  Settings: blazie_emu.ini beside the program.
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
#include "tns_keymap_win.h"

#define RATE 44100
#define BLOCK (RATE / 50)
#define NBLOCKS 4

enum { ID_EN = 100, ID_ES, ID_TNS_EN, ID_TNS_ES, ID_FACTORY, ID_EXIT, ID_HISS = 200, ID_WHINE, ID_QUIET,
       ID_KEYS = 300, ID_ABOUT };

/* state NULL: a cold start (the Type 'n Speak asks to initialise its flash; answer y twice) */
typedef struct { const char *name; int kind; const char *firmware, *state, *saved, *ini; } unit_kind;
static const unit_kind KINDS[] = {
    {"Braille Lite 2000 (English)", EMU_BRAILLE_LITE, "BL2ENG.BNS", "bl2_2003_warm.state", "english.state",
     "english"},
    {"Braille Lite 2000 (Spanish)", EMU_BRAILLE_LITE, "spanish\\BL2SPA.BNS", "spanish\\bl2spa_fresh.state",
     "spanish.state", "spanish"},
    {"Type 'n Speak (English)", EMU_TYPE_N_SPEAK, "tns\\TNSENG.TNS", NULL, "tns_english.state", "tns_english"},
    {"Type 'n Speak (Spanish)", EMU_TYPE_N_SPEAK, "tns\\TNSSPA.TNS", NULL, "tns_spanish.state", "tns_spanish"},
};
#define N_KINDS ((int)(sizeof KINDS / sizeof KINDS[0]))

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
static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_fw_dir[MAX_PATH], g_save_dir[MAX_PATH];
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
static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* what the unit keeps while switched off (its files and settings): %APPDATA%\ssi263-speech\blazie-emu\<kind>.state */
static void saved_path(int kind, char *out, int cap)
{
    snprintf(out, cap, "%s\\%s", g_save_dir, KINDS[kind].saved);
}

static void save_unit(void)
{
    char path[MAX_PATH];
    int ok = 1;
    if (!g_unit)
        return;
    CreateDirectoryA(g_save_dir, NULL);
    saved_path(g_kind, path, sizeof path);
    EnterCriticalSection(&g_lock);
    ok = emu_save(g_unit, path);
    LeaveCriticalSection(&g_lock);
    if (!ok)
        MessageBoxA(g_wnd, "Could not save the unit's memory; what you wrote this time is lost.", "Blazie emulator",
                    MB_OK | MB_ICONWARNING);
}

static int start_unit(int kind)
{
    char fw[MAX_PATH], st[MAX_PATH], err[256];
    const char *state = st;
    emu_unit *u;
    save_unit();                            /* first: the unit being left keeps its memory (it may be this kind) */
    snprintf(fw, sizeof fw, "%s\\%s", g_fw_dir, KINDS[kind].firmware);
    saved_path(kind, st, sizeof st);
    if (!exists(st)) {                      /* the first time: the unit as it left the factory */
        if (KINDS[kind].state)
            snprintf(st, sizeof st, "%s\\%s", g_fw_dir, KINDS[kind].state);
        else
            state = NULL;                   /* a cold start */
    }
    status("Starting");
    u = emu_create(KINDS[kind].kind, fw, state, RATE, g_whine, err, sizeof err);
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
    CheckMenuRadioItem(GetMenu(g_wnd), ID_EN, ID_EN + N_KINDS - 1, ID_EN + kind, MF_BYCOMMAND);
    WritePrivateProfileStringA("unit", "kind", KINDS[kind].ini, g_ini);
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
    AppendMenuA(unit, MF_STRING, ID_TNS_EN, "&Type 'n Speak, English");
    AppendMenuA(unit, MF_STRING, ID_TNS_ES, "Type 'n Speak, S&panish");
    AppendMenuA(unit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(unit, MF_STRING, ID_FACTORY, "Back to the &factory state (erases this unit's files)...");
    AppendMenuA(unit, MF_STRING, ID_EXIT, "E&xit");
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

/* the Type 'n Speak's keys held now (their down codes), so a key held when the window loses the keyboard is let go */
static unsigned char g_tns_held[256];

static int tns_active(void)
{
    return g_unit && emu_kind(g_unit) == EMU_TYPE_N_SPEAK;
}

static void tns_release_all(void)
{
    int vk;
    EnterCriticalSection(&g_lock);
    for (vk = 0; vk < 256; vk++)
        if (g_tns_held[vk]) {
            if (tns_active())
                emu_key(g_unit, g_tns_held[vk] & 0x7F);
            g_tns_held[vk] = 0;
        }
    LeaveCriticalSection(&g_lock);
}

static LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (tns_active() && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP)) {
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN, code;
        if (wp == VK_F11) {                 /* this program's own key: its menu */
            if (down)
                PostMessageA(w, WM_SYSCOMMAND, SC_KEYMENU, 0);
            return 0;
        }
        code = wp < 256 ? tns_code_for_key(wp, lp) : 0;
        if (code) {
            EnterCriticalSection(&g_lock);
            if (down && !g_tns_held[wp]) {  /* the first down only: auto-repeat is the unit's business */
                g_tns_held[wp] = (unsigned char)code;
                emu_key(g_unit, code);
            } else if (!down && g_tns_held[wp]) {
                emu_key(g_unit, g_tns_held[wp] & 0x7F);
                g_tns_held[wp] = 0;
            }
            LeaveCriticalSection(&g_lock);
        }
        return 0;                           /* nothing of the unit's keyboard reaches Windows (no menu on Alt) */
    }
    if (tns_active() && (msg == WM_CHAR || msg == WM_SYSCHAR))
        return 0;
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
        tns_release_all();                  /* and the Type 'n Speak's held keys come up */
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_EN: case ID_ES: case ID_TNS_EN: case ID_TNS_ES:
            tns_release_all();
            start_unit(LOWORD(wp) - ID_EN);
            return 0;
        case ID_EXIT: DestroyWindow(w); return 0;
        case ID_FACTORY: {
            char path[MAX_PATH];
            emu_unit *old;
            if (MessageBoxA(w, "Put this unit back as it left the factory? Its files and settings are erased.",
                            "Blazie emulator", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return 0;
            EnterCriticalSection(&g_lock);
            old = g_unit;
            g_unit = NULL;                  /* so start_unit saves nothing over the deletion */
            LeaveCriticalSection(&g_lock);
            emu_destroy(old);
            saved_path(g_kind, path, sizeof path);
            DeleteFileA(path);
            start_unit(g_kind);
            return 0;
        }
        case ID_HISS: set_whine(1); return 0;
        case ID_WHINE: set_whine(2); return 0;
        case ID_QUIET: set_whine(0); return 0;
        case ID_KEYS:
            MessageBoxA(w, "Braille Lite, while this window is in front:\n\n"
                        "F D S = dots 1 2 3\nJ K L = dots 4 5 6\nSpace bar = space\nA or ; = advance bar\n\n"
                        "Press the keys of a chord together; it goes to the unit when you let go of them.\n"
                        "The keys can be changed in blazie_emu.ini, section [keys].\n"
                        "Alt opens this program's menu; Alt+F4 closes it.\n\n"
                        "Type 'n Speak: the whole keyboard is the unit's, Alt and the function keys included.\n"
                        "F11 opens this program's menu (Unit > Exit closes it).\n"
                        "The first time, the unit asks to initialize its flash: press y, then y again "
                        "(the Spanish unit: s, then s).", "Keys", MB_OK);
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
    {
        char appdata[MAX_PATH];
        if (!GetEnvironmentVariableA("APPDATA", appdata, sizeof appdata))
            snprintf(appdata, sizeof appdata, "%s", g_dir);
        snprintf(g_save_dir, sizeof g_save_dir, "%s\\ssi263-speech", appdata);
        CreateDirectoryA(g_save_dir, NULL);
        snprintf(g_save_dir, sizeof g_save_dir, "%s\\ssi263-speech\\blazie-emu", appdata);
    }
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
    {
        int k, start = 0;
        for (k = 0; k < N_KINDS; k++)
            if (!strcmp(v, KINDS[k].ini))
                start = k;
        start_unit(start);
    }
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    close_audio();
    save_unit();                            /* switched off: the memory is kept */
    emu_destroy(g_unit);
    return 0;
}
