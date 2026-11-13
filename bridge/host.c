// host.c - the helper process. Built twice from this file: vst2bridge-host32.exe (runs 32-bit plugins for the
// 64-bit bridge) and vst2bridge-host64.exe (runs 64-bit plugins for the 32-bit bridge).
//   vst2bridge-hostNN.exe <plugin.dll> <object prefix> <shell id> <host pid>            Windows bridge: named objects
//   vst2bridge-hostNN.exe <plugin.dll> net:<port>:<token>:<shared file> <shell id> 0     Linux / macOS CLAP (under Wine)
// Loads one plugin and serves the bridge's calls (protocol.h): the main thread runs the window message loop and the
// calls of the host's GUI / control thread, a second thread the calls of the host's audio thread.
// Under Wine (net:) the shared memory is a file of the Linux / macOS side, the signals travel over four loopback TCP
// connections (one byte per signal) and a lock word in shared memory replaces the named mutex (see protocol.h).
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <math.h>
#include "vst2.h"
#include "protocol.h"
#include "marshal.h"

typedef struct { HANDLE ev; SOCKET s; } Sig;    // a signal to the bridge: a named event, or (net:) a byte on a socket

static VbShared* sh;
static HANDLE   mutex, ev_main_h, ev_audio_h, ev_aux_h;         // signals from the bridge (net: local events fed by readers)
static Sig      sig_ready, sig_main_s, sig_main_v, sig_audio_s, sig_aux_s;
static int      net;                            // 1: run by the Linux / macOS CLAP under Wine
static char     title[64];                      // net: title of the editor window the Linux CLAP looks for
static AEffect* fx;
static DWORD    main_tid, audio_tid;
static int      main_active, audio_active;      // >0: the thread takes part in a call on its channel
static CRITICAL_SECTION aux_cs;
static volatile LONG quitting, closing;
static int32_t  shell_id;
static void*    chunk_ptr; static size_t chunk_size;    // result of the last effGetChunk
static HWND     editor_wnd;
static int      editor_open, ed_w, ed_h, ed_mode;   // ed_mode: 0 child of the host's window, VB_F_TOPLEVEL, VB_F_FLOATING
static VstTimeInfo time_info[3];                // per channel: the plugin keeps the pointer until its next call
static char     directory[VB_STR_MAX];
static wchar_t  plugin_path[MAX_PATH];


typedef struct {
    int32_t op, index; int64_t value; float opt;
    const void* in; uint32_t in_len;
    void* out; uint32_t out_cap; uint32_t out_len;
    int64_t ret;
} Req;

static void serve_req(VbFrame* f);

static void fill_info(void)
{
    VbInfo* i = &sh->info;
    if (!fx) return;
    i->numPrograms = fx->numPrograms; i->numParams = fx->numParams; i->numInputs = fx->numInputs; i->numOutputs = fx->numOutputs;
    i->flags = fx->flags; i->uniqueID = fx->uniqueID; i->version = fx->version;
    i->initialDelay = fx->initialDelay;
    i->ioRatio = fx->ioRatio; i->has_replacing = fx->processReplacing != 0; i->has_double = fx->processDoubleReplacing != 0;
}

// ----------------------------------------------------------------------------- channel plumbing

static void sig_set(Sig* g)
{
    if (g->s != INVALID_SOCKET) { char b = 1; send(g->s, &b, 1, 0); }
    else SetEvent(g->ev);
}

static int hwait(HANDLE ev, int pump)
{
    for (;;) {
        DWORD r = pump ? MsgWaitForMultipleObjects(1, &ev, FALSE, INFINITE, QS_SENDMESSAGE) : WaitForSingleObject(ev, INFINITE);
        if (r == WAIT_OBJECT_0) return 1;
        if (pump && r == WAIT_OBJECT_0 + 1) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        return 0;
    }
}

static Sig* main_to_stub(void) { return sh->main.owner == VB_OWNER_HELPER ? &sig_main_v : &sig_main_s; }

// serves the request on top of ch and replies
static void serve_top(VbChan* ch, int* active, Sig* to)
{
    MemoryBarrier();
    if (ch->msg != VB_MSG_REQ || ch->depth < 1) return;
    (*active)++;
    serve_req(&ch->f[ch->depth - 1]);
    (*active)--;
    fill_info();
    MemoryBarrier();
    ch->msg = VB_MSG_REPLY;
    sig_set(to);
}

// one call to the bridge; serves its nested calls meanwhile
static int hexchange(VbChan* ch, Sig* to, HANDLE in, int pump, int* active, Req* r)
{
    int d = ch->depth;
    if (d >= VB_MAXDEPTH || r->in_len > VB_FRAME_DATA) return 0;
    VbFrame* f = &ch->f[d];
    f->kind = VB_K_MASTER; f->op = r->op; f->index = r->index; f->flags = 0;
    f->value = r->value; f->opt = r->opt; f->ret = 0; f->len = r->in_len;
    if (r->in_len) memcpy(f->data, r->in, r->in_len);
    fill_info();
    ch->depth = d + 1;
    MemoryBarrier();
    ch->msg = VB_MSG_REQ;
    (*active)++;
    sig_set(to);
    int ok = 1;
    for (;;) {
        if (!hwait(in, pump)) { ok = 0; break; }
        MemoryBarrier();
        if (ch->msg == VB_MSG_REPLY && ch->depth == d + 1) break;
        if (ch->msg == VB_MSG_REQ && ch->depth > d + 1) serve_top(ch, active, to);
    }
    (*active)--;
    r->ret = f->ret;
    r->out_len = f->len < r->out_cap ? f->len : r->out_cap;
    if (ok && r->out && r->out_len) memcpy(r->out, f->data, r->out_len);
    ch->depth = d;
    return ok;
}

// main thread, outside a call: take the mutex; serve the bridge's calls while waiting for it
static int lock_main(void)
{
    if (net) {                                  // the lock word in shared memory (contention is rare: poll it)
        for (;;) {
            if (InterlockedCompareExchange((volatile LONG*)&sh->main.lock, VB_OWNER_HELPER, 0) == 0) return 1;
            DWORD r = MsgWaitForMultipleObjects(1, &ev_main_h, FALSE, 1, QS_SENDMESSAGE);
            if (r == WAIT_OBJECT_0) serve_top(&sh->main, &main_active, main_to_stub());
            else if (r == WAIT_OBJECT_0 + 1) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); }
            else if (r != WAIT_TIMEOUT) return 0;
        }
    }
    HANDLE w[2] = { mutex, ev_main_h };
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) return 1;
        if (r == WAIT_OBJECT_0 + 1) { serve_top(&sh->main, &main_active, main_to_stub()); continue; }
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        return 0;
    }
}

static int call_host(Req* r)
{
    DWORD me = GetCurrentThreadId();
    if (me == main_tid) {
        if (main_active) return hexchange(&sh->main, main_to_stub(), ev_main_h, 1, &main_active, r);
        if (!lock_main()) return 0;
        sh->main.owner = VB_OWNER_HELPER;
        int ok = hexchange(&sh->main, &sig_main_v, ev_main_h, 1, &main_active, r);
        sh->main.owner = VB_OWNER_NONE;
        if (net) InterlockedExchange((volatile LONG*)&sh->main.lock, 0); else ReleaseMutex(mutex);
        return ok;
    }
    if (me == audio_tid && audio_active) return hexchange(&sh->audio, &sig_audio_s, ev_audio_h, 0, &audio_active, r);
    EnterCriticalSection(&aux_cs);                                  // any other thread of the plugin
    int dummy = 0;
    int ok = hexchange(&sh->aux, &sig_aux_s, ev_aux_h, 0, &dummy, r);
    LeaveCriticalSection(&aux_cs);
    return ok;
}

// ----------------------------------------------------------------------------- plugin -> host

static intptr_t VSTCALL master(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt)
{
    (void)e;
    if (op == audioMasterCurrentId) return shell_id;
    if (op == audioMasterVersion && closing) return 2400;
    if (closing || !sh) return 0;
    int size, m = master_marshal(op, &size);
    Req r; memset(&r, 0, sizeof r);
    r.op = op; r.index = idx; r.value = (int64_t)val; r.opt = opt;
    int ch = GetCurrentThreadId() == main_tid ? 0 : GetCurrentThreadId() == audio_tid ? 1 : 2;
    switch (m) {
    case M_NONE:
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_STR_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)strnlen((const char*)ptr, VB_STR_MAX - 1) + 1; }
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_STR_OUT:
        if (ptr) { r.out = ptr; r.out_cap = 256; }                  // kVstMaxVendorStrLen / kVstMaxProductStrLen are 64
        return call_host(&r) ? (intptr_t)r.ret : 0;
    case M_SPECIAL:
        switch (op) {
        case audioMasterGetTime:
            r.out = &time_info[ch]; r.out_cap = sizeof(VstTimeInfo);
            if (!call_host(&r) || !r.ret || r.out_len != sizeof(VstTimeInfo)) return 0;
            return (intptr_t)&time_info[ch];
        case audioMasterGetDirectory:
            r.out = directory; r.out_cap = sizeof directory - 1;
            if (!call_host(&r) || !r.ret) return 0;
            directory[r.out_len] = 0;
            return (intptr_t)directory;
        case audioMasterVendorSpecific:
            return ptr ? 0 : call_host(&r) ? (intptr_t)r.ret : 0;
        case audioMasterSizeWindow:
            if (editor_wnd && idx > 0 && val > 0) {
                ed_w = idx; ed_h = (int)val;
                RECT rc = { 0, 0, ed_w, ed_h };
                if (ed_mode == VB_F_FLOATING) AdjustWindowRectEx(&rc, GetWindowLongW(editor_wnd, GWL_STYLE), FALSE, GetWindowLongW(editor_wnd, GWL_EXSTYLE));
                SetWindowPos(editor_wnd, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            }
            return call_host(&r) ? (intptr_t)r.ret : 0;
        case audioMasterProcessEvents: {                            // MIDI out of the plugin
            const VstEvents* evs = (const VstEvents*)ptr;
            if (!evs || evs->numEvents <= 0) return 0;
            uint8_t* buf = (uint8_t*)malloc(VB_FRAME_DATA);
            if (!buf) return 0;
            uint32_t n = 0, cnt = evs->numEvents > VB_MAXEVENTS_TO_HOST ? VB_MAXEVENTS_TO_HOST : (uint32_t)evs->numEvents;
            VbEvent* out = (VbEvent*)(buf + 4);
            size_t sx_room = VB_FRAME_DATA - 4 - cnt * sizeof(VbEvent), sx_used = 0;
            uint8_t* sx = buf + 4 + cnt * sizeof(VbEvent);
            for (uint32_t i = 0; i < cnt; i++) {
                const VstEvent* ev = evs->events[i];
                if (!ev) continue;
                VbEvent* q = &out[n]; memset(q, 0, sizeof *q);
                q->type = ev->type; q->delta = ev->deltaFrames; q->flags = ev->flags;
                if (ev->type == kVstMidiType) {
                    const VstMidiEvent* me = (const VstMidiEvent*)ev;
                    memcpy(q->midi, me->midiData, 4); q->note_length = me->noteLength; q->note_offset = me->noteOffset;
                    q->detune = me->detune; q->off_velocity = (uint8_t)me->noteOffVelocity;
                } else if (ev->type == kVstSysExType) {
                    const VstMidiSysexEvent* se = (const VstMidiSysexEvent*)ev;
                    size_t len = se->dumpBytes > 0 ? (size_t)se->dumpBytes : 0;
                    if (!se->sysexDump || !len || sx_used + len > sx_room) continue;
                    q->sysex_off = (uint32_t)sx_used; q->sysex_len = (uint32_t)len;
                    memcpy(sx + sx_used, se->sysexDump, len); sx_used += len;
                } else continue;
                n++;
            }
            // pack: the events, then the SysEx bytes right behind them
            if (n < cnt) memmove(buf + 4 + n * sizeof(VbEvent), sx, sx_used);
            memcpy(buf, &n, 4);
            r.in = buf; r.in_len = (uint32_t)(4 + n * sizeof(VbEvent) + sx_used);
            intptr_t ret = call_host(&r) ? (intptr_t)r.ret : 0;
            free(buf);
            return ret;
        }
        }
        return 0;
    default:
        return 0;                                                   // M_REFUSE: not supported across processes
    }
}

// ----------------------------------------------------------------------------- editor window

static LRESULT CALLBACK ed_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_TIMER && fx && editor_open) {
        // A plugin that opens a modal dialog (MessageBox, DialogBox) runs its own message loop on this thread and does
        // not look at ev_main_h, while the host is blocked in a call waiting for us and shares our input queue, so
        // the dialog never gets its clicks (Hypersonic 2, #30). Timers are dispatched by modal loops: serve here.
        // effEditClose is left to the main loop (it destroys this window); and nothing runs after it closed the editor.
        VbChan* mc = &sh->main;
        int pending = mc->msg == VB_MSG_REQ && mc->depth >= 1;
        if (!main_active && pending && mc->f[mc->depth - 1].op != effEditClose && WaitForSingleObject(ev_main_h, 0) == WAIT_OBJECT_0)
            serve_top(mc, &main_active, main_to_stub());
        if (editor_open) fx->dispatcher(fx, effEditIdle, 0, 0, 0, 0);
        return 0;
    }
    if (m == WM_CLOSE && ed_mode == VB_F_FLOATING) { ShowWindow(w, SW_HIDE); sh->editor_closed = 1; return 0; }  // closing only hides it
    // The editor is a child of the host's window, which belongs to another process. DefWindowProc would pass a wheel
    // message the plugin did not use up to that window with a blocking SendMessage while the host turns the wheel into
    // effVendorSpecific('stCA', 'Whee') and waits for this thread: both wait for each other (FL Studio hang, #30).
    if (m == WM_MOUSEWHEEL || m == WM_MOUSEHWHEEL) return 0;
    return DefWindowProcW(w, m, wp, lp);
}

static void edit_rect(void)
{
    ERect* r = 0;
    fx->dispatcher(fx, effEditGetRect, 0, 0, &r, 0);
    if (r && r->right > r->left && r->bottom > r->top) { ed_w = r->right - r->left; ed_h = r->bottom - r->top; }
}

// mode 0: a child of the host's window (Windows); VB_F_TOPLEVEL: a bare popup with a known title that the Linux CLAP
// moves into the host's X11 window; VB_F_FLOATING: a window of its own with a title bar (macOS, cannot be embedded)
static intptr_t edit_open(HWND parent, int mode)
{
    static int registered;
    if (editor_open) { if (mode == VB_F_FLOATING && editor_wnd) { sh->editor_closed = 0; ShowWindow(editor_wnd, SW_SHOW); } return 1; }
    if (!(fx->flags & effFlagsHasEditor)) return 0;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = ed_proc; wc.hInstance = GetModuleHandleW(0); wc.lpszClassName = L"VST2BridgeEditor";
        wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
        RegisterClassW(&wc); registered = 1;
    }
    edit_rect();
    if (ed_w <= 0) { ed_w = 640; ed_h = 480; }
    ed_mode = mode;
    if (mode == VB_F_FLOATING) {
        char name[256]; memset(name, 0, sizeof name);
        fx->dispatcher(fx, effGetEffectName, 0, 0, name, 0);
        name[sizeof name - 1] = 0;
        wchar_t wname[256];
        if (!name[0] || !MultiByteToWideChar(CP_ACP, 0, name, -1, wname, 256)) wcscpy(wname, L"VST2Bridge");
        DWORD st = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
        RECT rc = { 0, 0, ed_w, ed_h };
        AdjustWindowRectEx(&rc, st, FALSE, WS_EX_TOPMOST);
        editor_wnd = CreateWindowExW(WS_EX_TOPMOST, L"VST2BridgeEditor", wname, st, CW_USEDEFAULT, CW_USEDEFAULT,
                                     rc.right - rc.left, rc.bottom - rc.top, 0, 0, GetModuleHandleW(0), 0);
    } else if (mode == VB_F_TOPLEVEL) {
        wchar_t wt[64]; MultiByteToWideChar(CP_ACP, 0, title, -1, wt, 64);
        editor_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"VST2BridgeEditor", wt, WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                     0, 0, ed_w, ed_h, 0, 0, GetModuleHandleW(0), 0);
    } else    // a child of the host's window, although that window belongs to another process (Windows allows this)
        editor_wnd = CreateWindowExW(0, L"VST2BridgeEditor", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                     0, 0, ed_w, ed_h, parent, 0, GetModuleHandleW(0), 0);
    if (!editor_wnd) return 0;
    intptr_t ret = fx->dispatcher(fx, effEditOpen, 0, 0, editor_wnd, 0);
    editor_open = 1;
    edit_rect();                                                    // some plugins know their size only once open
    RECT rc = { 0, 0, ed_w, ed_h };
    if (mode == VB_F_FLOATING) AdjustWindowRectEx(&rc, GetWindowLongW(editor_wnd, GWL_STYLE), FALSE, GetWindowLongW(editor_wnd, GWL_EXSTYLE));
    SetWindowPos(editor_wnd, 0, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    SetTimer(editor_wnd, 1, 30, 0);
    ShowWindow(editor_wnd, mode == VB_F_FLOATING ? SW_SHOW : SW_SHOWNA);
    return ret ? ret : 1;
}

static void edit_close(void)
{
    if (!editor_open) return;
    KillTimer(editor_wnd, 1);
    fx->dispatcher(fx, effEditClose, 0, 0, 0, 0);
    editor_open = 0;
    DestroyWindow(editor_wnd); editor_wnd = 0;
    sh->editor_closed = 0;
}


// ----------------------------------------------------------------------------- host -> plugin

static void do_process(VbFrame* f)
{
    static VstMidiEvent mev[VB_MAXEVENTS_TO_PLUGIN];
    static VstMidiSysexEvent sev[VB_MAXEVENTS_TO_PLUGIN];
    static char evbuf[sizeof(VstEvents) + VB_MAXEVENTS_TO_PLUGIN * sizeof(VstEvent*)];
    static double zero[VB_MAXFRAMES], sink[VB_MAXFRAMES];
    static void* ins[256]; static void* outs[256];
    VbAudio* a = &sh->au;
    int n = (int)f->value, dbl = f->op == VB_P_DOUBLE;
    size_t es = dbl ? sizeof(double) : sizeof(float);
    if (n < 0) n = 0;
    if (n > VB_MAXFRAMES) n = VB_MAXFRAMES;

    uint32_t ne = a->n_events > VB_MAXEVENTS_TO_PLUGIN ? VB_MAXEVENTS_TO_PLUGIN : a->n_events, cnt = 0;
    VstEvents* list = (VstEvents*)evbuf;
    for (uint32_t i = 0; i < ne; i++) {
        const VbEvent* e = &a->events[i];
        if (e->type == kVstSysExType) {
            if ((uint64_t)e->sysex_off + e->sysex_len > VB_SYSEX_MAX) continue;
            memset(&sev[i], 0, sizeof sev[i]);
            sev[i].type = kVstSysExType; sev[i].byteSize = sizeof sev[i]; sev[i].deltaFrames = e->delta; sev[i].flags = e->flags;
            sev[i].dumpBytes = (int32_t)e->sysex_len; sev[i].sysexDump = (char*)a->sysex + e->sysex_off;
            list->events[cnt++] = (VstEvent*)&sev[i];
        } else {
            memset(&mev[i], 0, sizeof mev[i]);
            mev[i].type = kVstMidiType; mev[i].byteSize = sizeof mev[i]; mev[i].deltaFrames = e->delta; mev[i].flags = e->flags;
            mev[i].noteLength = e->note_length; mev[i].noteOffset = e->note_offset;
            memcpy(mev[i].midiData, e->midi, 4); mev[i].detune = e->detune; mev[i].noteOffVelocity = (char)e->off_velocity;
            list->events[cnt++] = (VstEvent*)&mev[i];
        }
    }
    if (cnt) { list->numEvents = (int32_t)cnt; list->reserved = 0; fx->dispatcher(fx, effProcessEvents, 0, 0, list, 0); }

    int nin = fx->numInputs < 256 ? fx->numInputs : 256, nout = fx->numOutputs < 256 ? fx->numOutputs : 256;
    memset(zero, 0, n * es);
    for (int c = 0; c < nin; c++) ins[c] = c < a->nin ? (void*)a->in[c] : (void*)zero;
    for (int c = 0; c < nout; c++) { outs[c] = c < VB_MAXCH ? (void*)a->out[c] : (void*)sink; memset(outs[c], 0, n * es); }
    if (dbl && fx->processDoubleReplacing) fx->processDoubleReplacing(fx, (double**)ins, (double**)outs, n);
    else if (fx->processReplacing && (fx->flags & effFlagsCanReplacing)) fx->processReplacing(fx, (float**)ins, (float**)outs, n);
    else if (fx->process) fx->process(fx, (float**)ins, (float**)outs, n);        // accumulates into the cleared outputs
    f->len = 0;
}

// the Linux file system is drive Z: under Wine
static void wine_path(char* out, size_t n, const char* unix_path)
{
    snprintf(out, n, "Z:%s", unix_path);
    for (char* q = out; *q; q++) if (*q == '/') *q = '\\';
}

static HANDLE map_file(const char* unix_path, DWORD size)
{
    char win[MAX_PATH + 8];
    wine_path(win, sizeof win, unix_path);
    HANDLE f = CreateFileA(win, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HANDLE m = CreateFileMappingA(f, 0, PAGE_READWRITE, 0, size, 0);
    CloseHandle(f);                                                 // the mapping keeps the file open
    return m;
}

// a mapping for one large chunk: a named mapping (Windows) or a file of the Linux / macOS side (net:)
static void* open_view(const char* name, HANDLE* m)
{
    *m = net ? map_file(name, 0) : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    void* v = *m ? MapViewOfFile(*m, FILE_MAP_ALL_ACCESS, 0, 0, 0) : 0;
    if (*m && !v) { CloseHandle(*m); *m = 0; }
    return v;
}

static void do_dispatch(VbFrame* f)
{
    int size, m = disp_marshal(f->op, &size);
    uint8_t* d = f->data;
    intptr_t ret = 0, val = (intptr_t)f->value;
    uint32_t in_len = f->len;
    f->len = 0;
    if (f->op == effEditClose) { edit_close(); f->ret = 0; return; }
    switch (m) {
    case M_NONE: ret = fx->dispatcher(fx, f->op, f->index, val, 0, f->opt); break;
    case M_STR_IN:
        if (in_len) d[in_len - 1] = 0;
        ret = fx->dispatcher(fx, f->op, f->index, val, in_len ? d : 0, f->opt);
        break;
    case M_STR_OUT: {
        char buf[VB_STR_MAX]; memset(buf, 0, sizeof buf);
        ret = fx->dispatcher(fx, f->op, f->index, val, buf, f->opt);
        buf[VB_STR_MAX - 1] = 0;
        f->len = (uint32_t)strlen(buf) + 1; memcpy(d, buf, f->len);
        break;
    }
    case M_IN: ret = fx->dispatcher(fx, f->op, f->index, val, in_len >= (uint32_t)size ? d : 0, f->opt); break;
    case M_INOUT:
        if (in_len < (uint32_t)size) memset(d + in_len, 0, size - in_len);
        ret = fx->dispatcher(fx, f->op, f->index, val, d, f->opt);
        f->len = (uint32_t)size;
        break;
    case M_SPECIAL:
        switch (f->op) {
        case effEditGetRect: {
            ERect* r = 0;
            ret = fx->dispatcher(fx, f->op, 0, 0, &r, 0);
            if (r) { memcpy(d, r, sizeof *r); f->len = sizeof *r; }
            break;
        }
        case effEditOpen: ret = edit_open((HWND)(uintptr_t)(uint32_t)f->value, f->flags & (VB_F_TOPLEVEL | VB_F_FLOATING)); break;
        case effGetChunk: {
            void* cp = 0;
            ret = fx->dispatcher(fx, f->op, f->index, 0, &cp, 0);
            chunk_ptr = cp; chunk_size = ret > 0 && cp ? (size_t)ret : 0;
            if (chunk_size && chunk_size <= VB_FRAME_DATA) { memcpy(d, cp, chunk_size); f->len = (uint32_t)chunk_size; }
            break;
        }
        case effSetChunk:
            if (f->flags & VB_F_MAPPED) {
                if (in_len) d[in_len - 1] = 0;
                HANDLE mh; void* v = open_view((const char*)d, &mh);
                if (v) { ret = fx->dispatcher(fx, f->op, f->index, val, v, 0); UnmapViewOfFile(v); CloseHandle(mh); }
            } else ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)in_len, d, 0);
            break;
        case effSetSpeakerArrangement: {
            int32_t si = 0;
            if (in_len >= 4) memcpy(&si, d, 4);
            if (si <= 0 || 4 + (uint32_t)si >= in_len) break;
            ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)(d + 4), d + 4 + si, f->opt);
            break;
        }
        case effGetSpeakerArrangement: {
            void* pin = 0; void* pout = 0;
            ret = fx->dispatcher(fx, f->op, f->index, (intptr_t)&pin, &pout, f->opt);
            uint32_t off = 0;
            void* src[2] = { pin, pout };
            for (int k = 0; k < 2; k++) {
                int32_t n = src[k] ? speaker_arr_size(src[k]) : 0;
                if (off + 4 + (uint32_t)n > VB_FRAME_DATA) n = 0;
                memcpy(d + off, &n, 4); off += 4;
                if (n) { memcpy(d + off, src[k], n); off += (uint32_t)n; }
            }
            f->len = off;
            break;
        }
        case effVendorSpecific: ret = fx->dispatcher(fx, f->op, f->index, val, 0, f->opt); break;
        }
        break;
    default: break;
    }
    f->ret = ret;
}

static void serve_req(VbFrame* f)
{
    switch (f->kind) {
    case VB_K_DISPATCH: do_dispatch(f); break;
    case VB_K_SETPARAM: fx->setParameter(fx, f->index, f->opt); f->len = 0; break;
    case VB_K_GETPARAM: f->opt = fx->getParameter(fx, f->index); f->len = 0; break;
    case VB_K_PROCESS:  do_process(f); break;
    case VB_K_READ_CHUNK: {
        f->ret = 0;
        if (f->len) f->data[f->len - 1] = 0;
        HANDLE mh; void* v = open_view((const char*)f->data, &mh);
        if (v) {
            if (chunk_ptr && chunk_size == (size_t)f->value) { memcpy(v, chunk_ptr, chunk_size); f->ret = 1; }
            UnmapViewOfFile(v); CloseHandle(mh);
        }
        f->len = 0;
        break;
    }
    case VB_K_EDITOR_SHOW:
        if (editor_wnd) { if (f->index) sh->editor_closed = 0; ShowWindow(editor_wnd, f->index ? (ed_mode == VB_F_FLOATING ? SW_SHOW : SW_SHOWNA) : SW_HIDE); }
        f->ret = editor_wnd != 0; f->len = 0;
        break;
    case VB_K_QUIT: InterlockedExchange(&quitting, 1); f->ret = 1; f->len = 0; break;
    default: f->ret = 0; f->len = 0; break;
    }
}

// ----------------------------------------------------------------------------- threads

static DWORD WINAPI audio_thread(LPVOID a)
{
    (void)a;
    audio_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (hwait(ev_audio_h, 0) && !quitting) serve_top(&sh->audio, &audio_active, &sig_audio_s);
    return 0;
}

static DWORD WINAPI watchdog(LPVOID a)                  // the host is gone (crashed): end as well
{
    HANDLE h = (HANDLE)a;
    WaitForSingleObject(h, INFINITE);
    ExitProcess(0);
    return 0;
}

static void fail(const char* msg)
{
    snprintf(sh->error, sizeof sh->error, "%s", msg);
    sh->ok = 0;
    sig_set(&sig_ready);
}

// net: one reader per connection turns the bridge's bytes into a local event; the bridge is gone when it closes
typedef struct { SOCKET s; HANDLE ev; } Reader;
static DWORD WINAPI reader(LPVOID a)
{
    Reader* r = (Reader*)a; char b[64];
    for (;;) {
        int n = recv(r->s, b, sizeof b, 0);
        if (n <= 0) ExitProcess(0);                                 // the host has ended (or crashed)
        SetEvent(r->ev);                                            // one signal at a time: the protocol never sends two
    }
    return 0;
}

static SOCKET connect_to(int port, const char* token, char kind)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) { closesocket(s); return INVALID_SOCKET; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    char hello[48]; snprintf(hello, sizeof hello, "%.16s%c", token, kind);   // the token proves the connection is ours
    send(s, hello, (int)strlen(hello), 0);
    return s;
}

static HANDLE start_reader(SOCKET s)
{
    Reader* r = (Reader*)calloc(1, sizeof *r);
    r->s = s; r->ev = CreateEventA(0, FALSE, FALSE, 0);
    CreateThread(0, 0, reader, r, 0, 0);
    return r->ev;
}

// net:<port>:<token>:<shared file>
static int open_net(char* arg, HANDLE* map)
{
    char* port = arg + 4; char* token = strchr(port, ':'); if (!token) return 0; *token++ = 0;
    char* file = strchr(token, ':'); if (!file) return 0; *file++ = 0;
    snprintf(title, sizeof title, "VST2Bridge %.16s", token);
    WSADATA wd; if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 0;
    int pn = atoi(port);
    SOCKET sm = connect_to(pn, token, 'M'), sv = connect_to(pn, token, 'V'), sa = connect_to(pn, token, 'A'), sx = connect_to(pn, token, 'X');
    if (sm == INVALID_SOCKET || sv == INVALID_SOCKET || sa == INVALID_SOCKET || sx == INVALID_SOCKET) return 0;
    sig_ready.s = sm; sig_main_s.s = sm; sig_main_v.s = sv; sig_audio_s.s = sa; sig_aux_s.s = sx;
    *map = map_file(file, (DWORD)sizeof(VbShared));
    if (!*map) return 0;
    ev_main_h = start_reader(sm); ev_audio_h = start_reader(sa); ev_aux_h = start_reader(sx);
    start_reader(sv);                                               // nothing comes this way; it notices a closed connection
    return 1;
}

static HANDLE open_obj(const char* prefix, const char* suffix, int kind)
{
    char name[160];
    snprintf(name, sizeof name, "%s%s", prefix, suffix);
    if (kind == 0) return OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    if (kind == 1) return OpenMutexA(MUTEX_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
    return OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
}

typedef AEffect* (VSTCALL *VstMain)(AudioMasterCallback);

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
    (void)hi; (void)hp; (void)cmd; (void)show;
    int argc; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 5) return 2;
    char prefix[1024];
    WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, prefix, sizeof prefix, 0, 0);
    shell_id = (int32_t)wcstol(argv[3], 0, 10);
    DWORD host_pid = (DWORD)wcstoul(argv[4], 0, 10);
    wcsncpy(plugin_path, argv[1], MAX_PATH - 1);

    Sig* outs[] = { &sig_ready, &sig_main_s, &sig_main_v, &sig_audio_s, &sig_aux_s };
    for (size_t i = 0; i < sizeof outs / sizeof *outs; i++) { outs[i]->ev = 0; outs[i]->s = INVALID_SOCKET; }
    HANDLE map = 0;
    if (!strncmp(prefix, "net:", 4)) {
        net = 1;
        if (!open_net(prefix, &map)) return 3;
    } else {
        map = open_obj(prefix, VB_OBJ_MAP, 2);
        sig_ready.ev = open_obj(prefix, VB_OBJ_READY, 0); mutex = open_obj(prefix, VB_OBJ_MUTEX, 1);
        ev_main_h = open_obj(prefix, VB_OBJ_MAIN_H, 0); sig_main_s.ev = open_obj(prefix, VB_OBJ_MAIN_S, 0); sig_main_v.ev = open_obj(prefix, VB_OBJ_MAIN_V, 0);
        ev_audio_h = open_obj(prefix, VB_OBJ_AUDIO_H, 0); sig_audio_s.ev = open_obj(prefix, VB_OBJ_AUDIO_S, 0);
        ev_aux_h = open_obj(prefix, VB_OBJ_AUX_H, 0); sig_aux_s.ev = open_obj(prefix, VB_OBJ_AUX_S, 0);
        if (!map || !sig_ready.ev || !mutex || !ev_main_h || !sig_main_s.ev || !sig_main_v.ev || !ev_audio_h || !sig_audio_s.ev || !ev_aux_h || !sig_aux_s.ev) return 3;
    }
    sh = (VbShared*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!sh || sh->magic != VB_MAGIC || sh->version != VB_VERSION) return 3;
    InitializeCriticalSection(&aux_cs);
    main_tid = GetCurrentThreadId();
    HANDLE hp_ = host_pid ? OpenProcess(SYNCHRONIZE, FALSE, host_pid) : 0;     // net: the closed connections tell instead
    if (hp_) CreateThread(0, 0, watchdog, hp_, 0, 0);

    // the plugin finds its files next to itself (the bridge also starts the helper in the plugin's folder)
    HMODULE dll = LoadLibraryExW(argv[1], 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { char m[200]; snprintf(m, sizeof m, "cannot load the plugin (error %lu)", GetLastError()); fail(m); return 4; }
    VstMain entry = (VstMain)(void*)GetProcAddress(dll, "VSTPluginMain");
    if (!entry) entry = (VstMain)(void*)GetProcAddress(dll, "main");
    if (!entry) { fail("the DLL has no VSTPluginMain: not a VST2 plugin"); return 4; }
    fx = entry(master);
    if (!fx || fx->magic != kEffectMagic) { fail("the plugin did not start (VSTPluginMain returned no plugin)"); return 4; }
    fill_info();
    sh->ok = 1;
    HANDLE at = CreateThread(0, 0, audio_thread, 0, 0, 0);
    sig_set(&sig_ready);

    while (!quitting) {
        DWORD r = MsgWaitForMultipleObjects(1, &ev_main_h, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) serve_top(&sh->main, &main_active, main_to_stub());
        else if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        } else break;
    }
    edit_close();
    InterlockedExchange(&closing, 1);
    InterlockedExchange(&quitting, 1);
    SetEvent(ev_audio_h);
    WaitForSingleObject(at, 2000);
    fx->dispatcher(fx, effClose, 0, 0, 0, 0);
    return 0;
}
