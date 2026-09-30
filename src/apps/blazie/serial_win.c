/* serial_win.c -- see serial_win.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "serial_win.h"
#include <setupapi.h>

/* ---- the list ------------------------------------------------------------------------------------------------ */
static int port_number(const char *name)
{
    return (!_strnicmp(name, "COM", 3)) ? atoi(name + 3) : 0;
}

static int by_number(const void *a, const void *b)
{
    return port_number(((const com_port_info *)a)->name) - port_number(((const com_port_info *)b)->name);
}

typedef struct { char port[16], name[128]; } port_name;

/* the devices' own names for their ports, from Device Manager ("Prolific USB-to-Serial Comm Port (COM6)"), without
   the trailing "(COM6)": the port classes (Ports; com0com's CNCPorts; Modem, Bluetooth's) in one pass */
static int friendly_names(port_name *out, int max)
{
    HDEVINFO set = SetupDiGetClassDevsA(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dev;
    DWORD i;
    int n = 0;
    if (set == INVALID_HANDLE_VALUE)
        return 0;
    dev.cbSize = sizeof dev;
    for (i = 0; n < max && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
        char cls[64], *nm = out[n].name, *paren;
        DWORD type, size = sizeof out[n].port;
        HKEY key;
        LONG r;
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_CLASS, NULL, (PBYTE)cls, sizeof cls, NULL))
            continue;
        if (_stricmp(cls, "Ports") && _stricmp(cls, "CNCPorts") && _stricmp(cls, "Modem"))
            continue;
        key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        r = RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)out[n].port, &size);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || type != REG_SZ)
            continue;
        out[n].port[sizeof out[n].port - 1] = 0;
        if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_FRIENDLYNAME, NULL, (PBYTE)nm, sizeof out[n].name, NULL)
                && !SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_DEVICEDESC, NULL, (PBYTE)nm, sizeof out[n].name,
                                                      NULL))
            continue;
        nm[sizeof out[n].name - 1] = 0;
        paren = strrchr(nm, '(');
        if (paren && paren > nm && !_strnicmp(paren + 1, out[n].port, strlen(out[n].port))) {
            while (paren > nm && paren[-1] == ' ')
                paren--;
            *paren = 0;
        }
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int com_list(com_port_info *ports, int max)
{
    static port_name names[COM_MAX];
    HKEY key;
    DWORD i;
    int n = 0, n_names;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    n_names = friendly_names(names, COM_MAX);
    for (i = 0; n < max; i++) {
        char dev[256], name[64], nice[128];
        DWORD dev_len = sizeof dev, name_len = sizeof name, type;
        const char *tail;
        int k;
        LONG r = RegEnumValueA(key, i, dev, &dev_len, NULL, &type, (LPBYTE)name, &name_len);
        if (r == ERROR_NO_MORE_ITEMS)
            break;
        if (r != ERROR_SUCCESS || type != REG_SZ || strlen(name) >= sizeof ports[n].name)
            continue;
        snprintf(ports[n].name, sizeof ports[n].name, "%s", name);
        nice[0] = 0;
        for (k = 0; k < n_names; k++)
            if (!_stricmp(names[k].port, name))
                snprintf(nice, sizeof nice, "%s", names[k].name);
        if (!nice[0]) {                     /* the kernel's device name: "\Device\com0com10" -> "com0com10" */
            tail = strrchr(dev, '\\');
            snprintf(nice, sizeof nice, "%s", tail ? tail + 1 : dev);
        }
        k = snprintf(ports[n].label, sizeof ports[n].label, "%s, ", name);
        for (tail = nice; *tail && k + 2 < (int)sizeof ports[n].label; tail++) {   /* & would underline a letter */
            if (*tail == '&')
                ports[n].label[k++] = '&';
            ports[n].label[k++] = *tail;
        }
        ports[n].label[k] = 0;
        n++;
    }
    RegCloseKey(key);
    qsort(ports, (size_t)n, sizeof *ports, by_number);
    return n;
}

/* ---- the link ------------------------------------------------------------------------------------------------ */
#define OUT_CAP 1024

struct com_link {
    HANDLE port, thread, quit, kick;
    CRITICAL_SECTION *lock;
    emu_unit **unit;
    OVERLAPPED ro, wo;
    int reading, writing;
    unsigned char in[BL_SERIAL_ROOM];      /* read from the port, not yet taken by the unit */
    int n_in;
    unsigned char out[OUT_CAP];            /* the unit's bytes being written, all of one status */
    int n_out;
    bl_serial_status out_st, applied;
    int have_applied;
};

static int same_format(const bl_serial_status *a, const bl_serial_status *b)
{
    return a->baud == b->baud && a->data_bits == b->data_bits && a->parity == b->parity && a->stop_bits == b->stop_bits;
}

/* the port set to the unit's line status: the format (after what was sent under the old one has left), DTR = the
   unit's port on, RTS = /RTS0 asserted while on */
static void apply(com_link *c, const bl_serial_status *st)
{
    int dtr = st->powered, rts = st->powered && st->rts;
    if (c->have_applied && bl_serial_same(&c->applied, st))
        return;
    if (!c->have_applied || !same_format(&c->applied, st)) {
        DCB dcb;
        memset(&dcb, 0, sizeof dcb);
        dcb.DCBlength = sizeof dcb;
        FlushFileBuffers(c->port);         /* the bytes of the old format leave first */
        if (GetCommState(c->port, &dcb)) {
            if (st->baud > 0)
                dcb.BaudRate = (DWORD)st->baud;
            dcb.ByteSize = (BYTE)st->data_bits;
            dcb.Parity = st->parity == 'E' ? EVENPARITY : st->parity == 'O' ? ODDPARITY : NOPARITY;
            dcb.fParity = st->parity != 'N';
            dcb.StopBits = st->stop_bits == 2 ? TWOSTOPBITS : ONESTOPBIT;
            dcb.fBinary = TRUE;
            dcb.fOutxCtsFlow = FALSE;
            dcb.fOutxDsrFlow = FALSE;
            dcb.fDsrSensitivity = FALSE;
            dcb.fOutX = FALSE;             /* the unit's XON/XOFF are bytes for the far end */
            dcb.fInX = FALSE;
            dcb.fErrorChar = FALSE;
            dcb.fNull = FALSE;
            dcb.fAbortOnError = FALSE;
            dcb.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
            dcb.fRtsControl = rts ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
            SetCommState(c->port, &dcb);   /* a rate the port cannot do leaves it as it was */
        }
    } else {
        EscapeCommFunction(c->port, dtr ? SETDTR : CLRDTR);
        EscapeCommFunction(c->port, rts ? SETRTS : CLRRTS);
    }
    c->applied = *st;
    c->have_applied = 1;
}

/* one round: finished I/O collected, bytes exchanged with the unit, new I/O started; 1 if anything moved */
static int round_trip(com_link *c)
{
    DWORD got;
    int moved = 0, space = 0, fetched = 0;
    bl_serial_status st;
    if (c->reading && GetOverlappedResult(c->port, &c->ro, &got, FALSE)) {
        c->reading = 0;
        c->n_in += (int)got;
        moved |= got > 0;
    } else if (c->reading && GetLastError() != ERROR_IO_INCOMPLETE) {
        DWORD errors;
        c->reading = 0;                    /* a line error: clear it and read on */
        ClearCommError(c->port, &errors, NULL);
    }
    if (c->writing && (GetOverlappedResult(c->port, &c->wo, &got, FALSE) || GetLastError() != ERROR_IO_INCOMPLETE)) {
        c->writing = 0;
        c->n_out = 0;
        moved = 1;
    }
    EnterCriticalSection(c->lock);
    if (*c->unit) {
        if (c->n_in) {
            int k = emu_serial_write(*c->unit, c->in, c->n_in);
            memmove(c->in, c->in + k, (size_t)(c->n_in - k));
            c->n_in -= k;
            moved |= k > 0;
        }
        space = emu_serial_space(*c->unit) - c->n_in;
        if (!c->writing && !c->n_out) {
            c->n_out = emu_serial_read(*c->unit, c->out, OUT_CAP, &st);
            c->out_st = st;
            fetched = 1;
        }
    }
    LeaveCriticalSection(c->lock);
    if (!c->writing && (c->n_out || fetched))
        apply(c, &c->out_st);              /* before the bytes that left under it (none: the status now) */
    if (!c->writing && c->n_out) {
        ResetEvent(c->wo.hEvent);
        if (WriteFile(c->port, c->out, (DWORD)c->n_out, &got, &c->wo)) {
            c->n_out = 0;
            moved = 1;
        } else if (GetLastError() == ERROR_IO_PENDING)
            c->writing = 1;
        else
            c->n_out = 0;                  /* the port refused them: they are lost, as on a dead line */
    }
    if (!c->reading && space > 0) {
        ResetEvent(c->ro.hEvent);
        if (ReadFile(c->port, c->in + c->n_in, (DWORD)space, &got, &c->ro)) {
            c->n_in += (int)got;
            moved |= got > 0;
        } else if (GetLastError() == ERROR_IO_PENDING)
            c->reading = 1;
    }
    return moved;
}

static DWORD WINAPI link_thread(LPVOID arg)
{
    com_link *c = (com_link *)arg;
    for (;;) {
        HANDLE wait[4];
        DWORD n = 0, r;
        if (round_trip(c))
            continue;
        wait[n++] = c->quit;
        wait[n++] = c->kick;
        if (c->reading)
            wait[n++] = c->ro.hEvent;
        if (c->writing)
            wait[n++] = c->wo.hEvent;
        r = WaitForMultipleObjects(n, wait, FALSE, 1000);
        if (r == WAIT_OBJECT_0)
            break;
    }
    CancelIo(c->port);
    if (c->reading)
        GetOverlappedResult(c->port, &c->ro, &(DWORD){0}, TRUE);
    if (c->writing)
        GetOverlappedResult(c->port, &c->wo, &(DWORD){0}, TRUE);
    return 0;
}

com_link *com_open(const char *name, CRITICAL_SECTION *lock, emu_unit **unit, char *err, int errlen)
{
    char path[96];
    COMMTIMEOUTS to;
    com_link *c = (com_link *)calloc(1, sizeof(com_link));
    if (!c) {
        snprintf(err, errlen, "Out of memory.");
        return NULL;
    }
    snprintf(path, sizeof path, "\\\\.\\%s", name);
    c->port = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (c->port == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        char msg[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
        snprintf(err, errlen, "%s%s", msg, e == ERROR_ACCESS_DENIED ? "(Another program may have it open.)" : "");
        free(c);
        return NULL;
    }
    /* a read returns as soon as a byte is there, or after a second with none; writes never time out */
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = 1000;
    SetCommTimeouts(c->port, &to);
    SetupComm(c->port, 4096, 4096);
    PurgeComm(c->port, PURGE_RXCLEAR | PURGE_TXCLEAR);
    c->lock = lock;
    c->unit = unit;
    c->ro.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->wo.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->quit = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->kick = CreateEventA(NULL, FALSE, FALSE, NULL);
    c->thread = CreateThread(NULL, 0, link_thread, c, 0, NULL);
    if (!c->thread) {
        snprintf(err, errlen, "Could not start the port's thread.");
        c->thread = NULL;
        com_close(c);
        return NULL;
    }
    return c;
}

void com_kick(com_link *c)
{
    if (c)
        SetEvent(c->kick);
}

void com_close(com_link *c)
{
    if (!c)
        return;
    if (c->thread) {
        SetEvent(c->quit);
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
    }
    if (c->have_applied) {                 /* the unit is gone from the line: its DTR and RTS drop */
        EscapeCommFunction(c->port, CLRDTR);
        EscapeCommFunction(c->port, CLRRTS);
    }
    CloseHandle(c->port);
    CloseHandle(c->ro.hEvent);
    CloseHandle(c->wo.hEvent);
    CloseHandle(c->quit);
    CloseHandle(c->kick);
    free(c);
}
