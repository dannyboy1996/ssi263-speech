/* main_win.c -- the Windows shell of the Blazie emulator: one window, a menu, the keyboard, the sound card.
 *
 * While the window is in front:
 *   Braille Lite: the braille keys (default S D F J K L = dots 3 2 1 4 5 6, the space bar, and A or ; for the
 *   advance bar) go to the unit as chords (chords.h); every other key goes to Windows as usual, so Alt opens the
 *   menu and Alt+F4 closes it.
 *   Type 'n Speak: the whole keyboard is the unit's (tns_keymap_win.c) -- Alt and F10 included -- except F11, which
 *   opens this program's menu.
 * Sound: waveOut, four blocks of 10 ms ([sound] block_ms, 5-20), each rendered by the unit when the card gives one
 * back -- the card's clock paces the unit.  A key reaches the unit at the next block rendered, which plays behind the
 * blocks already queued: four blocks of 20 ms (0.6.0) made that 60-80 ms, 10 ms blocks make it 30-40.
 * Settings: blazie_emu.ini beside the program.  Settings > Serial port plugs the unit's serial port
 * into a COM port (serial_win.c), for WinDisk, PCDISK or a terminal on the other end.
 *
 * Firmware: firmware\ beside the program (a release carries it), or firmware_dir= in the settings.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "chords.h"
#include "emu_unit.h"
#include "serial_win.h"
#include "tns_keymap_win.h"
#include "tns_rescue.h"
#include "tns_setup.h"
#include "../../csrc/blazie/bl_files.h"
#include "../../csrc/blazie/bl_files_state.h"
#include "../../csrc/blazie/bl_files_xfer.h"
#include "../../csrc/blazie/fat_img.h"

#define RATE_MAX 48000
#define BLOCK_MAX (RATE_MAX / 50)
#define NBLOCKS 4
#define BLOCK_MS_DEFAULT 10

enum { ID_EN = 100, ID_ES, ID_TNS_EN, ID_TNS_ES, ID_FACTORY, ID_EXIT, ID_EXPORT, ID_IMPORT,
       ID_HISS = 200, ID_WHINE, ID_QUIET, ID_UNITSOUND,
       ID_OPEN_OFF = 210, ID_OPEN_UNTIL, ID_OPEN_ALWAYS, ID_POPCLICK = 215, ID_TICK, ID_QUICK = 218,
       ID_RATE = 220,
       ID_KEYS = 300, ID_ABOUT,
       ID_SERIAL_NONE = 400, ID_SERIAL_PORT };   /* ID_SERIAL_PORT + k: g_ports[k] */

/* state NULL: a cold start (the Type 'n Speak's cold reset asks how to set itself up: TNS_FIRST_START) */
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
static int g_kind, g_whine = 3;          /* the idle channel: 0 silent, 1 hiss, 2 whine, 3 as the unit (by volume) */
static int g_keep_open = 1;              /* 0 off, 1 until the unit clicks off, 2 always */
static int g_popclick = 1, g_tick = 1;   /* the channel's pop and click-off, its 10 Hz tick */
static chord_state g_chord;
static HWAVEOUT g_wave;
static WAVEHDR g_hdr[NBLOCKS];
static short g_buf[NBLOCKS][BLOCK_MAX];
static int g_rate = 44100;               /* the sound card's rate; the unit renders at it */
static int g_block_ms = BLOCK_MS_DEFAULT;
static int g_quick;                      /* quick key response (emu_set_quick): off, as the real unit */
static const int RATES[] = {11025, 16000, 22050, 32000, 44100, 48000};
#define N_RATES ((int)(sizeof RATES / sizeof RATES[0]))
static HANDLE g_wave_event, g_thread;
static volatile LONG g_quit;
static char g_dir[MAX_PATH], g_ini[MAX_PATH], g_fw_dir[MAX_PATH], g_save_dir[MAX_PATH];
static int g_keymap[256];                  /* virtual key -> chord bit */
static com_link *g_link;                   /* the COM port the unit's serial port is plugged into; NULL: none */
static char g_com[16];                     /* its name ("COM10"), or "" */
static HMENU g_serial_menu;
static com_port_info g_ports[COM_MAX];     /* as the Serial port menu last listed them */
static int g_n_ports;

/* the window's title, which a screen reader reads on focus: the unit, and the COM port it is plugged into */
static void status(const char *text)
{
    char title[256];
    if (g_link)
        snprintf(title, sizeof title, "%s, serial port on %s - Blazie emulator", text, g_com);
    else
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

/* A saved Type 'n Speak that was never set up (tns_rescue.h: the 0.6 and 0.7 previews' first start missed the unit's
   cold reset): the person chooses -- set it up now keeping its RAM files, the factory state (its own questions), or
   as it is.  0: start it from no state (the factory's cold start); 1: from st. */
static int offer_setup(int kind, const char *fw, const char *st)
{
    char msg[4000], lost[300] = "", err[300], before[MAX_PATH + 20];
    tns_rescue_report r;
    int answer;
    emu_unit *old;
    if (tns_needs_setup(st, &r) != 1)
        return 1;
    EnterCriticalSection(&g_lock);          /* the running unit (already saved) switched off: the minute's save, */
    old = g_unit;                           /* which runs while a message box is up, must not write it over the */
    g_unit = NULL;                          /* state set up here */
    LeaveCriticalSection(&g_lock);
    emu_destroy(old);
    if (r.lost_flash)
        snprintf(lost, sizeof lost, ", and %d it lost when moving them to flash (their text was never written: "
                 "only their names are left)", r.lost_flash);
    snprintf(before, sizeof before, "%s.before-setup", st);
    snprintf(msg, sizeof msg, "This %s was never set up: its file system and folders were not made when it first "
             "started (the 0.6 and 0.7 previews' first start missed the unit's cold reset; or a setup question was "
             "answered n). On it a new file can lose its first letter, and a file moved to flash is lost.\n\n"
             "Its files: %d in RAM%s.\n\n"
             "Yes: set it up now and keep its RAM files (in its RAM startup folder). Its settings go back to the "
             "factory's.\n"
             "No: start it from the factory state: it asks its own setup questions, and its files are not kept.\n"
             "Cancel: start it as it is.\n\n"
             "Yes and No keep the old memory as %s.", KINDS[kind].name, r.ram_files, lost, before);
    answer = MessageBoxA(g_wnd, msg, "Type 'n Speak not set up", MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDNO) {
        if (!CopyFileA(st, before, FALSE)) {
            MessageBoxA(g_wnd, "Could not keep the old memory; the unit starts as it is.", "Blazie emulator",
                        MB_OK | MB_ICONERROR);
            return 1;
        }
        DeleteFileA(st);
        return 0;
    }
    if (answer != IDYES)
        return 1;
    status("Setting up the Type 'n Speak (a few seconds)");
    if (!tns_rescue(fw, st, 1, &r, err, sizeof err)) {
        snprintf(msg, sizeof msg, "Could not set the unit up: %s\n\nIt starts as it is.", err);
        MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONERROR);
        return 1;
    }
    snprintf(msg, sizeof msg, "The %s is set up: %d files kept%s. The old memory is in %s.\n\n%s", KINDS[kind].name,
             r.carried, r.first_lost ? " (one had lost its first letter on the old unit)" : "", before, r.log);
    MessageBoxA(g_wnd, msg, "Type 'n Speak set up", MB_OK | MB_ICONINFORMATION);
    return 1;
}

static int start_unit(int kind)
{
    char fw[MAX_PATH], st[MAX_PATH], err[256];
    const char *state = st;
    emu_unit *u;
    save_unit();                            /* first: the unit being left keeps its memory (it may be this kind) */
    snprintf(fw, sizeof fw, "%s\\%s", g_fw_dir, KINDS[kind].firmware);
    saved_path(kind, st, sizeof st);
    if (exists(st) && KINDS[kind].kind == EMU_TYPE_N_SPEAK && !offer_setup(kind, fw, st))
        state = NULL;                       /* the factory state: the unit's cold reset, its own questions */
    else if (!exists(st)) {                 /* the first time: the unit as it left the factory */
        if (KINDS[kind].state)
            snprintf(st, sizeof st, "%s\\%s", g_fw_dir, KINDS[kind].state);
        else
            state = NULL;                   /* a cold start */
    }
    if (!state && KINDS[kind].kind == EMU_TYPE_N_SPEAK)
        MessageBoxA(g_wnd, TNS_FIRST_START, KINDS[kind].name, MB_OK | MB_ICONINFORMATION);
    status("Starting");
    u = emu_create(KINDS[kind].kind, fw, state, g_rate, g_whine, err, sizeof err);
    if (!u) {
        char msg[700];
        snprintf(msg, sizeof msg, "Could not start the %s.\n\n%s\n\nFirmware looked for in:\n%s", KINDS[kind].name,
                 err, g_fw_dir);
        MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONERROR);
        return 0;
    }
    if (g_link)
        emu_serial_attach(u, 1);            /* the new unit takes the old one's place on the COM port */
    emu_set_idle(u, g_whine, g_keep_open, g_popclick, g_tick);
    emu_set_quick(u, g_quick);
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

/* ---- files in and out: the unit's files as a FAT disk image (../../csrc/blazie/bl_files_xfer.h) --------------- */
static void tns_release_all(void);

static int image_dialog(int save, char *path, int cap)
{
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner = g_wnd;
    o.lpstrFilter = "Disk images (*.img)\0*.img\0All files (*.*)\0*.*\0";
    o.lpstrFile = path;
    o.nMaxFile = (DWORD)cap;
    o.lpstrDefExt = "img";
    o.lpstrTitle = save ? "Export the unit's files to a disk image" : "Import files from a disk image";
    o.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameA(&o) : GetOpenFileNameA(&o);
}

/* the report's first lines, after a summary, for a message box a screen reader reads out */
static void report_text(char *out, size_t cap, const char *summary, const blx_report *r)
{
    const char *p = r->log ? r->log : "";
    int lines = 0;
    size_t n;
    snprintf(out, cap, "%s", summary);
    n = strlen(out);
    while (*p && lines < 25 && n + 4 < cap) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (n + len + 3 >= cap) break;
        out[n++] = '\n';
        memcpy(out + n, p, len);
        n += len;
        out[n] = 0;
        lines++;
        p = e ? e + 1 : p + len;
    }
    if (*p && n + 8 < cap)
        snprintf(out + n, cap - n, "\n...");
}

/* the state as saved, opened for bl_files; 0 with a message box */
static int open_saved(const char *st, bls_unit *u, blf_fs **fs)
{
    char err[300], msg[700];
    if (!bls_load(st, u, err, sizeof err)) {
        snprintf(msg, sizeof msg, "Could not read the unit's memory (%s): %s", st, err);
        MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONERROR);
        return 0;
    }
    *fs = blf_open(u->model, u->ram, u->flash, u->flash_size, err, sizeof err);
    if (!*fs) {
        bls_free(u);
        snprintf(msg, sizeof msg, "Could not read the unit's files: %s", err);
        MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONERROR);
        return 0;
    }
    return 1;
}

static int write_bytes(const char *path, const unsigned char *d, unsigned long n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return 0;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static unsigned char *read_bytes(const char *path, unsigned long *n)
{
    FILE *f = fopen(path, "rb");
    long size;
    unsigned char *d;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = size > 0 ? (unsigned char *)malloc((size_t)size) : NULL;
    if (d && fread(d, 1, (size_t)size, f) != (size_t)size) { free(d); d = NULL; }
    fclose(f);
    *n = (unsigned long)size;
    return d;
}

/* every file the unit holds, as it holds them now (the unit runs on) */
static void export_files(void)
{
    char path[MAX_PATH], st[MAX_PATH], err[300], summary[600], msg[4000];
    bls_unit u;
    blf_fs *fs;
    blx_report r;
    unsigned char *img;
    unsigned long size;
    if (!g_unit) return;
    tns_release_all();
    snprintf(path, sizeof path, "%s files.img", KINDS[g_kind].name);
    if (!image_dialog(1, path, sizeof path)) return;
    save_unit();                            /* its memory as it is this moment */
    saved_path(g_kind, st, sizeof st);
    if (!open_saved(st, &u, &fs)) return;
    blx_report_init(&r);
    img = blx_export(fs, u.model, &size, &r, err, sizeof err);
    if (!img || !write_bytes(path, img, size)) {
        snprintf(msg, sizeof msg, "Could not export the files: %s", img ? "the image could not be written" : err);
        MessageBoxA(g_wnd, msg, "Export files", MB_OK | MB_ICONERROR);
    } else {
        snprintf(summary, sizeof summary, "Exported %d files to %s.\n\nEach of the unit's folders is a folder in "
                 "the image; 7-Zip opens it. Text files keep the unit's line ends (a lone carriage return).",
                 r.exported, path);
        report_text(msg, sizeof msg, summary, &r);
        MessageBoxA(g_wnd, msg, "Export files", MB_OK | MB_ICONINFORMATION);
    }
    free(img);
    blx_report_free(&r);
    blf_close(fs);
    bls_free(&u);
}

/* the image's files into the unit: only while it is not writing its flash; it is switched off, its files changed,
   and switched on again -- the firmware then finds them as if it had written them */
static void import_files(void)
{
    char path[MAX_PATH] = "", st[MAX_PATH], err[300], summary[1200], msg[4000];
    unsigned char *img;
    unsigned long size;
    fat_volume *v;
    emu_unit *old;
    bls_unit u;
    blf_fs *fs;
    blx_report r;
    int busy, ok, cancelled = 0;
    if (!g_unit) return;
    tns_release_all();
    if (!image_dialog(0, path, sizeof path)) return;
    img = read_bytes(path, &size);
    v = img ? fat_read(img, size, err, sizeof err) : NULL;
    if (!v) {
        snprintf(msg, sizeof msg, "%s is not a disk image the emulator can read: %s", path, img ? err : "unreadable");
        MessageBoxA(g_wnd, msg, "Import files", MB_OK | MB_ICONERROR);
        free(img);
        return;
    }
    fat_close(v);
    CreateDirectoryA(g_save_dir, NULL);
    saved_path(g_kind, st, sizeof st);
    EnterCriticalSection(&g_lock);
    busy = emu_flash(g_unit, NULL);
    ok = !busy && emu_save(g_unit, st);
    old = ok ? g_unit : NULL;
    if (ok)
        g_unit = NULL;                      /* switched off: nothing runs while its memory changes */
    LeaveCriticalSection(&g_lock);
    if (!ok) {
        MessageBoxA(g_wnd, busy ? "The unit is writing its flash memory. Import the files when it has finished "
                    "(when it is quiet)." : "Could not save the unit's memory; nothing was imported.",
                    "Import files", MB_OK | MB_ICONWARNING);
        free(img);
        return;
    }
    emu_destroy(old);
    blx_report_init(&r);
    ok = open_saved(st, &u, &fs);
    if (ok) {
        ok = blx_import(fs, img, size, &r, err, sizeof err) && blf_check(fs, err, sizeof err);
        if (ok && r.deleted) {              /* an image holding only new files would empty the unit: ask */
            char ask[3000];
            const char *line = r.log ? r.log : "";
            size_t n = (size_t)snprintf(ask, sizeof ask, "The image does not have %d of the unit's files. Importing "
                                        "it deletes them from the unit:\n", r.deleted);
            while (*line && n + 80 < sizeof ask) {   /* the report's "deleted NAME" lines */
                const char *e = strchr(line, '\n');
                size_t len = e ? (size_t)(e - line) : strlen(line);
                if (len > 8 && !strncmp(line, "deleted ", 8))
                    n += (size_t)snprintf(ask + n, sizeof ask - n, "\n%.*s", (int)(len - 8), line + 8);
                line = e ? e + 1 : line + len;
            }
            snprintf(ask + n, sizeof ask - n, "\n\nDelete them and import? No imports nothing.");
            if (MessageBoxA(g_wnd, ask, "Import files", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                ok = 0, cancelled = 1;
        }
        if (ok) {                           /* the unit as it was, kept beside it: undone by one file copy */
            char before[MAX_PATH + 20];
            snprintf(before, sizeof before, "%s.before-import", st);
            CopyFileA(st, before, FALSE);
            ok = bls_save(st, &u, err, sizeof err);
        }
        blf_close(fs);
        bls_free(&u);
    }
    free(img);
    start_unit(g_kind);                     /* switched on again */
    if (cancelled)
        MessageBoxA(g_wnd, "Nothing was imported. The unit's files are as they were.", "Import files",
                    MB_OK | MB_ICONINFORMATION);
    else if (!ok) {
        snprintf(summary, sizeof summary, "Nothing was imported: %s\n\nThe unit was restarted with its files as "
                 "they were.", err);
        report_text(msg, sizeof msg, summary, &r);
        MessageBoxA(g_wnd, msg, "Import files", MB_OK | MB_ICONERROR);
    } else {
        snprintf(summary, sizeof summary, "Imported from %s: %d added, %d rewritten, %d moved, %d deleted, %d "
                 "unchanged, %d kept, %d skipped%s. The unit was restarted. (The unit as it was before is kept in "
                 "%s.before-import.)", path, r.added, r.replaced, r.moved, r.deleted, r.unchanged, r.kept, r.skipped,
                 r.folders_added ? ", new folders made" : "", st);
        report_text(msg, sizeof msg, summary, &r);
        MessageBoxA(g_wnd, msg, "Import files", MB_OK | (r.skipped || r.kept ? MB_ICONWARNING : MB_ICONINFORMATION));
    }
    blx_report_free(&r);
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
                emu_render(g_unit, g_buf[i], g_rate * g_block_ms / 1000);
            else
                memset(g_buf[i], 0, sizeof g_buf[i]);
            com_kick(g_link);               /* the unit ran: its serial bytes may be waiting, both ways */
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
    f.nSamplesPerSec = g_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = g_rate * 2;
    g_wave_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &f, (DWORD_PTR)g_wave_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return 0;
    for (i = 0; i < NBLOCKS; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = (DWORD)(sizeof(short) * (g_rate * g_block_ms / 1000));
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

static void check_rate(void)
{
    int k;
    for (k = 0; k < N_RATES; k++)
        if (RATES[k] == g_rate)
            CheckMenuRadioItem(GetMenu(g_wnd), ID_RATE, ID_RATE + N_RATES - 1, ID_RATE + k, MF_BYCOMMAND);
}

/* a new sample rate: the sound card reopened at it, and the unit restarted to render at it (its memory kept) */
static void set_rate(int r)
{
    char v[16];
    if (r == g_rate)
        return;
    close_audio();
    CloseHandle(g_thread);
    CloseHandle(g_wave_event);
    g_rate = r;
    InterlockedExchange(&g_quit, 0);
    start_unit(g_kind);
    if (!open_audio())
        MessageBoxA(g_wnd, "Could not open the sound card at that rate.", "Blazie emulator", MB_OK | MB_ICONERROR);
    check_rate();
    snprintf(v, sizeof v, "%d", r);
    WritePrivateProfileStringA("sound", "rate", v, g_ini);
}

/* ---- the window ------------------------------------------------------------------------------------------------ */
static const char *const IDLE_NAMES[] = {"off", "hiss", "whine", "unit"};
static const int IDLE_IDS[] = {ID_QUIET, ID_HISS, ID_WHINE, ID_UNITSOUND};
static const char *const OPEN_NAMES[] = {"off", "until", "always"};

/* the idle channel's settings to the unit (bl_idle.h: the sound, when it is heard, the pop and click, the tick), the
   menu's marks and the settings file */
static void apply_idle(void)
{
    HMENU m = GetMenu(g_wnd);
    EnterCriticalSection(&g_lock);
    if (g_unit)
        emu_set_idle(g_unit, g_whine, g_keep_open, g_popclick, g_tick);
    LeaveCriticalSection(&g_lock);
    CheckMenuRadioItem(m, ID_HISS, ID_UNITSOUND, IDLE_IDS[g_whine], MF_BYCOMMAND);
    CheckMenuRadioItem(m, ID_OPEN_OFF, ID_OPEN_ALWAYS, ID_OPEN_OFF + g_keep_open, MF_BYCOMMAND);
    CheckMenuItem(m, ID_POPCLICK, MF_BYCOMMAND | (g_popclick ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_TICK, MF_BYCOMMAND | (g_tick ? MF_CHECKED : MF_UNCHECKED));
    WritePrivateProfileStringA("sound", "idle", IDLE_NAMES[g_whine], g_ini);
    WritePrivateProfileStringA("sound", "keep_open", OPEN_NAMES[g_keep_open], g_ini);
    WritePrivateProfileStringA("sound", "pop_click", g_popclick ? "1" : "0", g_ini);
    WritePrivateProfileStringA("sound", "tick", g_tick ? "1" : "0", g_ini);
}

static void load_idle(void)
{
    char v[32];
    int k;
    GetPrivateProfileStringA("sound", "idle", "unit", v, sizeof v, g_ini);
    for (k = 0; k < 4; k++)
        if (!strcmp(v, IDLE_NAMES[k]))
            g_whine = k;
    GetPrivateProfileStringA("sound", "keep_open", "until", v, sizeof v, g_ini);
    for (k = 0; k < 3; k++)
        if (!strcmp(v, OPEN_NAMES[k]))
            g_keep_open = k;
    g_popclick = GetPrivateProfileIntA("sound", "pop_click", 1, g_ini) != 0;
    g_tick = GetPrivateProfileIntA("sound", "tick", 1, g_ini) != 0;
}

/* ---- the serial port ------------------------------------------------------------------------------------------ */
/* plugs the unit's serial port into COM port `name` ("" = none, unplugged); 1 on success.  at_start: the port saved
   last time -- if it is gone, the unit starts unplugged and the setting stays for next time */
static int set_serial(const char *name, int at_start)
{
    char err[300], msg[600];
    com_link *old = g_link, *link = NULL;
    EnterCriticalSection(&g_lock);
    g_link = NULL;                          /* the sound thread stops kicking it */
    LeaveCriticalSection(&g_lock);
    com_close(old);                         /* (its thread takes the lock: never hold it here) */
    EnterCriticalSection(&g_lock);
    if (g_unit)
        emu_serial_attach(g_unit, 0);
    LeaveCriticalSection(&g_lock);
    g_com[0] = 0;
    if (name[0]) {
        EnterCriticalSection(&g_lock);
        if (g_unit)
            emu_serial_attach(g_unit, 1);   /* first, so the port's first status is the unit's */
        LeaveCriticalSection(&g_lock);
        link = com_open(name, &g_lock, &g_unit, err, sizeof err);
        if (!link) {
            EnterCriticalSection(&g_lock);
            if (g_unit)
                emu_serial_attach(g_unit, 0);
            LeaveCriticalSection(&g_lock);
            snprintf(msg, sizeof msg, "Could not open %s. %s\n\nThe unit's serial port is not connected.%s", name,
                     err, at_start ? " Settings, Serial port chooses another port." : "");
            MessageBoxA(g_wnd, msg, "Blazie emulator", MB_OK | MB_ICONWARNING);
        } else {
            snprintf(g_com, sizeof g_com, "%s", name);
            EnterCriticalSection(&g_lock);
            g_link = link;
            LeaveCriticalSection(&g_lock);
            com_kick(link);
        }
    }
    if (!at_start || link)
        WritePrivateProfileStringA("serial", "port", link ? name : "none", g_ini);
    if (g_unit)
        status(KINDS[g_kind].name);
    return link != NULL;
}

/* the Serial port menu, listed afresh each time it opens (a com0com pair made meanwhile shows up) */
static void fill_serial_menu(void)
{
    int k, current = -1;
    while (GetMenuItemCount(g_serial_menu) > 0)
        DeleteMenu(g_serial_menu, 0, MF_BYPOSITION);
    g_n_ports = com_list(g_ports, COM_MAX);
    AppendMenuA(g_serial_menu, MF_STRING, ID_SERIAL_NONE, "&None (not connected)");
    for (k = 0; k < g_n_ports; k++) {
        AppendMenuA(g_serial_menu, MF_STRING, ID_SERIAL_PORT + k, g_ports[k].label);
        if (g_link && !_stricmp(g_ports[k].name, g_com))
            current = k;
    }
    if (!g_n_ports)
        AppendMenuA(g_serial_menu, MF_STRING | MF_GRAYED, ID_SERIAL_PORT + COM_MAX, "No COM ports found");
    CheckMenuRadioItem(g_serial_menu, ID_SERIAL_NONE, ID_SERIAL_PORT + COM_MAX, current < 0 ? ID_SERIAL_NONE
                       : ID_SERIAL_PORT + current, MF_BYCOMMAND);
}

static HMENU make_menu(void)
{
    HMENU bar = CreateMenu(), unit = CreatePopupMenu(), sound = CreatePopupMenu(), help = CreatePopupMenu();
    HMENU rates = CreatePopupMenu();
    int k;
    AppendMenuA(unit, MF_STRING, ID_EN, "Braille Lite 2000, &English");
    AppendMenuA(unit, MF_STRING, ID_ES, "Braille Lite 2000, &Spanish");
    AppendMenuA(unit, MF_STRING, ID_TNS_EN, "&Type 'n Speak, English");
    AppendMenuA(unit, MF_STRING, ID_TNS_ES, "Type 'n Speak, S&panish");
    AppendMenuA(unit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(unit, MF_STRING, ID_EXPORT, "Exp&ort files to disk image (.img)...");
    AppendMenuA(unit, MF_STRING, ID_IMPORT, "&Import files from disk image (.img)...");
    AppendMenuA(unit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(unit, MF_STRING, ID_FACTORY, "Back to the &factory state (erases this unit's files)...");
    AppendMenuA(unit, MF_STRING, ID_EXIT, "E&xit");
    AppendMenuA(sound, MF_STRING, ID_UNITSOUND, "Idle channel: as the &unit (hiss at even volumes, whine at odd)");
    AppendMenuA(sound, MF_STRING, ID_HISS, "Idle channel: &hiss");
    AppendMenuA(sound, MF_STRING, ID_WHINE, "Idle channel: &whine");
    AppendMenuA(sound, MF_STRING, ID_QUIET, "Idle channel: &silent");
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    AppendMenuA(sound, MF_STRING, ID_OPEN_OFF, "Keep the channel open: &off (silent as soon as speech ends)");
    AppendMenuA(sound, MF_STRING, ID_OPEN_UNTIL, "Keep the channel open: until the unit &clicks off");
    AppendMenuA(sound, MF_STRING, ID_OPEN_ALWAYS, "Keep the channel open: &always");
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    AppendMenuA(sound, MF_STRING, ID_POPCLICK, "The &pop when the channel opens, the click when it clicks off");
    AppendMenuA(sound, MF_STRING, ID_TICK, "The 10 Hz &tick of the open channel");
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    AppendMenuA(sound, MF_STRING, ID_QUICK, "&Quick key response (faster than the real unit)");
    AppendMenuA(help, MF_STRING, ID_KEYS, "&Keys");
    AppendMenuA(help, MF_STRING, ID_ABOUT, "&About");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)unit, "&Firmware");
    for (k = 0; k < N_RATES; k++) {
        char label[32];
        snprintf(label, sizeof label, "%d Hz", RATES[k]);
        AppendMenuA(rates, MF_STRING, ID_RATE + k, label);
    }
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    AppendMenuA(sound, MF_POPUP, (UINT_PTR)rates, "Sample &rate");
    g_serial_menu = CreatePopupMenu();
    AppendMenuA(g_serial_menu, MF_STRING, ID_SERIAL_NONE, "&None (not connected)");   /* filled when it opens */
    AppendMenuA(sound, MF_POPUP, (UINT_PTR)g_serial_menu, "Serial &port");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)sound, "S&ettings");
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
    /* Alt+Shift+F always opens this program's menu, whatever unit has the keyboard (the Type 'n Speak takes Alt for
       itself); the unit is told its held keys came up */
    if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && wp == 'F' && (GetKeyState(VK_MENU) & 0x8000)
            && (GetKeyState(VK_SHIFT) & 0x8000)) {
        tns_release_all();
        PostMessageA(w, WM_SYSCOMMAND, SC_KEYMENU, 0);
        return 0;
    }
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
            if (g_unit)
                emu_keys_down(g_unit, g_chord.down);   /* held: the unit sees them as it starts (i-chord: cold reset) */
            LeaveCriticalSection(&g_lock);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wp < 256 && g_keymap[wp]) {
            int chord;
            EnterCriticalSection(&g_lock);
            chord = chord_up(&g_chord, g_keymap[wp]);
            if (g_unit)
                emu_keys_down(g_unit, g_chord.down);
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
        if (g_unit)
            emu_keys_down(g_unit, 0);
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
        case ID_EXPORT: export_files(); return 0;
        case ID_IMPORT: import_files(); return 0;
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
        case ID_HISS: g_whine = 1; apply_idle(); return 0;
        case ID_WHINE: g_whine = 2; apply_idle(); return 0;
        case ID_QUIET: g_whine = 0; apply_idle(); return 0;
        case ID_UNITSOUND: g_whine = 3; apply_idle(); return 0;
        case ID_OPEN_OFF: case ID_OPEN_UNTIL: case ID_OPEN_ALWAYS:
            g_keep_open = LOWORD(wp) - ID_OPEN_OFF;
            apply_idle();
            return 0;
        case ID_POPCLICK: g_popclick = !g_popclick; apply_idle(); return 0;
        case ID_TICK: g_tick = !g_tick; apply_idle(); return 0;
        case ID_SERIAL_NONE: set_serial("", 0); return 0;
        case ID_QUICK:
            g_quick = !g_quick;
            EnterCriticalSection(&g_lock);
            if (g_unit)
                emu_set_quick(g_unit, g_quick);
            LeaveCriticalSection(&g_lock);
            CheckMenuItem(GetMenu(w), ID_QUICK, MF_BYCOMMAND | (g_quick ? MF_CHECKED : MF_UNCHECKED));
            WritePrivateProfileStringA("unit", "quick_keys", g_quick ? "1" : "0", g_ini);
            return 0;
        default:
            if (LOWORD(wp) >= ID_RATE && LOWORD(wp) < ID_RATE + N_RATES) {
                set_rate(RATES[LOWORD(wp) - ID_RATE]);
                return 0;
            }
            if (LOWORD(wp) >= ID_SERIAL_PORT && LOWORD(wp) < ID_SERIAL_PORT + g_n_ports) {
                char name[16];
                snprintf(name, sizeof name, "%s", g_ports[LOWORD(wp) - ID_SERIAL_PORT].name);
                set_serial(name, 0);
                return 0;
            }
            break;
        case ID_KEYS:
            MessageBoxA(w, "Braille Lite, while this window is in front:\n\n"
                        "F D S = dots 1 2 3\nJ K L = dots 4 5 6\nSpace bar = space\nA or ; = advance bar\n\n"
                        "Press the keys of a chord together; it goes to the unit when you let go of them.\n"
                        "Keys held while the unit starts are read as it starts: p-chord, l restarts it; "
                        "hold i-chord at once for the cold reset.\n"
                        "The keys can be changed in blazie_emu.ini, section [keys].\n"
                        "Alt opens this program's menu; Alt+F4 closes it.\n\n"
                        "Type 'n Speak: the whole keyboard is the unit's, Alt and the function keys included.\n"
                        "Alt+Shift+F (or F11) opens this program's menu (Firmware > Exit closes it).\n\n"
                        TNS_FIRST_START, "Keys", MB_OK);
            return 0;
        case ID_ABOUT:
            MessageBoxA(w, "Blazie emulator, part of ssi263-speech.\n\n"
                        "It runs Blazie Engineering's own firmware on an emulated board with an emulated SSI-263 "
                        "speech chip. The firmware is shared with permission. This program is not a product of "
                        "Blazie Engineering.\n\n"
                        "Licence: MIT (see LICENSE beside the program), with Casso's MIT notice, which the chip "
                        "model draws on. The Z180 core is MAME's and keeps its BSD-3-Clause licence. Both "
                        "notices are in the licenses folder.", "About", MB_OK);
            return 0;
        }
        break;
    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_serial_menu)
            fill_serial_menu();
        break;
    case WM_TIMER:                          /* the unit's memory saved every minute: nothing is lost if the */
        save_unit();                        /* program is ended without closing its window */
        return 0;
    case WM_QUERYENDSESSION:
        save_unit();                        /* Windows logging off or shutting down */
        return TRUE;
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
    SetTimer(g_wnd, 1, 60000, NULL);
    load_idle();
    apply_idle();
    g_block_ms = GetPrivateProfileIntA("sound", "block_ms", BLOCK_MS_DEFAULT, g_ini);
    if (g_block_ms < 5 || g_block_ms > 20)
        g_block_ms = BLOCK_MS_DEFAULT;
    g_quick = GetPrivateProfileIntA("unit", "quick_keys", 0, g_ini) != 0;
    CheckMenuItem(GetMenu(g_wnd), ID_QUICK, MF_BYCOMMAND | (g_quick ? MF_CHECKED : MF_UNCHECKED));
    {
        int k, r = GetPrivateProfileIntA("sound", "rate", 44100, g_ini);
        for (k = 0; k < N_RATES; k++)
            if (RATES[k] == r)
                g_rate = r;
        check_rate();
    }
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
    GetPrivateProfileStringA("serial", "port", "none", v, sizeof v, g_ini);
    if (_stricmp(v, "none") && v[0])
        set_serial(v, 1);                   /* the COM port chosen last time */
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    set_serial("", 1);                      /* unplugged: the port closed (the setting kept) */
    close_audio();
    save_unit();                            /* switched off: the memory is kept */
    emu_destroy(g_unit);
    return 0;
}
