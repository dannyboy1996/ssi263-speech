/* test_serial_win.c -- the Windows side of the serial port (serial_win.c) end to end, without a COM port: its thread
 * opens a named pipe in place of \\.\COMn (the port calls -- SetCommState and the rest -- fail there and are skipped;
 * the overlapped reads and writes, the kicks and the lock are the app's own), and this program is the far end.
 *
 * The unit boots (faster than real time), then runs in real time as the app's sound thread runs it: 5 ms per
 * render under the shell's lock, a kick after each.  The storage key: the far end must see XON ENQ, answers ACK,
 * and must get 'C' back.  Built against the board with the receive path cut (test_serial_win_cut.exe), "ACK
 * answered" must FAIL.
 *
 *   test_serial_win FIRMWARE STATE    (the Braille Lite)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serial_win.h"

#define RATE 11025
#define BLOCK (RATE / 200)

static CRITICAL_SECTION lock;
static emu_unit *unit;
static com_link *link;
static HANDLE far_end;
static unsigned char heard[256];
static int n_heard, failures;

static void check(const char *name, int ok, const char *detail)
{
    printf("%-4s %-26s %s\n", ok ? "ok" : "FAIL", name, detail);
    failures += !ok;
}

static void listen_far_end(void)
{
    DWORD avail = 0, got = 0;
    if (PeekNamedPipe(far_end, NULL, 0, NULL, &avail, NULL) && avail) {
        if (avail > sizeof heard - (DWORD)n_heard)
            avail = sizeof heard - (DWORD)n_heard;
        if (avail && ReadFile(far_end, heard + n_heard, avail, &got, NULL))
            n_heard += (int)got;
    }
}

/* `seconds` of the unit; real time: 5 ms per render, as the sound card paces it */
static void run(double seconds, int real_time)
{
    static short buf[BLOCK];
    int i, n = (int)(seconds * 200);
    for (i = 0; i < n; i++) {
        EnterCriticalSection(&lock);
        emu_render(unit, buf, BLOCK);
        com_kick(link);
        LeaveCriticalSection(&lock);
        if (real_time)
            Sleep(5);
        listen_far_end();
    }
}

static int find(unsigned char byte, int from)
{
    int i;
    for (i = from; i < n_heard; i++)
        if (heard[i] == byte)
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    char name[64], path[80], err[300], d[200];
    int i, enq = -1, c = -1;
    if (argc < 3) {
        printf("usage: test_serial_win FIRMWARE STATE\n");
        return 2;
    }
    InitializeCriticalSection(&lock);
    unit = emu_create(EMU_BRAILLE_LITE, argv[1], argv[2], RATE, 0, err, sizeof err);
    if (!unit) {
        printf("FAIL create: %s\n", err);
        return 1;
    }
    snprintf(name, sizeof name, "pipe\\blazie_serial_test_%lu", (unsigned long)GetCurrentProcessId());
    snprintf(path, sizeof path, "\\\\.\\%s", name);
    far_end = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096,
                               4096, 0, NULL);
    if (far_end == INVALID_HANDLE_VALUE) {
        printf("FAIL the far end's pipe: error %lu\n", GetLastError());
        return 1;
    }
    emu_serial_attach(unit, 1);
    link = com_open(name, &lock, &unit, err, sizeof err);
    check("open", link != NULL, link ? name : err);
    if (!link)
        return 1;
    run(8.0, 0);                            /* the greeting */
    EnterCriticalSection(&lock);
    emu_key(unit, 0x4E);                    /* s-chord: storage */
    LeaveCriticalSection(&lock);
    for (i = 0; i < 200 && enq < 0; i++) {  /* up to a second */
        run(0.005, 1);
        enq = find(0x05, 0);
    }
    snprintf(d, sizeof d, "%d bytes heard, ENQ %s", n_heard, enq >= 0 ? "among them" : "not");
    check("XON ENQ reach the far end", enq == 1 && heard[0] == 0x11, d);
    if (enq >= 0) {
        DWORD put;
        WriteFile(far_end, "\x06", 1, &put, NULL);
        for (i = 0; i < 200 && c < 0; i++) {
            run(0.005, 1);
            c = find(0x43, enq + 1);
        }
    }
    snprintf(d, sizeof d, "ACK -> %s", c >= 0 ? "'C' back" : "nothing back");
    check("ACK answered", c >= 0, d);
    com_close(link);
    CloseHandle(far_end);
    emu_destroy(unit);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
