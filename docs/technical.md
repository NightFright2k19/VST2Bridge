# How the bridge works

[Back to the README](../README.md)

## Files

| File | Built from | Role |
|---|---|---|
| `<name>_x64.dll` | `bridge/stub.c` (64-bit, `vst2bridge-x64.dll`) | Loaded by a 64-bit host in place of the 32-bit plugin `<name>.dll` |
| `<name>_x86.dll` | `bridge/stub.c` (32-bit, `vst2bridge-x86.dll`) | Loaded by a 32-bit host in place of the 64-bit plugin `<name>.dll` |
| `<name>_x64.clap` | `bridge/clap.c` + `bridge/stub.c` (64-bit, `vst2bridge-x64.clap`) | CLAP plugin for 64-bit hosts; has the bridge built in and runs `<name>.dll` (needs no `_x64.dll`) |
| `<name>_x86.clap` | `bridge/clap.c` + `bridge/stub.c` (32-bit, `vst2bridge-x86.clap`) | CLAP plugin for 32-bit hosts; has the bridge built in and runs `<name>.dll` (needs no `_x86.dll`) |
| `vst2bridge-host32.exe` | `bridge/host.c` (32-bit) | Helper that runs a 32-bit plugin |
| `vst2bridge-host64.exe` | `bridge/host.c` (64-bit) | Helper that runs a 64-bit plugin |
| `<name>.clap` (Linux) | `bridge/clap.c` + `bridge/stub.c` (x86-64, `vst2bridge-linux.clap`) | CLAP plugin; runs `<name>.dll` (32 or 64-bit) in the helper of its bitness under Wine |
| `<name>.clap/` (macOS bundle) | `bridge/clap.c` + `bridge/stub.c` (universal, `vst2bridge-macos`) | The same for macOS: `<name>.clap/Contents/MacOS/<name>`, `Info.plist`, signed ad hoc |

All bridge DLLs of one bitness are the same file. With filename-based loading, the bridge removes `.dll` and the suffix `_x64` (64-bit bridge) or `_x86` (32-bit bridge) from its own name and loads `<name>.dll` from its own folder. The helper must be in the same folder. The bridge exports `VSTPluginMain`, `main` (old hosts) and the marker `VST2BridgeStub`, by which the patcher recognises its own files (it also skips JBridge's files, which export `JBridgeBootstrap`).

## Direct loading on Windows

A VST2 host can load the bridge DLL for its own bitness and resolve `VST2BridgeCreateA` or `VST2BridgeCreateW`. Both take the host callback and the original plugin's absolute DLL path and return a proxy `AEffect*`, or `NULL` on failure. `A` accepts the Windows ANSI code page and `W` accepts UTF-16. The declarations are in [`bridge/vst2bridge.h`](../bridge/vst2bridge.h); the step-by-step guide for host authors, with an example and the release's developer kit, is [Host integration](host-integration.md).

Keep the helper for the plugin's bitness beside the bridge DLL. The helper starts in the original plugin's directory, so the plugin can find its companion files. Close the returned proxy with `effClose` before unloading the bridge DLL. Direct loading uses the same protocol 2 and 1,024-event limits as filename-based loading.

## CLAP bridge

The CLAP bridge (`bridge/clap.c`) is a VST2 host in a CLAP plugin: it creates the plugin through the VST2 bridge (`stub.c`, linked into the CLAP, so no separate bridge DLL is needed) and translates every CLAP call into VST2 calls. The plugin is the file with the CLAP's own name without the suffix (`foo_x64.clap` → `foo.dll` in the same folder). The process boundary, the helper and the passing of data are those of the VST2 bridge, unchanged. All CLAP bridges of one bitness are the same file; it exports `clap_entry` and the marker `VST2BridgeClap`.

| CLAP | VST2 |
|---|---|
| Descriptor | Name (`effGetEffectName`, else `effGetProductString`, else the file name), vendor, version (`effGetVendorVersion`), features `instrument` + `synthesizer` for an instrument (`effFlagsIsSynth`, category synth, no audio inputs, or `effCanDo receiveVstEvents` / `receiveVstMidiEvent`; old instruments set no flag) or `audio-effect`, plus `stereo` / `mono` for the main output; id `io.github.nightfright2k19.vst2bridge.<file name without suffix, lower case>`. To read them the bridge creates the plugin once when the host first asks; that instance is handed to the first `create_plugin`, so a host that scans and then loads starts the helper only once. |
| `activate` / `deactivate` | `effSetSampleRate`, `effSetBlockSize`, `effMainsChanged` |
| `start_processing` / `stop_processing` | `effStartProcess` / `effStopProcess` |
| Audio ports | VST2 channels in pairs: port 0 = channels 1-2 (main), port 1 = channels 3-4 ...; a last odd channel is a mono port |
| Note ports | MIDI in always (VST2 hosts send MIDI to every plugin, and many plugins answer no `effCanDo`); MIDI out for `sendVstEvents` / `sendVstMidiEvent` |
| `process` | CLAP MIDI, SysEx and note on / off events → `effProcessEvents` at their frame; parameter events → `setParameter` before the block; transport → `audioMasterGetTime`; `processReplacing`; MIDI and SysEx of the plugin (`audioMasterProcessEvents` during process) → output events. `reset` sends all notes off and reset controllers on 16 channels with the next block. |
| Parameters | id = VST2 index, range 0 … 1, name `effGetParamName`, default = value after loading; text = `effGetParamDisplay` + `effGetParamLabel` for the current value (other values as a number). `audioMasterAutomate` / `BeginEdit` / `EndEdit` from the plugin become `PARAM_VALUE` / gesture events (during process directly, otherwise queued, with `request_flush`); changes the host made itself are not sent back. `audioMasterUpdateDisplay` → `params.rescan`. |
| State | `'VBCL'`, version, kind, program, length, data: the plugin's bank chunk (`effGetChunk` / `effSetChunk`) when it has `effFlagsProgramChunks`, otherwise all parameter values |
| Latency | `initialDelay`, read after `effMainsChanged`; a change while active (`audioMasterIOChanged`) asks the host for a restart |
| GUI | Win32, embedded: `effEditGetRect`, `effEditOpen` with the host's window, `effEditClose`; `audioMasterSizeWindow` → `gui.request_resize`. Not resizable by the host. |

## Linux and macOS

The Linux / macOS CLAP is `clap.c` and `stub.c` linked into one native file (`make -C bridge linux` / `macos`), so the CLAP side and the whole protocol are the code of the Windows bridges; `bridge/plat.h` covers the differences (locks, thread ids, atomics). Instead of loading a VST2 DLL, `clap.c` calls the bridge directly (`vb_stub_main`). The file takes the plugin from its own name: `foo.clap` → `foo.dll` in the same folder (no suffix is removed; on macOS the host may pass the bundle or the binary inside it). The bitness of the DLL (PE header) decides the helper.

The helper is the same Windows program, started under Wine:

```
wine vst2bridge-hostNN.exe Z:\path\foo.dll net:<port>:<token>:<shared file> <shell id> 0
```

- **Shared memory** is a file (Linux `/dev/shm`, macOS `$TMPDIR`) of the size of `VbShared`, mapped by both sides (Wine sees `/` as drive `Z:`); it is deleted as soon as both have it open. Chunks above 64 KB use files of their own in the same folder.
- **Signals:** the helper connects four times to a loopback TCP port the CLAP opened (`M`, `V`, `A`, `X`, each with a random token); each named event becomes one byte on one of them (see `protocol.h`). In the helper a reader thread per connection turns the bytes back into local events, so its waits, message pumping and nesting stay as they are. A closed connection ends the helper (the host is gone); in the CLAP it marks the helper as dead.
- **Mutex:** the main channel's named mutex becomes the lock word `VbChan.lock` in shared memory (compare-and-swap on both sides; contention is rare, waiting polls every 0.2 / 1 ms).
- **audioMasterSizeWindow** from the plugin's GUI goes straight to the CLAP side (`gui.request_resize` may be called from any thread), not through an editor thread as on Windows.
- **Editor, Linux:** `effEditOpen` with the flag `VB_F_TOPLEVEL`: the helper opens the editor in a bare popup titled `VST2Bridge <token>`; the CLAP finds that X11 window by its title and moves it into the host's window (`XReparentWindow`, via xcb). A CLAP timer (100 ms) tells Wine where the window is on the screen, so mouse clicks hit when the host's window moves.
- **Editor, macOS:** a window of another process cannot be embedded, so the CLAP offers a floating GUI (`cocoa`, floating): `show` opens the editor in a window of its own with a title bar (always on top, named after the plugin) or shows it again (`VB_K_EDITOR_SHOW`), `hide` hides it; closing it by hand only hides it, sets `VbShared.editor_closed`, and a timer (250 ms) tells the host (`gui.closed`). Its size follows the plugin's `sizeWindow`.
- **Wine:** `VB_WINE`, then (macOS) the usual install folders, then `wine` in the path; `WINEDEBUG=-all` unless set; `VB_DEBUG=1` keeps Wine's output. The helper runs in the plugin's folder. Failures go to `vst2bridge.log` in `$TMPDIR` or `/tmp`.

## Start

`VSTPluginMain` of the bridge creates one shared-memory block and a set of named events and a mutex (`Local\V2B<host pid>_<n>_*`), asks the host for `audioMasterCurrentId` (shell plugins), and starts the helper in the plugin's folder:

```
vst2bridge-hostNN.exe <plugin.dll> <object prefix> <shell id> <host pid>
```

The helper loads the plugin, calls its `VSTPluginMain`, writes the plugin's `AEffect` fields (`numInputs`, `flags`, `uniqueID` ...) into shared memory and signals "ready". The bridge copies those fields into its own `AEffect` and returns it to the host. The helper is put into a job object that ends it with the host, and it also watches the host process itself (Linux / macOS: it ends when its connections close).

If anything fails, `VSTPluginMain` returns NULL (the host reports that the plugin cannot be loaded) and the reason goes to `%TEMP%\vst2bridge.log`.

## Calls across the process boundary

Every host → plugin call (`dispatcher`, `setParameter`, `getParameter`, `process*`) becomes a request frame in shared memory; the helper makes the real call and writes the reply into the same frame. The plugin's calls to the host (`audioMaster`) travel the other way. Calls nest: during `effEditOpen` a plugin may call `audioMasterSizeWindow`, and the host may call `effEditGetRect` while answering it. So each channel holds a stack of up to 8 frames and the side waiting for a reply serves the other side's nested requests meanwhile, on the same thread, just as the calls would run in one process.

Three channels connect the processes (`bridge/protocol.h`):

| Channel | Bridge side | Helper side | Who starts calls |
|---|---|---|---|
| main | the host's GUI / control threads | the main thread (runs the window message loop) | both; a named mutex decides who starts the outermost call |
| audio | the thread that calls `process` | an audio thread (time-critical priority) | the bridge only (the plugin's callbacks from its audio thread nest inside) |
| aux | a server thread | any other thread of the plugin | the helper only (one call at a time) |

Calls the host makes on the thread that last called `process` go through the audio channel, so they never wait for the GUI; all other calls go through the main channel. Editor calls always use the main channel, because the editor window belongs to the helper's main thread.

Calls the plugin starts on its own from its GUI thread (a knob turned: `audioMasterBeginEdit` / `Automate` / `EndEdit`) are answered by a server thread of the bridge; `audioMasterSizeWindow` from there is handed to the host's editor thread (where the editor was opened), because hosts expect window changes on that thread. While a thread of the bridge waits, it keeps answering messages sent to its windows, so cross-process window messages cannot deadlock.

## What is passed and how

The data behind `ptr` is copied into the frame (`bridge/marshal.h`):

| Kind | Opcodes |
|---|---|
| Value only | `effOpen`, `effSetProgram`, `effGetProgram`, `effSetSampleRate`, `effSetBlockSize`, `effMainsChanged`, `effEditClose`, `effCanBeAutomated`, `effGetPlugCategory`, `effSetBypass`, `effGetVendorVersion`, `effGetTailSize`, `effGetVstVersion`, `effEditKeyDown/Up`, `effStartProcess`, `effStopProcess`, `effSetProcessPrecision`, `effGetNumMidi*Channels`, ... and unknown opcodes without data |
| String in | `effSetProgramName`, `effString2Parameter`, `effCanDo` |
| String out | `effGetProgramName`, `effGetParamLabel/Display/Name`, `effGetProgramNameIndexed`, `effGetEffectName`, `effGetVendorString`, `effGetProductString`, `effShellGetNextPlugin` |
| Structure in / out | `effGetInput/OutputProperties`, `effGetParameterProperties`, `effGetMidiProgramName`, `effGetCurrentMidiProgram`, `effGetMidiProgramCategory`, `effGetMidiKeyName`, `effBeginLoadBank/Program` |
| Special | `effEditGetRect` (the bridge keeps a copy of the rectangle), `effEditOpen` (the helper creates a child window of the host's window), `effGetChunk` / `effSetChunk` (up to 64 KB in the frame, larger through a mapping of their own, any size), `effProcessEvents` (queued, sent with the next `process`), `effSetSpeakerArrangement` / `effGetSpeakerArrangement`, `effVendorSpecific` (only without data) |
| Answered by the bridge | `effEditIdle` (the helper gives the editor idle time itself, every 30 ms) |

Plugin → host: value-only calls (`audioMasterAutomate`, `Version`, `Idle`, `IOChanged`, `GetSampleRate`, `GetBlockSize`, `BeginEdit`, `EndEdit`, `UpdateDisplay`, ...), `CanDo` (string in), `GetVendorString` / `GetProductString` (string out), `GetTime` (the time info is copied back), `ProcessEvents` (MIDI and SysEx out of the plugin), `GetDirectory`, `SizeWindow` (the helper also resizes its window), `VendorSpecific` without data. `audioMasterCurrentId` is answered by the helper with the id the bridge got from the host. Before each callback and after each call the bridge refreshes its `AEffect` fields, so `audioMasterIOChanged` reports the plugin's new values.

## Audio

`processReplacing`, `process` (accumulating) and `processDoubleReplacing` all go through the audio channel: the inputs (up to 32 channels), the events queued by `effProcessEvents` and the frame count go into shared memory, the helper calls `effProcessEvents` and the plugin's process function, and the outputs come back. Blocks longer than 4096 frames are split; events are given to the piece they fall into. A plugin without `processReplacing` is run with `process` into cleared buffers. A call costs one round trip between the processes (two event signals).

**Waiting on the audio channel (Windows).** Waking a sleeping thread takes longer than most plugins need for a block, so the bridge's audio thread first polls the channel's message word for up to 30 µs (`VST2BRIDGE_SPIN`, in microseconds, `0` = never; set in the host's environment; always off on a single-core machine) and only then sleeps on the event. `VbChan.sleep_s` tells the helper which of the two is the case: the helper writes the reply and sets the event only if `sleep_s` is 1 (both sides use full barriers, so one of them always notices the other). If the polling keeps failing (8 times in a row: a heavy plugin) the bridge stops polling for 256 calls, so such plugins do not burn a core. Because the bridge polls, the helper must never leave the channel in a state that looks like the reply to the bridge's request: after a nested call (the plugin's callback during `process`) it sets `msg` to `VB_MSG_NONE` before it lowers `depth`. The helper's audio thread registers with MMCSS (task "Pro Audio") when `avrt.dll` is there. Linux / macOS (sockets): the polling is not used there, but the helper's audio thread reads its socket itself instead of going through a reader thread and a local event, which saves one thread wake-up per block (the other three connections keep their reader threads). `bash bridge/test/run_bench.sh` measures microseconds per `process` call under Wine for 1 to 32 instances.

## Sample rates

The bridge does no sample rate handling of its own. The rate the host sets with `effSetSampleRate` (or CLAP `activate`) goes to the plugin unchanged, `audioMasterGetSampleRate` and the `sampleRate` in `audioMasterGetTime` answer the host's rate, and `initialDelay` is the plugin's own. Earlier releases (r4 to r6) converted unusual host rates to 44100 Hz and read `SampleRate` from `vst2bridge.ini`; both are gone, and a `vst2bridge.ini` is ignored.

## Failure

If the helper ends (the plugin crashed), every waiting call returns at once; from then on the bridge outputs silence and returns 0, so the host keeps running.
