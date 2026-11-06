// stub.c - the bridge: a VST2 plugin (AEffect) that runs the real plugin in a helper process. Built three ways:
//   64-bit Windows DLL: copied as <plugin>_x64.dll next to a 32-bit plugin; runs it in vst2bridge-host32.exe
//   32-bit Windows DLL: copied as <plugin>_x86.dll next to a 64-bit plugin; runs it in vst2bridge-host64.exe
//   Linux / macOS: linked into the CLAP (clap.c, <plugin>.clap); runs the plugin of either bitness in the helper of its
//                  bitness under Wine. Entry vb_stub_main() instead of VSTPluginMain.
// Windows: the host loads the bridge like any VST2 plugin. The bridge finds the real plugin from its own file name
// (syxg50_x64.dll -> syxg50.dll in the same folder), starts the helper of the plugin's bitness and passes every
// call through shared memory (protocol.h); the plugin's calls to the host come back the same way.
// Hosts with direct bridge support can use VST2BridgeCreateA/W to open a plugin in its original folder.
// The transport (named events and a mutex on Windows; loopback sockets, a shared file and a lock word elsewhere) is in
// the two platform blocks below; the calls themselves (exchange, marshalling, audio) are the same on all systems.
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "plat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <wchar.h>
#include "vst2bridge.h"
#endif
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sched.h>
#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char** environ;
#endif
#endif
#include "vst2.h"
#include "protocol.h"
#include "marshal.h"

enum { S_READY, S_MAIN_H, S_MAIN_S, S_MAIN_V, S_AUDIO_H, S_AUDIO_S, S_AUX_H, S_AUX_S, S_N };    // the signals

typedef struct {
    AEffect fx;                         // first member: the host's AEffect* is also the Inst*
    AudioMasterCallback master;
    VbShared* sh;
    volatile int32_t dead, stopping, loading;
    volatile vb_tid main_tid;           // bridge thread taking part in the current main-channel call
    volatile vb_tid process_tid;        // thread that last called process: its calls use the audio channel
    vb_mutex audio_cs, ev_cs;
#ifdef _WIN32
    HANDLE proc, job, map, mutex, sig[S_N];
    HANDLE srv_thread, aux_thread;
    char prefix[64];
    HWND msgwnd;                        // on the host's editor thread: sizeWindow requests are run there
#else
    pid_t pid;
    int fd[4], shm_fd, threads;         // connections M, V, A, X (see protocol.h)
    pthread_t srv_thread, aux_thread;
    char shm_path[PATH_MAX], token[24];
#endif
    ERect rect;
    uint8_t* chunk; size_t chunk_cap;
    uint8_t* spk[2]; size_t spk_cap[2];
    uint32_t n_ev, sysex_n;             // events of effProcessEvents wait here for the next process call
    VbEvent ev[VB_MAXEVENTS_TO_PLUGIN];
    uint8_t sysex[VB_SYSEX_MAX];
    char error[256];
} Inst;

typedef struct {
    int32_t kind, op, index, flags;
    int64_t value;
    float opt;
    const void* in; uint32_t in_len;    // request data
    void* out; uint32_t out_cap;        // where the reply data goes
    uint32_t out_len;
    int64_t ret; float ret_opt;
} Req;

typedef struct {                        // a mapping for one chunk larger than a frame
    void* view; size_t n;
#ifdef _WIN32
    char name[MAX_PATH];
    HANDLE m;
#else
    char name[PATH_MAX > 260 ? PATH_MAX : 260];
    int fd;
#endif
} Chunk;

static void serve_master(Inst* p, VbFrame* f, int on_server);
static int call_main(Inst* p, Req* r);
static void serve_loop(Inst* p, VbChan* ch, int in, int reply, int on_server);
static volatile int32_t g_counter;

typedef struct {
#ifdef _WIN32
    wchar_t plugin[MAX_PATH], helper[MAX_PATH], working_dir[MAX_PATH];
#else
    const char* dir;
    const char* stem;
#endif
} StartPaths;

#ifdef _WIN32
// ============================================================================= Windows: named objects

#ifdef _WIN64
#define VB_SUFFIX   L"_x64"
#define VB_HELPER   L"vst2bridge-host32.exe"
#else
#define VB_SUFFIX   L"_x86"
#define VB_HELPER   L"vst2bridge-host64.exe"
#endif
#define VB_SIZEWINDOW_MSG (WM_APP + 1)

static HMODULE g_self;
static const char* const g_sig_names[S_N] = { VB_OBJ_READY, VB_OBJ_MAIN_H, VB_OBJ_MAIN_S, VB_OBJ_MAIN_V, VB_OBJ_AUDIO_H, VB_OBJ_AUDIO_S, VB_OBJ_AUX_H, VB_OBJ_AUX_S };

static void log_error(const StartPaths* paths, const char* m)
{
    wchar_t file[MAX_PATH + 32];
    DWORD n = GetTempPathW(MAX_PATH, file);
    if (!n || n >= MAX_PATH) return;
    wcscpy(file + n, L"vst2bridge.log");
    FILE* fp = _wfopen(file, L"a");
    if (fp) {
        char name[MAX_PATH * 3];
        int converted = WideCharToMultiByte(CP_UTF8, 0, paths->plugin, -1, name, sizeof name, 0, 0);
        fprintf(fp, "%s: %s\n", converted ? name : "VST2Bridge", m);
        fclose(fp);
    }
}

static int file_exists(const wchar_t* f) { DWORD a = GetFileAttributesW(f); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

static void sig_send(Inst* p, int s) { SetEvent(p->sig[s]); }

// waits for signal s; answers messages sent to this thread's windows meanwhile (pump). 0 = the helper is gone.
static int chan_wait(Inst* p, int s, int pump)
{
    HANDLE w[2] = { p->sig[s], p->proc };
    for (;;) {
        DWORD r = pump ? MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE) : WaitForMultipleObjects(2, w, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) return !p->dead;
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }   // runs the sent messages
        vb_xchg(&p->dead, 1);
        return 0;
    }
}

static int lock_main(Inst* p)
{
    HANDLE w[2] = { p->mutex, p->proc };
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(2, w, FALSE, INFINITE, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED_0) return 1;
        if (r == WAIT_OBJECT_0 + 2) { MSG m; PeekMessageW(&m, 0, 0, 0, PM_NOREMOVE); continue; }
        vb_xchg(&p->dead, 1);
        return 0;
    }
}
static void unlock_main(Inst* p) { ReleaseMutex(p->mutex); }

static LRESULT CALLBACK msg_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == VB_SIZEWINDOW_MSG) {
        Inst* p = (Inst*)GetWindowLongPtrW(w, GWLP_USERDATA);
        return p ? (LRESULT)p->master(&p->fx, audioMasterSizeWindow, (int32_t)wp, (intptr_t)lp, 0, 0) : 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static void make_msgwnd(Inst* p)
{
    static int registered;
    if (p->msgwnd) return;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = msg_proc; wc.hInstance = g_self; wc.lpszClassName = L"VST2BridgeMsg";
        RegisterClassW(&wc); registered = 1;
    }
    p->msgwnd = CreateWindowExW(0, L"VST2BridgeMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, g_self, 0);
    if (p->msgwnd) SetWindowLongPtrW(p->msgwnd, GWLP_USERDATA, (LONG_PTR)p);
}

// audioMasterSizeWindow started by the plugin's GUI (server thread): run it on the host's editor thread, where hosts
// expect window changes
static intptr_t size_window(Inst* p, AEffect* fx, VbFrame* f, int on_server)
{
    if (!on_server || !p->msgwnd) return p->master(fx, f->op, f->index, (intptr_t)f->value, 0, f->opt);
    DWORD_PTR res = 0;
    DWORD gui = GetWindowThreadProcessId(p->msgwnd, 0);
    vb_tid prev = p->main_tid;
    p->main_tid = gui;                                              // calls the host makes meanwhile nest in this call
    if (!SendMessageTimeoutW(p->msgwnd, VB_SIZEWINDOW_MSG, (WPARAM)f->index, (LPARAM)f->value, SMTO_NORMAL, 5000, &res)) res = 0;
    p->main_tid = prev;
    return (intptr_t)res;
}

static DWORD WINAPI server_thread(LPVOID a) { Inst* p = (Inst*)a; serve_loop(p, &p->sh->main, S_MAIN_V, S_MAIN_H, 1); return 0; }
static DWORD WINAPI aux_thread(LPVOID a) { Inst* p = (Inst*)a; serve_loop(p, &p->sh->aux, S_AUX_S, S_AUX_H, 0); return 0; }

static int plat_start(Inst* p, const StartPaths* paths)
{
    wchar_t cmd[4 * MAX_PATH], wpfx[64];
    if (!file_exists(paths->plugin)) { snprintf(p->error, sizeof p->error, "plugin DLL not found"); return 0; }
    if (!file_exists(paths->helper)) { snprintf(p->error, sizeof p->error, "%ls not found next to the bridge", VB_HELPER); return 0; }

    snprintf(p->prefix, sizeof p->prefix, "Local\\V2B%lu_%ld_", GetCurrentProcessId(), (long)vb_inc(&g_counter));
    MultiByteToWideChar(CP_ACP, 0, p->prefix, -1, wpfx, 64);
    char nm[128];
    #define OBJ(sfx) (snprintf(nm, sizeof nm, "%s%s", p->prefix, sfx), nm)
    p->map = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, (DWORD)sizeof(VbShared), OBJ(VB_OBJ_MAP));
    p->mutex = CreateMutexA(0, FALSE, OBJ(VB_OBJ_MUTEX));
    int ok = p->map && p->mutex;
    for (int s = 0; s < S_N; s++) { p->sig[s] = CreateEventA(0, FALSE, FALSE, OBJ(g_sig_names[s])); ok = ok && p->sig[s]; }
    #undef OBJ
    if (!ok) { snprintf(p->error, sizeof p->error, "cannot create shared objects (error %lu)", GetLastError()); return 0; }
    p->sh = (VbShared*)MapViewOfFile(p->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p->sh) { snprintf(p->error, sizeof p->error, "cannot map shared memory"); return 0; }
    p->sh->magic = VB_MAGIC; p->sh->version = VB_VERSION;

    // shell plugins ask for the id of the sub-plugin while they are created; the helper answers with this one
    int32_t shell_id = p->master ? (int32_t)p->master(0, audioMasterCurrentId, 0, 0, 0, 0) : 0;
    _snwprintf(cmd, 4 * MAX_PATH, L"\"%ls\" \"%ls\" %ls %ld %lu",
               paths->helper, paths->plugin, wpfx, (long)shell_id, GetCurrentProcessId());

    p->job = CreateJobObjectW(0, 0);                                // the helper ends with the host, even if the host crashes
    if (p->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessW(paths->helper, cmd, 0, 0, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED, 0, paths->working_dir, &si, &pi)
        && !CreateProcessW(paths->helper, cmd, 0, 0, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, 0, paths->working_dir, &si, &pi)) {
        snprintf(p->error, sizeof p->error, "cannot start %ls (error %lu)", VB_HELPER, GetLastError()); return 0;
    }
    if (p->job) AssignProcessToJobObject(p->job, pi.hProcess);
    p->proc = pi.hProcess;
    p->srv_thread = CreateThread(0, 0, server_thread, p, 0, 0);
    p->aux_thread = CreateThread(0, 0, aux_thread, p, 0, 0);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);

    HANDLE w[2] = { p->sig[S_READY], p->proc };
    DWORD r = WaitForMultipleObjects(2, w, FALSE, 120000);          // big sample sets: loading can take a while
    if (r != WAIT_OBJECT_0) {
        snprintf(p->error, sizeof p->error, r == WAIT_OBJECT_0 + 1 ? "the helper ended while loading the plugin" : "the plugin took too long to load");
        return 0;
    }
    return 1;
}

static void plat_stop(Inst* p)
{
    if (p->proc && !p->dead && p->sh && p->sh->ok) {
        Req r; memset(&r, 0, sizeof r); r.kind = VB_K_QUIT;
        call_main(p, &r);
        if (WaitForSingleObject(p->proc, 5000) != WAIT_OBJECT_0) TerminateProcess(p->proc, 0);
    } else if (p->proc) TerminateProcess(p->proc, 0);
    vb_xchg(&p->stopping, 1);
    vb_xchg(&p->dead, 1);
    if (p->sig[S_MAIN_V]) SetEvent(p->sig[S_MAIN_V]);
    if (p->sig[S_AUX_S]) SetEvent(p->sig[S_AUX_S]);
    if (p->srv_thread) { WaitForSingleObject(p->srv_thread, 3000); CloseHandle(p->srv_thread); }
    if (p->aux_thread) { WaitForSingleObject(p->aux_thread, 3000); CloseHandle(p->aux_thread); }
    if (p->sh) UnmapViewOfFile(p->sh);
    HANDLE* h[] = { &p->proc, &p->job, &p->map, &p->mutex };
    for (size_t i = 0; i < sizeof h / sizeof *h; i++) if (*h[i]) { CloseHandle(*h[i]); *h[i] = 0; }
    for (int s = 0; s < S_N; s++) if (p->sig[s]) { CloseHandle(p->sig[s]); p->sig[s] = 0; }
    p->sh = 0;
    if (p->msgwnd) {
        SetWindowLongPtrW(p->msgwnd, GWLP_USERDATA, 0);
        if (GetWindowThreadProcessId(p->msgwnd, 0) == GetCurrentThreadId()) DestroyWindow(p->msgwnd);
        else PostMessageW(p->msgwnd, WM_CLOSE, 0, 0);
        p->msgwnd = 0;
    }
}

static int chunk_map(Inst* p, size_t n, Chunk* c)
{
    snprintf(c->name, sizeof c->name, "%s%s%ld", p->prefix, VB_OBJ_CHUNK, (long)vb_inc(&g_counter));
    c->n = n;
    c->m = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, (DWORD)((uint64_t)n >> 32), (DWORD)n, c->name);
    c->view = c->m ? MapViewOfFile(c->m, FILE_MAP_ALL_ACCESS, 0, 0, 0) : 0;
    if (c->m && !c->view) { CloseHandle(c->m); c->m = 0; }
    return c->view != 0;
}
static void chunk_unmap(Chunk* c) { UnmapViewOfFile(c->view); CloseHandle(c->m); }

#else
// ============================================================================= Linux / macOS: Wine, sockets, a shared file

static const int g_sig_fd[S_N] = { 0, 0, 0, 1, 2, 2, 3, 3 };       // M M M V A A X X
#ifdef __APPLE__
#define SEND_FLAGS 0                                                // SO_NOSIGPIPE is set on the sockets instead
#else
#define SEND_FLAGS MSG_NOSIGNAL                                     // a helper that is gone must not kill the host with SIGPIPE
#endif

static void log_error(const StartPaths* paths, const char* m)
{
    const char* t = getenv("TMPDIR");
    char f[PATH_MAX];
    snprintf(f, sizeof f, "%s/vst2bridge.log", t && *t ? t : "/tmp");
    FILE* fp = fopen(f, "a");
    if (fp) { fprintf(fp, "%s.clap: %s\n", paths->stem, m); fclose(fp); }
}

static int file_exists(const char* f) { struct stat st; return stat(f, &st) == 0 && S_ISREG(st.st_mode); }

static void sig_send(Inst* p, int s) { char b = 1; ssize_t n = send(p->fd[g_sig_fd[s]], &b, 1, SEND_FLAGS); (void)n; }

// one byte on the connection of signal s; 0 = the helper is gone (connection closed)
static int chan_wait(Inst* p, int s, int pump)
{
    (void)pump;
    char b;
    for (;;) {
        ssize_t n = recv(p->fd[g_sig_fd[s]], &b, 1, 0);
        if (n == 1) return !vb_load(&p->dead);
        if (n < 0 && errno == EINTR) continue;
        vb_xchg(&p->dead, 1);
        return 0;
    }
}

// the lock word in shared memory replaces the named mutex (contention is rare)
static int lock_main(Inst* p)
{
    for (int i = 0; ; i++) {
        if (vb_cas(&p->sh->main.lock, 0, VB_OWNER_STUB)) return 1;
        if (p->dead) return 0;
        if (i < 50) sched_yield(); else usleep(200);
    }
}
static void unlock_main(Inst* p) { vb_xchg(&p->sh->main.lock, 0); }

// CLAP hosts take gui.request_resize from any thread (clap.c), so the plugin's sizeWindow goes straight on
static intptr_t size_window(Inst* p, AEffect* fx, VbFrame* f, int on_server)
{
    (void)on_server;
    return p->master(fx, f->op, f->index, (intptr_t)f->value, 0, f->opt);
}

static void* server_thread(void* a) { Inst* p = (Inst*)a; serve_loop(p, &p->sh->main, S_MAIN_V, S_MAIN_H, 1); return 0; }
static void* aux_thread(void* a) { Inst* p = (Inst*)a; serve_loop(p, &p->sh->aux, S_AUX_S, S_AUX_H, 0); return 0; }

static void tune_socket(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static int helper_gone(Inst* p)
{
    if (p->pid > 0 && waitpid(p->pid, 0, WNOHANG) == p->pid) p->pid = 0;
    return p->pid <= 0;
}

// wine binary: VB_WINE, then (macOS: a host's PATH is short) the usual install folders, then wine in PATH
static const char* find_wine(void)
{
    const char* e = getenv("VB_WINE");
    if (e && *e) return e;
#ifdef __APPLE__
    static const char* cand[] = {
        "/opt/homebrew/bin/wine", "/usr/local/bin/wine", "/opt/homebrew/bin/wine64", "/usr/local/bin/wine64",
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine", "/Applications/Wine Staging.app/Contents/Resources/wine/bin/wine",
        "/Applications/Wine Devel.app/Contents/Resources/wine/bin/wine",
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine", 0
    };
    for (int i = 0; cand[i]; i++) if (access(cand[i], X_OK) == 0) return cand[i];
#endif
    return "wine";
}

// 32 or 64 from the machine field of a Windows DLL (0: not an x86 / x64 DLL)
static int pe_bits(const char* f)
{
    unsigned char h[64], m[6];
    FILE* fp = fopen(f, "rb");
    if (!fp) return 0;
    int bits = 0;
    if (fread(h, 1, 64, fp) == 64 && h[0] == 'M' && h[1] == 'Z') {
        long pe = (long)(h[60] | h[61] << 8 | h[62] << 16 | (unsigned)h[63] << 24);
        if (pe > 0 && fseek(fp, pe, SEEK_SET) == 0 && fread(m, 1, 6, fp) == 6 && !memcmp(m, "PE\0\0", 4)) {
            unsigned mach = m[4] | m[5] << 8;
            bits = mach == 0x014C ? 32 : mach == 0x8664 ? 64 : 0;
        }
    }
    fclose(fp);
    return bits;
}

static const char* shm_dir(char* buf, size_t n)
{
#ifdef __APPLE__
    const char* t = getenv("TMPDIR");
    if (!t || !*t || !realpath(t, buf)) snprintf(buf, n, "/tmp");
    return buf;
#else
    (void)n; (void)buf;
    return "/dev/shm";                                              // Wine maps it like any folder (drive Z:)
#endif
}

static int plat_start(Inst* p, const StartPaths* paths)
{
    char dll[PATH_MAX], exe[PATH_MAX], t[PATH_MAX + 64], tdir[PATH_MAX];
    for (int i = 0; i < 4; i++) p->fd[i] = -1;
    p->shm_fd = -1;
    snprintf(t, sizeof t, "%s/%s.dll", paths->dir, paths->stem);
    if (!file_exists(t) || !realpath(t, dll)) { snprintf(p->error, sizeof p->error, "plugin %s.dll not found next to the CLAP", paths->stem); return 0; }
    int bits = pe_bits(dll);
    if (!bits) { snprintf(p->error, sizeof p->error, "%s.dll is not a 32-bit or 64-bit Windows plugin", paths->stem); return 0; }
    const char* helper = bits == 32 ? "vst2bridge-host32.exe" : "vst2bridge-host64.exe";
    snprintf(t, sizeof t, "%s/%s", paths->dir, helper);
    if (!file_exists(t) || !realpath(t, exe)) { snprintf(p->error, sizeof p->error, "%s not found next to the CLAP", helper); return 0; }

    snprintf(p->shm_path, sizeof p->shm_path, "%.900s/vst2bridge_%d_%d", shm_dir(tdir, sizeof tdir), (int)getpid(), (int)vb_inc(&g_counter));
    p->shm_fd = open(p->shm_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (p->shm_fd < 0 || ftruncate(p->shm_fd, sizeof(VbShared)) != 0) { snprintf(p->error, sizeof p->error, "cannot create %s", p->shm_path); return 0; }
    void* v = mmap(0, sizeof(VbShared), PROT_READ | PROT_WRITE, MAP_SHARED, p->shm_fd, 0);
    if (v == MAP_FAILED) { snprintf(p->error, sizeof p->error, "cannot map shared memory"); return 0; }
    p->sh = (VbShared*)v;
    p->sh->magic = VB_MAGIC; p->sh->version = VB_VERSION;

    unsigned char rnd[8] = {0};
    int ur = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (ur >= 0) { ssize_t k = read(ur, rnd, sizeof rnd); (void)k; close(ur); }
    for (int i = 0; i < 8; i++) snprintf(p->token + 2 * i, 3, "%02x", rnd[i]);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls >= 0) tune_socket(ls);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (ls < 0 || bind(ls, (struct sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0 || getsockname(ls, (struct sockaddr*)&a, &al) != 0) {
        snprintf(p->error, sizeof p->error, "cannot open a local socket"); if (ls >= 0) close(ls); return 0;
    }

    // wine <helper> Z:\path\plugin.dll net:<port>:<token>:<shared file> <shell id> 0
    char wdll[PATH_MAX + 4], arg2[PATH_MAX + 64], sid[24];
    snprintf(wdll, sizeof wdll, "Z:%s", dll);
    for (char* q = wdll; *q; q++) if (*q == '/') *q = '\\';
    snprintf(arg2, sizeof arg2, "net:%d:%s:%s", ntohs(a.sin_port), p->token, p->shm_path);
    int32_t shell_id = p->master ? (int32_t)p->master(0, audioMasterCurrentId, 0, 0, 0, 0) : 0;
    snprintf(sid, sizeof sid, "%ld", (long)shell_id);
    const char* wine = find_wine();
    char* argv[] = { (char*)wine, exe, wdll, arg2, sid, (char*)"0", 0 };
    int ne = 0; while (environ[ne]) ne++;
    char** envp = (char**)calloc((size_t)ne + 2, sizeof *envp);
    if (!envp) { close(ls); snprintf(p->error, sizeof p->error, "out of memory"); return 0; }
    int has_dbg = 0;
    for (int i = 0; i < ne; i++) { envp[i] = environ[i]; if (!strncmp(environ[i], "WINEDEBUG=", 10)) has_dbg = 1; }
    if (!has_dbg) envp[ne] = (char*)"WINEDEBUG=-all";
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addchdir_np(&fa, paths->dir);         // the plugin finds its files next to itself
    if (!getenv("VB_DEBUG")) {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    int sr = posix_spawnp(&p->pid, wine, &fa, 0, argv, envp);
    posix_spawn_file_actions_destroy(&fa); free(envp);
    if (sr != 0) { p->pid = 0; close(ls); snprintf(p->error, sizeof p->error, "cannot start Wine (%s): %s", wine, strerror(sr)); return 0; }

    // the helper connects four times (M, V, A, X), each time with the token and the kind
    for (int i = 0; i < 4; i++) {
        struct pollfd pf = { ls, POLLIN, 0 };
        int waited = 0, got = -1;
        while (waited < 120000) {
            if (poll(&pf, 1, 200) > 0) { got = accept(ls, 0, 0); break; }
            waited += 200;
            if (helper_gone(p)) break;
        }
        if (got < 0) {
            close(ls);
            snprintf(p->error, sizeof p->error, p->pid > 0 ? "Wine did not start the helper in time"
                     : "Wine ended without starting the helper (is Wine installed? see VB_WINE and VB_DEBUG in the documentation)");
            return 0;
        }
        char hello[24] = {0}; struct pollfd hp = { got, POLLIN, 0 };
        const char* kinds = "MVAX"; const char* k = 0;
        if (poll(&hp, 1, 5000) > 0 && read(got, hello, 17) == 17 && !strncmp(hello, p->token, 16)) k = strchr(kinds, hello[16]);
        if (!k || !hello[16] || p->fd[k - kinds] >= 0) { close(got); close(ls); snprintf(p->error, sizeof p->error, "unexpected connection"); return 0; }
        tune_socket(got);
        p->fd[k - kinds] = got;
    }
    close(ls);
    // the plugin may call the host while it loads (main channel, started by the helper): serve that from now on
    if (pthread_create(&p->srv_thread, 0, server_thread, p) == 0) p->threads |= 1;
    if (pthread_create(&p->aux_thread, 0, aux_thread, p) == 0) p->threads |= 2;
    struct pollfd rp = { p->fd[0], POLLIN, 0 };
    for (int waited = 0; ; waited += 200) {                         // "ready" (big sample sets: loading can take a while)
        if (poll(&rp, 1, 200) > 0) {
            char b;
            if (recv(p->fd[0], &b, 1, 0) == 1) break;
            snprintf(p->error, sizeof p->error, "the helper ended while loading the plugin"); return 0;
        }
        if (waited >= 120000) { snprintf(p->error, sizeof p->error, "the plugin took too long to load"); return 0; }
        if (helper_gone(p)) { snprintf(p->error, sizeof p->error, "the helper ended while loading the plugin"); return 0; }
    }
    unlink(p->shm_path); p->shm_path[0] = 0;                       // both sides have it open
    return 1;
}

static void plat_stop(Inst* p)
{
    if (p->pid > 0) {
        if (!p->dead && p->sh && p->sh->ok) {
            Req r; memset(&r, 0, sizeof r); r.kind = VB_K_QUIT;
            call_main(p, &r);
            for (int i = 0; i < 50 && !helper_gone(p); i++) usleep(100000);
        }
        if (p->pid > 0) { kill(p->pid, SIGKILL); waitpid(p->pid, 0, 0); p->pid = 0; }
    }
    vb_xchg(&p->stopping, 1);
    vb_xchg(&p->dead, 1);
    for (int i = 0; i < 4; i++) if (p->fd[i] >= 0) shutdown(p->fd[i], SHUT_RDWR);     // wakes the server threads
    if (p->threads & 1) pthread_join(p->srv_thread, 0);
    if (p->threads & 2) pthread_join(p->aux_thread, 0);
    p->threads = 0;
    for (int i = 0; i < 4; i++) if (p->fd[i] >= 0) { close(p->fd[i]); p->fd[i] = -1; }
    if (p->sh) munmap(p->sh, sizeof(VbShared));
    p->sh = 0;
    if (p->shm_fd >= 0) close(p->shm_fd);
    p->shm_fd = -1;
    if (p->shm_path[0]) { unlink(p->shm_path); p->shm_path[0] = 0; }
}

static int chunk_map(Inst* p, size_t n, Chunk* c)
{
    char tdir[PATH_MAX];
    snprintf(c->name, sizeof c->name, "%.900s/vst2bridge_%d_%s_c%d", shm_dir(tdir, sizeof tdir), (int)getpid(), p->token, (int)vb_inc(&g_counter));
    c->n = n; c->view = 0;
    c->fd = open(c->name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (c->fd < 0) return 0;
    void* v = ftruncate(c->fd, (off_t)n) == 0 ? mmap(0, n, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0) : MAP_FAILED;
    if (v == MAP_FAILED) { close(c->fd); unlink(c->name); return 0; }
    c->view = v;
    return 1;
}
static void chunk_unmap(Chunk* c) { munmap(c->view, c->n); close(c->fd); unlink(c->name); }
#endif

// ============================================================================= calls (all systems)

static void sync_info(Inst* p)
{
    const VbInfo* i = &p->sh->info;
    AEffect* fx = &p->fx;
    fx->numPrograms = i->numPrograms; fx->numParams = i->numParams;
    fx->numInputs = i->numInputs; fx->numOutputs = i->numOutputs;
    fx->flags = (i->flags | effFlagsCanReplacing) & ~(i->has_double ? 0 : effFlagsCanDoubleReplacing);
    fx->initialDelay = i->initialDelay; fx->uniqueID = i->uniqueID; fx->version = i->version; fx->ioRatio = i->ioRatio;
}

// one call: push r on ch, signal `to`, wait on `in` for the reply and serve the helper's nested calls meanwhile
static int exchange(Inst* p, VbChan* ch, int to, int in, int pump, Req* r)
{
    if (p->dead) return 0;
    int d = ch->depth;
    if (d >= VB_MAXDEPTH || r->in_len > VB_FRAME_DATA) return 0;
    VbFrame* f = &ch->f[d];
    f->kind = r->kind; f->op = r->op; f->index = r->index; f->flags = r->flags;
    f->value = r->value; f->opt = r->opt; f->ret = 0;
    f->len = r->in_len;
    if (r->in_len) memcpy(f->data, r->in, r->in_len);
    ch->depth = d + 1;
    vb_barrier();
    ch->msg = VB_MSG_REQ;
    sig_send(p, to);
    for (;;) {
        if (!chan_wait(p, in, pump)) return 0;
        vb_barrier();
        if (ch->msg == VB_MSG_REPLY && ch->depth == d + 1) break;
        if (ch->msg == VB_MSG_REQ && ch->depth > d + 1) {           // the plugin calls the host
            serve_master(p, &ch->f[ch->depth - 1], 0);
            vb_barrier();
            ch->msg = VB_MSG_REPLY;
            sig_send(p, to);
        }
    }
    sync_info(p);
    r->ret = f->ret; r->ret_opt = f->opt;
    r->out_len = f->len < r->out_cap ? f->len : r->out_cap;
    if (r->out && r->out_len) memcpy(r->out, f->data, r->out_len);
    ch->depth = d;
    return 1;
}

static int call_main(Inst* p, Req* r)
{
    VbChan* ch = &p->sh->main;
    vb_tid me = vb_self();
    if (p->main_tid == me)                                          // nested in a call this thread takes part in
        return exchange(p, ch, S_MAIN_H, ch->owner == VB_OWNER_HELPER ? S_MAIN_V : S_MAIN_S, 1, r);
    if (!lock_main(p)) return 0;
    p->main_tid = me; ch->owner = VB_OWNER_STUB;
    int ok = exchange(p, ch, S_MAIN_H, S_MAIN_S, 1, r);
    ch->owner = VB_OWNER_NONE; p->main_tid = 0;
    unlock_main(p);
    return ok;
}

static int call_audio(Inst* p, Req* r)
{
    vb_lock(&p->audio_cs);                                          // recursive: nested calls of the same thread pass
    int ok = exchange(p, &p->sh->audio, S_AUDIO_H, S_AUDIO_S, 0, r);
    vb_unlock(&p->audio_cs);
    return ok;
}

// calls made on the thread that runs process go to the helper's audio thread, all others to its main thread
static int call(Inst* p, Req* r, int force_main)
{
    if (p->dead) return 0;
    if (!force_main && vb_self() == p->process_tid) return call_audio(p, r);
    return call_main(p, r);
}

// ----------------------------------------------------------------------------- plugin -> host

static void serve_master(Inst* p, VbFrame* f, int on_server)
{
    int size, m = master_marshal(f->op, &size);
    AEffect* fx = p->loading ? 0 : &p->fx;                          // plugins pass NULL while they are being created
    intptr_t ret = 0, val = (intptr_t)f->value;
    uint32_t in_len = f->len;
    sync_info(p);
    f->len = 0;
    switch (m) {
    case M_NONE: ret = p->master(fx, f->op, f->index, val, 0, f->opt); break;
    case M_STR_IN:
        if (in_len) f->data[in_len - 1] = 0;
        ret = p->master(fx, f->op, f->index, val, in_len ? f->data : 0, f->opt);
        break;
    case M_STR_OUT: {
        char buf[VB_STR_MAX]; memset(buf, 0, sizeof buf);
        ret = p->master(fx, f->op, f->index, val, buf, f->opt);
        buf[VB_STR_MAX - 1] = 0;
        f->len = (uint32_t)strlen(buf) + 1; memcpy(f->data, buf, f->len);
        break;
    }
    case M_SPECIAL:
        switch (f->op) {
        case audioMasterGetTime: {
            VstTimeInfo* t = (VstTimeInfo*)p->master(fx, f->op, f->index, val, 0, f->opt);
            if (t) { memcpy(f->data, t, sizeof *t); f->len = sizeof *t; ret = 1; }
            break;
        }
        case audioMasterGetDirectory: {
            const char* s = (const char*)p->master(fx, f->op, f->index, val, 0, f->opt);
            if (s) { size_t n = strnlen(s, VB_STR_MAX - 1); memcpy(f->data, s, n); f->data[n] = 0; f->len = (uint32_t)n + 1; ret = 1; }
            break;
        }
        case audioMasterProcessEvents: {                            // MIDI sent by the plugin
            uint32_t n = 0;
            if (in_len >= 4) memcpy(&n, f->data, 4);
            if (n > VB_MAXEVENTS_TO_HOST || 4 + (size_t)n * sizeof(VbEvent) > in_len) break;
            const VbEvent* e = (const VbEvent*)(f->data + 4);
            const uint8_t* sx = f->data + 4 + n * sizeof(VbEvent);
            size_t sx_len = in_len - 4 - n * sizeof(VbEvent);
            VstEvents* list = (VstEvents*)calloc(1, sizeof(VstEvents) + n * sizeof(VstEvent*));
            VstMidiEvent* me = (VstMidiEvent*)calloc(n ? n : 1, sizeof *me);
            VstMidiSysexEvent* se = (VstMidiSysexEvent*)calloc(n ? n : 1, sizeof *se);
            if (list && me && se) {
                int cnt = 0;
                for (uint32_t i = 0; i < n; i++) {
                    if (e[i].type == kVstSysExType) {
                        if ((uint64_t)e[i].sysex_off + e[i].sysex_len > sx_len) continue;
                        se[i].type = kVstSysExType; se[i].byteSize = sizeof se[i]; se[i].deltaFrames = e[i].delta; se[i].flags = e[i].flags;
                        se[i].dumpBytes = (int32_t)e[i].sysex_len; se[i].sysexDump = (char*)sx + e[i].sysex_off;
                        list->events[cnt++] = (VstEvent*)&se[i];
                    } else {
                        me[i].type = kVstMidiType; me[i].byteSize = sizeof me[i]; me[i].deltaFrames = e[i].delta; me[i].flags = e[i].flags;
                        me[i].noteLength = e[i].note_length; me[i].noteOffset = e[i].note_offset;
                        memcpy(me[i].midiData, e[i].midi, 4); me[i].detune = e[i].detune; me[i].noteOffVelocity = (char)e[i].off_velocity;
                        list->events[cnt++] = (VstEvent*)&me[i];
                    }
                }
                list->numEvents = cnt;
                ret = p->master(fx, f->op, f->index, val, list, f->opt);
            }
            free(list); free(me); free(se);
            break;
        }
        case audioMasterVendorSpecific:                             // the helper sends only calls with ptr = NULL
            ret = p->master(fx, f->op, f->index, val, 0, f->opt);
            break;
        case audioMasterSizeWindow:
            ret = size_window(p, fx, f, on_server);
            break;
        }
        break;
    default: break;                                                 // M_REFUSE (the helper does not send these)
    }
    f->ret = ret;
}

// helper-started calls on the main channel (the plugin's GUI, timers ...: server thread) and calls from other plugin
// threads (aux thread)
static void serve_loop(Inst* p, VbChan* ch, int in, int reply, int on_server)
{
    while (chan_wait(p, in, 0) && !p->stopping) {
        vb_barrier();
        if (ch->msg != VB_MSG_REQ || ch->depth < 1) continue;
        if (on_server) p->main_tid = vb_self();
        serve_master(p, &ch->f[ch->depth - 1], on_server);
        if (on_server) p->main_tid = 0;
        vb_barrier();
        ch->msg = VB_MSG_REPLY;
        sig_send(p, reply);
    }
}

// ----------------------------------------------------------------------------- start / stop

static int start(Inst* p, const StartPaths* paths)
{
    if (!plat_start(p, paths)) return 0;
    if (!p->sh->ok) { snprintf(p->error, sizeof p->error, "%s", p->sh->error[0] ? p->sh->error : "the plugin could not be loaded"); return 0; }
    return 1;
}

// ----------------------------------------------------------------------------- host -> plugin

static void queue_events(Inst* p, const VstEvents* evs)
{
    vb_lock(&p->ev_cs);
    for (int i = 0; i < evs->numEvents; i++) {
        const VstEvent* e = evs->events[i];
        if (!e || p->n_ev >= VB_MAXEVENTS_TO_PLUGIN) continue;
        VbEvent* q = &p->ev[p->n_ev];
        memset(q, 0, sizeof *q);
        q->type = e->type; q->delta = e->deltaFrames < 0 ? 0 : e->deltaFrames; q->flags = e->flags;
        if (e->type == kVstMidiType) {
            const VstMidiEvent* m = (const VstMidiEvent*)e;
            memcpy(q->midi, m->midiData, 4); q->note_length = m->noteLength; q->note_offset = m->noteOffset;
            q->detune = m->detune; q->off_velocity = (uint8_t)m->noteOffVelocity;
        } else if (e->type == kVstSysExType) {
            const VstMidiSysexEvent* s = (const VstMidiSysexEvent*)e;
            uint32_t n = s->dumpBytes > 0 ? (uint32_t)s->dumpBytes : 0;
            if (!s->sysexDump || !n || p->sysex_n + n > VB_SYSEX_MAX) continue;
            q->sysex_off = p->sysex_n; q->sysex_len = n;
            memcpy(p->sysex + p->sysex_n, s->sysexDump, n); p->sysex_n += n;
        } else continue;                                            // other event types are not passed on
        p->n_ev++;
    }
    vb_unlock(&p->ev_cs);
}

static void process(Inst* p, void** in, void** out, int32_t frames, int mode)
{
    size_t es = mode == VB_P_DOUBLE ? sizeof(double) : sizeof(float);
    int nin = p->fx.numInputs < VB_MAXCH ? p->fx.numInputs : VB_MAXCH;
    int nout = p->fx.numOutputs < VB_MAXCH ? p->fx.numOutputs : VB_MAXCH;
    uint32_t total = frames > 0 ? (uint32_t)frames : 0, pos = 0, ei = 0;
    vb_lock(&p->audio_cs);
    vb_lock(&p->ev_cs);
    p->process_tid = vb_self();
    VbAudio* a = &p->sh->au;
    while (pos < total) {
        uint32_t n = total - pos > VB_MAXFRAMES ? VB_MAXFRAMES : total - pos;
        int ok = !p->dead;
        if (ok) {
            a->nin = nin; a->nout = nout; a->n_events = 0; a->sysex_bytes = 0;
            for (int c = 0; c < nin; c++) {
                if (in && in[c]) memcpy(a->in[c], (char*)in[c] + pos * es, n * es);
                else memset(a->in[c], 0, n * es);
            }
            while (ei < p->n_ev) {                                  // the events that fall into this piece
                const VbEvent* e = &p->ev[ei];
                if (pos + n < total && (uint32_t)e->delta >= pos + n) break;
                VbEvent* d = &a->events[a->n_events++];
                *d = *e; d->delta = (uint32_t)e->delta > pos ? e->delta - (int32_t)pos : 0;
                if ((uint32_t)d->delta >= n) d->delta = (int32_t)n - 1;
                if (e->type == kVstSysExType) {
                    d->sysex_off = a->sysex_bytes;
                    memcpy(a->sysex + a->sysex_bytes, p->sysex + e->sysex_off, e->sysex_len);
                    a->sysex_bytes += e->sysex_len;
                }
                ei++;
            }
            Req r; memset(&r, 0, sizeof r);
            r.kind = VB_K_PROCESS; r.op = mode; r.value = n;
            ok = call_audio(p, &r);
        }
        for (int c = 0; c < p->fx.numOutputs; c++) {
            if (!out || !out[c]) continue;
            char* o = (char*)out[c] + pos * es;
            if (!ok || c >= nout) { if (mode != VB_P_ACCUMULATE) memset(o, 0, n * es); }
            else if (mode == VB_P_ACCUMULATE) { float* of = (float*)o; const float* s = (const float*)a->out[c]; for (uint32_t i = 0; i < n; i++) of[i] += s[i]; }
            else memcpy(o, a->out[c], n * es);
        }
        pos += n;
    }
    p->n_ev = 0; p->sysex_n = 0;
    vb_unlock(&p->ev_cs);
    vb_unlock(&p->audio_cs);
}

static void VSTCALL vst_process(AEffect* fx, float** in, float** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_ACCUMULATE); }
static void VSTCALL vst_process_replacing(AEffect* fx, float** in, float** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_REPLACING); }
static void VSTCALL vst_process_double(AEffect* fx, double** in, double** out, int32_t n) { process((Inst*)fx, (void**)in, (void**)out, n, VB_P_DOUBLE); }

static void VSTCALL vst_set_parameter(AEffect* fx, int32_t i, float v)
{
    Req r; memset(&r, 0, sizeof r); r.kind = VB_K_SETPARAM; r.index = i; r.opt = v;
    call((Inst*)fx, &r, 0);
}
static float VSTCALL vst_get_parameter(AEffect* fx, int32_t i)
{
    Req r; memset(&r, 0, sizeof r); r.kind = VB_K_GETPARAM; r.index = i;
    return call((Inst*)fx, &r, 0) ? r.ret_opt : 0.0f;
}

static void inst_free(Inst* p)
{
    vb_mutex_free(&p->audio_cs); vb_mutex_free(&p->ev_cs);
    free(p->chunk); free(p->spk[0]); free(p->spk[1]); free(p);
}

static intptr_t special(Inst* p, Req* r, void* ptr, intptr_t value)
{
    switch (r->op) {
    case effClose:
        plat_stop(p);
        inst_free(p);
        return 1;
    case effProcessEvents:
        if (ptr) queue_events(p, (const VstEvents*)ptr);
        return 1;
    case effEditGetRect: {
        ERect rc;
        r->out = &rc; r->out_cap = sizeof rc;
        if (!call(p, r, 1)) return 0;
        if (r->out_len == sizeof rc) { p->rect = rc; if (ptr) *(ERect**)ptr = &p->rect; }
        return (intptr_t)r->ret;
    }
    case effEditOpen:
#ifdef _WIN32
        if (!ptr) return 0;
        make_msgwnd(p);
        r->value = (int64_t)(uint32_t)(uintptr_t)ptr;               // window handles have 32 significant bits in 32 and 64-bit processes
        return call(p, r, 1) ? (intptr_t)r->ret : 0;
#else
        return 0;                                                   // Linux / macOS: vb_stub_edit_open (the window is the helper's own)
#endif
    case effGetChunk: {
        if (!ptr) return 0;
        size_t need = VB_FRAME_DATA;
        if (need > p->chunk_cap) { free(p->chunk); p->chunk = (uint8_t*)malloc(need); p->chunk_cap = p->chunk ? need : 0; }
        if (!p->chunk) return 0;
        r->out = p->chunk; r->out_cap = (uint32_t)p->chunk_cap;
        if (!call(p, r, 0) || r->ret <= 0) return 0;
        size_t n = (size_t)r->ret;
        if (r->out_len != n) {                                      // too big for a frame: through a mapping of its own
            Chunk c;
            if (!chunk_map(p, n, &c)) return 0;
            Req q; memset(&q, 0, sizeof q); q.kind = VB_K_READ_CHUNK; q.value = (int64_t)n;
            q.in = c.name; q.in_len = (uint32_t)strlen(c.name) + 1;
            int ok = call(p, &q, 0) && q.ret;
            if (ok && n > p->chunk_cap) { free(p->chunk); p->chunk = (uint8_t*)malloc(n); p->chunk_cap = p->chunk ? n : 0; }
            if (ok && p->chunk) memcpy(p->chunk, c.view, n); else n = 0;
            chunk_unmap(&c);
            if (!n) return 0;
        }
        *(void**)ptr = p->chunk;
        return (intptr_t)n;
    }
    case effSetChunk: {
        if (!ptr || value <= 0) return 0;
        size_t n = (size_t)value;
        if (n <= VB_FRAME_DATA) { r->in = ptr; r->in_len = (uint32_t)n; return call(p, r, 0) ? (intptr_t)r->ret : 0; }
        Chunk c;
        if (!chunk_map(p, n, &c)) return 0;
        memcpy(c.view, ptr, n);
        r->flags = VB_F_MAPPED; r->in = c.name; r->in_len = (uint32_t)strlen(c.name) + 1;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        chunk_unmap(&c);
        return ret;
    }
    case effSetSpeakerArrangement: {                                // value = input arrangement, ptr = output arrangement
        if (!value || !ptr) return 0;
        int32_t si = speaker_arr_size((void*)value), so = speaker_arr_size(ptr);
        uint8_t* buf = (uint8_t*)malloc(4 + si + so);
        if (!buf) return 0;
        memcpy(buf, &si, 4); memcpy(buf + 4, (void*)value, si); memcpy(buf + 4 + si, ptr, so);
        r->in = buf; r->in_len = 4 + si + so; r->value = 0;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        free(buf);
        return ret;
    }
    case effGetSpeakerArrangement: {                                // value / ptr = where the plugin puts its pointers
        uint8_t* buf = (uint8_t*)malloc(VB_FRAME_DATA);
        if (!buf) return 0;
        r->out = buf; r->out_cap = VB_FRAME_DATA; r->value = 0;
        intptr_t ret = call(p, r, 0) ? (intptr_t)r->ret : 0;
        uint32_t off = 0;
        for (int k = 0; k < 2; k++) {                               // [size][arrangement] for input, then output; size 0 = NULL
            int32_t n = 0;
            if (off + 4 <= r->out_len) memcpy(&n, buf + off, 4);
            off += 4;
            void* res = 0;
            if (n > 0 && off + (uint32_t)n <= r->out_len) {
                if ((size_t)n > p->spk_cap[k]) { free(p->spk[k]); p->spk[k] = (uint8_t*)malloc(n); p->spk_cap[k] = p->spk[k] ? (size_t)n : 0; }
                if (p->spk[k]) { memcpy(p->spk[k], buf + off, n); res = p->spk[k]; }
                off += (uint32_t)n;
            }
            void** dst = k == 0 ? (void**)value : (void**)ptr;
            if (dst) *dst = res;
        }
        free(buf);
        return ret;
    }
    case effVendorSpecific:                                         // only without data (e.g. the mouse wheel 'stCA' / 'Whee')
        if (ptr) return 0;
        return call(p, r, 0) ? (intptr_t)r->ret : 0;
    }
    return 0;
}

static intptr_t VSTCALL vst_dispatch(AEffect* fx, int32_t op, int32_t index, intptr_t value, void* ptr, float opt)
{
    Inst* p = (Inst*)fx;
    int size, m = disp_marshal(op, &size);
    Req r; memset(&r, 0, sizeof r);
    r.kind = VB_K_DISPATCH; r.op = op; r.index = index; r.value = (int64_t)value; r.opt = opt;
    if (op != effClose && p->dead) return 0;
    int editor = op == effEditClose || op == effEditKeyDown || op == effEditKeyUp || op == effSetEditKnobMode;
    switch (m) {
    case M_NONE:
        if (ptr && op > effGetNumMidiOutputChannels) return 0;      // unknown opcode with data we cannot pass
        break;
    case M_STR_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)strnlen((const char*)ptr, VB_STR_MAX - 1) + 1; }
        break;
    case M_STR_OUT:
        if (ptr) { r.out = ptr; r.out_cap = VB_STR_MAX; }
        break;
    case M_IN:
        if (ptr) { r.in = ptr; r.in_len = (uint32_t)size; }
        break;
    case M_INOUT:
        if (!ptr) return 0;
        r.in = ptr; r.in_len = (uint32_t)size; r.out = ptr; r.out_cap = (uint32_t)size;
        break;
    case M_SPECIAL:
        return special(p, &r, ptr, value);
    default:                                                        // M_LOCAL, M_REFUSE
        return 0;
    }
    return call(p, &r, editor) ? (intptr_t)r.ret : 0;
}

// ----------------------------------------------------------------------------- entry

static AEffect* create(AudioMasterCallback master, const StartPaths* paths)
{
    if (!master) return 0;
    Inst* p = (Inst*)calloc(1, sizeof *p);
    if (!p) return 0;
    vb_mutex_init(&p->audio_cs); vb_mutex_init(&p->ev_cs);
    p->master = master; p->loading = 1;
    AEffect* fx = &p->fx;
    fx->magic = kEffectMagic; fx->dispatcher = vst_dispatch; fx->process = vst_process;
    fx->setParameter = vst_set_parameter; fx->getParameter = vst_get_parameter;
    fx->processReplacing = vst_process_replacing; fx->processDoubleReplacing = vst_process_double;
    fx->object = p;
    if (!start(p, paths)) {
        log_error(paths, p->error);
        plat_stop(p);
        inst_free(p);
        return 0;
    }
    sync_info(p);
    p->loading = 0;
    return fx;
}

#ifdef _WIN32
static AEffect* create_windows(AudioMasterCallback master, const wchar_t* plugin_path, const wchar_t* helper_dir)
{
    StartPaths paths;
    wchar_t* leaf = 0;
    DWORD n = GetFullPathNameW(plugin_path, MAX_PATH, paths.plugin, &leaf);
    if (!n || n >= MAX_PATH || !leaf || !*leaf) return 0;
    size_t dir_len = (size_t)(leaf - paths.plugin);
    wmemcpy(paths.working_dir, paths.plugin, dir_len);
    paths.working_dir[dir_len] = 0;

    int len = _snwprintf(paths.helper, MAX_PATH, L"%ls\\%ls", helper_dir, VB_HELPER);
    if (len < 0 || len >= MAX_PATH) return 0;
    return create(master, &paths);
}

static AEffect* create_from_module(AudioMasterCallback master, const wchar_t* plugin_path)
{
    wchar_t module[MAX_PATH], sibling[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, module, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    for (wchar_t* q = module; *q; q++) if (*q == L'/') *q = L'\\';
    wchar_t* slash = wcsrchr(module, L'\\');
    if (!slash) return 0;

    if (!plugin_path) {
        wcscpy(sibling, module);
        wchar_t* base = sibling + (slash - module) + 1;
        wchar_t* dot = wcsrchr(base, L'.');
        if (dot) *dot = 0;
        size_t len = wcslen(base);
        if (len > 4 && !_wcsicmp(base + len - 4, VB_SUFFIX)) len -= 4;
        wchar_t* ext = base + len;
        if ((size_t)(ext - sibling) + sizeof L".dll" / sizeof(wchar_t) > MAX_PATH) return 0;
        wcscpy(ext, L".dll");
        plugin_path = sibling;
    }
    *slash = 0;
    return create_windows(master, plugin_path, module);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)r; if (reason == DLL_PROCESS_ATTACH) g_self = (HMODULE)h; return TRUE; }

AEffect* VSTPluginMain(AudioMasterCallback master)
{
    return create_from_module(master, 0);
}

AEffect* vb_main(AudioMasterCallback master) { return VSTPluginMain(master); }     // exported as "main" (old hosts)

AEffect* VSTCALL VST2BridgeCreateW(AudioMasterCallback master, const wchar_t* plugin_path)
{
    return plugin_path && *plugin_path ? create_from_module(master, plugin_path) : 0;
}

AEffect* VSTCALL VST2BridgeCreateA(AudioMasterCallback master, const char* plugin_path)
{
    wchar_t wide[MAX_PATH];
    if (!plugin_path || !MultiByteToWideChar(CP_ACP, 0, plugin_path, -1, wide, MAX_PATH)) return 0;
    return VST2BridgeCreateW(master, wide);
}

// the CLAP (clap.c) has this file linked in and creates the plugin through it: dir = the CLAP's folder, stem = <plugin>
// of <plugin>.dll there (<plugin>_x64.clap -> <plugin>.dll); the CLAP needs no bridge DLL next to it
AEffect* vb_stub_main(AudioMasterCallback master, const wchar_t* dir, const wchar_t* stem, const wchar_t* self_name)
{
    (void)self_name;
    wchar_t plugin[MAX_PATH];
    int len = _snwprintf(plugin, MAX_PATH, L"%ls\\%ls.dll", dir, stem);
    if (len < 0 || len >= MAX_PATH) return 0;
    return create_windows(master, plugin, dir);
}
int VST2BridgeStub(void) { return VB_VERSION; }                                     // marker for the patcher
#else
// the CLAP (clap.c) creates the plugin through these; dir = the CLAP's folder, stem = <plugin> of <plugin>.dll there
AEffect* vb_stub_main(AudioMasterCallback master, const char* dir, const char* stem)
{
    const StartPaths paths = { dir, stem };
    return create(master, &paths);
}

// the editor is a window of the helper: VB_F_TOPLEVEL (Linux: the CLAP moves it into the host's window, title
// "VST2Bridge <token>") or VB_F_FLOATING (macOS: a window of its own)
intptr_t vb_stub_edit_open(AEffect* fx, int mode)
{
    Inst* p = (Inst*)fx;
    Req r; memset(&r, 0, sizeof r);
    r.kind = VB_K_DISPATCH; r.op = effEditOpen; r.flags = mode;
    return call(p, &r, 1) ? (intptr_t)r.ret : 0;
}
int vb_stub_edit_show(AEffect* fx, int show)
{
    Req r; memset(&r, 0, sizeof r);
    r.kind = VB_K_EDITOR_SHOW; r.index = show;
    return call((Inst*)fx, &r, 1) && r.ret;
}
int vb_stub_edit_closed(AEffect* fx) { Inst* p = (Inst*)fx; return p->sh && vb_xchg(&p->sh->editor_closed, 0); }
const char* vb_stub_token(AEffect* fx) { return ((Inst*)fx)->token; }
#endif
