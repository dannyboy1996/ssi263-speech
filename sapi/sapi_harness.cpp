/* sapi_harness.cpp -- the SAPI engine DLL driven the way SAPI drives it, with no registration at all: the DLL loaded
 * from a path, its class factory taken from DllGetClassObject, the engine given a token (a stand-in holding only the
 * VoiceId, which is all the engine reads), GetOutputFormat asked, and Speak called with SAPI's fragments -- text,
 * bookmarks, the pitch's MiddleAdj -- and an engine site of our own that answers GetRate, collects every Write and
 * every event, and raises SPVES_ABORT after a given number of writes (SAPI's purge).
 *
 * Why not SAPI itself: SAPI refuses a voice token kept under HKCU (E_ACCESSDENIED at Speak, a Microsoft voice's
 * copied token included), and System.Speech does not even list one, so a development build cannot reach SAPI without
 * machine-wide tokens -- which would sit beside, and could be mistaken for, the installed engine's.  This drives the
 * same COM object in the same way, for sapi/test_sapi_engine.py.  Built by sapi\build.ps1 -Dev, x86 and x64.
 *
 *   sapi_harness.exe <ssi263_sapi.dll> <job file> <output folder>
 *
 * The job: one case per line, UTF-8, tab-separated --
 *     voice id, SAPI rate (-10..10), MiddleAdj (-10..10), settings "Inflection,Whine,AccentInflection,RunAhead,
 *     SampleRate", SSI263_SAPI_TEST_BREAK for this case ("-" for none), writes before an abort (0: none), text,
 *     and optionally a second text after a bookmark named 7.
 * The settings go to the key SSI263_SAPI_SETTINGS_KEY names (the development DLL reads that one).  For each case the
 * audio goes to <folder>\case_<k>.pcm and a line to stdout:
 *     case <k> hr=<hex> rate=<declared Hz> bytes=<written> writes=<n> marks=<names, comma-separated or -> ms=<n>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>

static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w(n, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

/* A token that holds only a VoiceId. */
class Token : public ISpObjectToken {
    LONG refs; std::wstring id;
public:
    Token(const std::wstring &v) : refs(1), id(v) {}
    STDMETHODIMP QueryInterface(REFIID i, void **p) {
        if (!p) return E_POINTER;
        *p = 0;
        if (i == IID_IUnknown || i == IID_ISpDataKey || i == IID_ISpObjectToken) *p = (ISpObjectToken *)this;
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() { ULONG n = InterlockedDecrement(&refs); if (!n) delete this; return n; }
    STDMETHODIMP GetStringValue(LPCWSTR name, LPWSTR *v) {
        if (!v) return E_POINTER;
        if (!name || wcscmp(name, L"VoiceId")) return SPERR_NOT_FOUND;
        size_t n = (id.size() + 1) * sizeof(wchar_t);
        *v = (LPWSTR)CoTaskMemAlloc(n);
        if (!*v) return E_OUTOFMEMORY;
        memcpy(*v, id.c_str(), n);
        return S_OK;
    }
    STDMETHODIMP SetData(LPCWSTR, ULONG, const BYTE *) { return E_NOTIMPL; }
    STDMETHODIMP GetData(LPCWSTR, ULONG *, BYTE *) { return SPERR_NOT_FOUND; }
    STDMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) { return E_NOTIMPL; }
    STDMETHODIMP SetDWORD(LPCWSTR, DWORD) { return E_NOTIMPL; }
    STDMETHODIMP GetDWORD(LPCWSTR, DWORD *) { return SPERR_NOT_FOUND; }
    STDMETHODIMP OpenKey(LPCWSTR, ISpDataKey **) { return SPERR_NOT_FOUND; }
    STDMETHODIMP CreateKey(LPCWSTR, ISpDataKey **) { return E_NOTIMPL; }
    STDMETHODIMP DeleteKey(LPCWSTR) { return E_NOTIMPL; }
    STDMETHODIMP DeleteValue(LPCWSTR) { return E_NOTIMPL; }
    STDMETHODIMP EnumKeys(ULONG, LPWSTR *) { return SPERR_NO_MORE_ITEMS; }
    STDMETHODIMP EnumValues(ULONG, LPWSTR *) { return SPERR_NO_MORE_ITEMS; }
    STDMETHODIMP SetId(LPCWSTR, LPCWSTR, BOOL) { return E_NOTIMPL; }
    STDMETHODIMP GetId(LPWSTR *) { return E_NOTIMPL; }
    STDMETHODIMP GetCategory(ISpObjectTokenCategory **) { return E_NOTIMPL; }
    STDMETHODIMP CreateInstance(IUnknown *, DWORD, REFIID, void **) { return E_NOTIMPL; }
    STDMETHODIMP GetStorageFileName(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR *) { return E_NOTIMPL; }
    STDMETHODIMP RemoveStorageFileName(REFCLSID, LPCWSTR, BOOL) { return E_NOTIMPL; }
    STDMETHODIMP Remove(const CLSID *) { return E_NOTIMPL; }
    STDMETHODIMP IsUISupported(LPCWSTR, void *, ULONG, IUnknown *, BOOL *) { return E_NOTIMPL; }
    STDMETHODIMP DisplayUI(HWND, LPCWSTR, LPCWSTR, void *, ULONG, IUnknown *) { return E_NOTIMPL; }
    STDMETHODIMP MatchesAttributes(LPCWSTR, BOOL *) { return E_NOTIMPL; }
};

/* SAPI's side of a Speak: the rate, the writes, the events, and a purge after `abort_after` writes. */
class Site : public ISpTTSEngineSite {
    LONG refs;
public:
    long rate; int abort_after, writes; std::vector<BYTE> audio; std::vector<std::wstring> marks;
    Site() : refs(1), rate(0), abort_after(0), writes(0) {}
    STDMETHODIMP QueryInterface(REFIID i, void **p) {
        if (!p) return E_POINTER;
        *p = 0;
        if (i == IID_IUnknown || i == IID_ISpEventSink || i == IID_ISpTTSEngineSite) *p = (ISpTTSEngineSite *)this;
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&refs); }
    STDMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&refs); }       /* on the stack */
    STDMETHODIMP AddEvents(const SPEVENT *ev, ULONG n) {
        for (ULONG k = 0; k < n; k++)
            if (ev[k].eEventId == SPEI_TTS_BOOKMARK && ev[k].elParamType == SPET_LPARAM_IS_STRING && ev[k].lParam)
                marks.push_back((const wchar_t *)ev[k].lParam);
        return S_OK;
    }
    STDMETHODIMP GetEventInterest(ULONGLONG *p) { if (p) *p = ~0ull; return S_OK; }
    STDMETHODIMP_(DWORD) GetActions() { return abort_after && writes >= abort_after ? SPVES_ABORT : 0; }
    STDMETHODIMP Write(const void *p, ULONG n, ULONG *done) {
        audio.insert(audio.end(), (const BYTE *)p, (const BYTE *)p + n);
        writes++;
        if (done) *done = n;
        return S_OK;
    }
    STDMETHODIMP GetRate(long *r) { if (r) *r = rate; return S_OK; }
    STDMETHODIMP GetVolume(USHORT *v) { if (v) *v = 100; return S_OK; }
    STDMETHODIMP GetSkipInfo(SPVSKIPTYPE *t, long *n) { if (t) *t = SPVST_SENTENCE; if (n) *n = 0; return S_OK; }
    STDMETHODIMP CompleteSkip(long) { return S_OK; }
};

static std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> out; size_t a = 0, b;
    while ((b = s.find(c, a)) != std::string::npos) { out.push_back(s.substr(a, b - a)); a = b + 1; }
    out.push_back(s.substr(a));
    return out;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) { fputs("sapi_harness.exe <ssi263_sapi.dll> <job> <folder>\n", stderr); return 2; }
    CoInitializeEx(0, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryExW(argv[1], 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { fprintf(stderr, "cannot load %ls (%lu)\n", argv[1], GetLastError()); return 1; }
    typedef HRESULT (STDAPICALLTYPE *GetClassObject)(REFCLSID, REFIID, void **);
    GetClassObject get = (GetClassObject)GetProcAddress(dll, "DllGetClassObject");
    static const CLSID dev = {0x3c0e5f7a,0x9d21,0x4e83,{0xb1,0x6f,0x52,0x0d,0x8a,0x4c,0x77,0x19}};
    IClassFactory *factory = 0;
    if (!get || FAILED(get(dev, IID_IClassFactory, (void **)&factory))) {
        fputs("not the development engine (no SSI263_SAPI_DEV class)\n", stderr);
        return 1;
    }
    wchar_t keyname[256] = L"Software\\SSI-263 SAPI (development)";
    GetEnvironmentVariableW(L"SSI263_SAPI_SETTINGS_KEY", keyname, 256);
    FILE *job = _wfopen(argv[2], L"rb");
    if (!job) { fputs("no job file\n", stderr); return 1; }
    std::string all; char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, job)) > 0) all.append(buf, got);
    fclose(job);
    int k = 0;
    for (const std::string &line0 : split(all, '\n')) {
        std::string line = line0;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f = split(line, '\t');
        if (f.size() < 7) continue;
        /* the settings, then the break for this case */
        std::vector<std::string> s = split(f[3], ',');
        const wchar_t *names[] = {L"Inflection", L"Whine", L"AccentInflection", L"RunAhead", L"SampleRate"};
        HKEY h;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyname, 0, 0, 0, KEY_WRITE, 0, &h, 0) == ERROR_SUCCESS) {
            for (int i = 0; i < 5 && i < (int)s.size(); i++) {
                DWORD v = (DWORD)atol(s[i].c_str());
                RegSetValueExW(h, names[i], 0, REG_DWORD, (const BYTE *)&v, sizeof v);
            }
            RegCloseKey(h);
        }
        SetEnvironmentVariableA("SSI263_SAPI_TEST_BREAK", f[4] == "-" ? 0 : f[4].c_str());
        ISpTTSEngine *engine = 0;
        ISpObjectWithToken *with = 0;
        HRESULT hr = factory->CreateInstance(0, IID_ISpTTSEngine, (void **)&engine);
        Token *tok = new Token(wide(f[0]));
        if (SUCCEEDED(hr)) hr = engine->QueryInterface(IID_ISpObjectWithToken, (void **)&with);
        if (SUCCEEDED(hr)) hr = with->SetObjectToken(tok);
        GUID fmt; WAVEFORMATEX *wfx = 0;
        if (SUCCEEDED(hr)) hr = engine->GetOutputFormat(0, 0, &fmt, &wfx);
        Site site;
        site.rate = atol(f[1].c_str());
        site.abort_after = atoi(f[5].c_str());
        long middle = atol(f[2].c_str());
        std::wstring t1 = wide(f[6]), t2 = f.size() > 7 ? wide(f[7]) : L"", seven = L"7";
        SPVTEXTFRAG fr[3];
        memset(fr, 0, sizeof fr);
        for (int i = 0; i < 3; i++) { fr[i].State.eAction = SPVA_Speak; fr[i].State.PitchAdj.MiddleAdj = middle; }
        fr[0].pTextStart = t1.c_str(); fr[0].ulTextLen = (ULONG)t1.size();
        if (f.size() > 7) {
            fr[0].pNext = &fr[1];
            fr[1].State.eAction = SPVA_Bookmark; fr[1].pTextStart = seven.c_str(); fr[1].ulTextLen = 1; fr[1].pNext = &fr[2];
            fr[2].pTextStart = t2.c_str(); fr[2].ulTextLen = (ULONG)t2.size(); fr[2].ulTextSrcOffset = (ULONG)t1.size();
        }
        DWORD t0 = GetTickCount();
        if (SUCCEEDED(hr)) hr = engine->Speak(SPF_DEFAULT, fmt, wfx, fr, &site);
        DWORD ms = GetTickCount() - t0;
        wchar_t out[MAX_PATH];
        _snwprintf(out, MAX_PATH, L"%ls\\case_%d.pcm", argv[3], k);
        FILE *o = _wfopen(out, L"wb");
        if (o) { if (!site.audio.empty()) fwrite(site.audio.data(), 1, site.audio.size(), o); fclose(o); }
        std::string marks;
        for (size_t i = 0; i < site.marks.size(); i++) {
            if (i) marks += ",";
            char m[64]; WideCharToMultiByte(CP_UTF8, 0, site.marks[i].c_str(), -1, m, 64, 0, 0); marks += m;
        }
        printf("case %d hr=%08lx rate=%lu bytes=%lu writes=%d marks=%s ms=%lu\n", k, (unsigned long)hr,
               wfx ? (unsigned long)wfx->nSamplesPerSec : 0ul, (unsigned long)site.audio.size(), site.writes,
               marks.empty() ? "-" : marks.c_str(), (unsigned long)ms);
        fflush(stdout);
        if (wfx) CoTaskMemFree(wfx);
        if (with) with->Release();
        if (engine) engine->Release();
        tok->Release();
        k++;
    }
    factory->Release();
    return 0;
}
