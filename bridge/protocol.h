// protocol.h - the link between the bridge DLL (stub.c, loaded by the host) and the helper process (host.c, which
// loads the real plugin). One shared-memory block per plugin instance; no pointers in it, the layout is the same
// for 32-bit and 64-bit builds. See docs/technical.md for the full description.
//
// Three channels, each a stack of call frames (calls can nest: a host call may make the plugin call back into
// the host, which may call the plugin again ...):
//   main   host GUI / control thread <-> helper main (window) thread. Both sides may start a call; a named
//          mutex (Linux / macOS: the lock word VbChan.lock) decides who starts the outermost one.
//   audio  host audio thread <-> helper audio thread. Only the bridge starts calls (process, and any other call
//          made on the thread that runs process); the plugin's callbacks from its audio thread nest inside.
//   aux    callbacks from other threads of the plugin -> bridge (one at a time).
// Linux / macOS (the CLAP runs the helper under Wine): the block is a file that both sides map, and each signal is one
// byte on one of four loopback TCP connections instead of a named event:
//   M  bridge -> helper: main_h        helper -> bridge: main_s, ready
//   V                                  helper -> bridge: main_v
//   A  bridge -> helper: audio_h       helper -> bridge: audio_s
//   X  bridge -> helper: aux_h         helper -> bridge: aux_s
// Large chunks go through files of their own (the frame names the file).
#ifndef VB_PROTOCOL_H
#define VB_PROTOCOL_H
#include <stdint.h>
#include "vst2.h"

#define VB_MAGIC        0x56324252u     // 'V2BR'
#define VB_VERSION      2
#define VB_MAXDEPTH     8               // nested calls per channel
#define VB_FRAME_DATA   65536           // bytes passed by pointer per call (larger chunks use a mapping of their own)
#define VB_MAXCH        32              // audio channels each way
#define VB_MAXFRAMES    4096            // longest piece of a process call (longer calls are split)
#define VB_MAXEVENTS_TO_PLUGIN 1024     // events queued for the next process call and sent in VbAudio
#define VB_MAXEVENTS_TO_HOST   1024     // events returned by audioMasterProcessEvents in VbFrame.data
#define VB_SYSEX_MAX    65536           // SysEx bytes per piece

enum { VB_MSG_NONE = 0, VB_MSG_REQ, VB_MSG_REPLY };
enum { VB_OWNER_NONE = 0, VB_OWNER_STUB, VB_OWNER_HELPER };

enum {                                  // VbFrame.kind
    VB_K_DISPATCH = 1,                  // bridge -> helper: dispatcher(op, index, value, data, opt)
    VB_K_SETPARAM,                      // index, opt
    VB_K_GETPARAM,                      // index -> opt
    VB_K_PROCESS,                       // audio channel: op = VB_P_*, value = frames (data in VbAudio)
    VB_K_READ_CHUNK,                    // data = mapping name: helper copies the chunk of the last effGetChunk into it
    VB_K_QUIT,
    VB_K_MASTER,                        // helper -> bridge: audioMaster(op, index, value, data, opt)
    VB_K_EDITOR_SHOW                    // index 1 = show, 0 = hide the editor window (Linux / macOS CLAP)
};
enum { VB_P_REPLACING = 0, VB_P_ACCUMULATE, VB_P_DOUBLE };
enum {                                  // VbFrame.flags
    VB_F_MAPPED   = 1,                  // effSetChunk: data holds the name of a mapping (net: a file) with the chunk
    VB_F_TOPLEVEL = 2,                  // effEditOpen (Linux): a bare window titled "VST2Bridge <token>", moved into the host's X11 window by the CLAP
    VB_F_FLOATING = 4                   // effEditOpen (macOS): a window of its own with a title bar
};

typedef struct {
    int32_t kind, op, index, flags;
    int64_t value;
    float   opt; int32_t pad0;
    int64_t ret;
    uint32_t len, pad1;                 // bytes used in data (request in, reply out)
    uint8_t  data[VB_FRAME_DATA];
} VbFrame;

typedef struct {
    volatile int32_t depth;             // frames in use
    volatile int32_t msg;               // kind of the last message: VB_MSG_REQ (frame depth-1 waits) or VB_MSG_REPLY
    volatile int32_t owner;             // main channel: side that started the outermost call
    volatile int32_t lock;              // main channel, Linux / macOS: 0 or the side that holds the lock (replaces the mutex)
    VbFrame f[VB_MAXDEPTH];
} VbChan;

typedef struct {                        // a MIDI or SysEx event (VstMidiEvent / VstMidiSysexEvent without pointers)
    int32_t  type;                      // kVstMidiType / kVstSysExType
    int32_t  delta, flags, note_length, note_offset;
    uint8_t  midi[4];
    int8_t   detune; uint8_t off_velocity; uint8_t pad[2];
    uint32_t sysex_off, sysex_len;
} VbEvent;

typedef struct {                        // the plugin's AEffect fields, refreshed by the helper before every message
    int32_t numPrograms, numParams, numInputs, numOutputs, flags, initialDelay, uniqueID, version;
    float   ioRatio;
    int32_t has_replacing, has_double, pad;
} VbInfo;

typedef struct {
    int32_t  nin, nout;
    uint32_t n_events, sysex_bytes;
    VbEvent  events[VB_MAXEVENTS_TO_PLUGIN];
    uint8_t  sysex[VB_SYSEX_MAX];
    double   in[VB_MAXCH][VB_MAXFRAMES];      // float pieces use the first half of each row as float[]
    double   out[VB_MAXCH][VB_MAXFRAMES];
} VbAudio;

typedef struct {
    uint32_t magic, version;
    int32_t  ok;                        // set by the helper before "ready": 1 = plugin loaded
    volatile int32_t editor_closed;     // the user closed the floating editor window (macOS); the CLAP tells the host
    char     error[256];
    VbInfo   info;
    VbChan   main, audio, aux;
    VbAudio  au;
} VbShared;

_Static_assert(sizeof(VbFrame) == 48 + VB_FRAME_DATA, "VbFrame layout");
_Static_assert(sizeof(VbEvent) == 36, "VbEvent layout");
_Static_assert(4 + VB_MAXEVENTS_TO_HOST * sizeof(VbEvent) <= VB_FRAME_DATA, "MIDI output fits in a frame");
_Static_assert(sizeof(VbInfo) == 48, "VbInfo layout");

// object names: "Local\V2B<host pid>_<n>_<suffix>"
#define VB_OBJ_MAP      "m"
#define VB_OBJ_READY    "r"             // helper -> bridge: plugin loaded (or failed)
#define VB_OBJ_MUTEX    "x"             // main channel: who starts the outermost call
#define VB_OBJ_MAIN_H   "mh"            // -> helper main thread
#define VB_OBJ_MAIN_S   "ms"            // -> bridge, call started by the bridge
#define VB_OBJ_MAIN_V   "mv"            // -> bridge, call started by the helper (served by the bridge's server thread)
#define VB_OBJ_AUDIO_H  "ah"
#define VB_OBJ_AUDIO_S  "as"
#define VB_OBJ_AUX_H    "xh"
#define VB_OBJ_AUX_S    "xs"
#define VB_OBJ_CHUNK    "c"             // + a number: a mapping for one large chunk

// the stub's marker export: the patcher skips DLLs that have it (they are bridges, not plugins)
#define VB_STUB_EXPORT  "VST2BridgeStub"
#endif
