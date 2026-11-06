# Add direct VST2Bridge support to a Windows VST2 host

Direct loading lets your host open an opposite-bitness VST2 plugin from its original DLL path. The host loads a VST2Bridge DLL of its own bitness and receives an `AEffect*` proxy. The plugin stays in its original folder, and the host can reuse the same bridge DLL for multiple plugins.

## Deploy the bridge files

Every release has a developer kit, `VST2Bridge-DevKit-r<N>.zip`, with these files: `x86_host/` (`vst2bridge-x86.dll`, `vst2bridge-host64.exe`) and `x64_host/` (`vst2bridge-x64.dll`, `vst2bridge-host32.exe`). The low-level details are in [How the bridge works](technical.md#direct-loading-on-windows).

Your host chooses the bridge directory. Keep the helper beside the bridge DLL. Falcosoft BASS_VST specifically looks in a `VST2Bridge` subfolder beside `bass_vst.dll`.

| Host | Plugin | DLL loaded by the host | EXE started by the DLL |
|---|---|---|---|
| x86 | x64 | `vst2bridge-x86.dll` | `vst2bridge-host64.exe` |
| x64 | x86 | `vst2bridge-x64.dll` | `vst2bridge-host32.exe` |

Use a DLL that exports `VST2BridgeCreateW` or `VST2BridgeCreateA` and take both files from the same build. A distribution that supports both host architectures can put all four files in the same folder. The host chooses the DLL by **host bitness**, and the DLL chooses its helper by **plugin bitness**.

## Add a fallback to your loader

1. Try your normal VST2 load first.
2. If loading the DLL fails, inspect its PE machine type. Use the bridge when the original DLL is a supported x86 or x64 image of the opposite bitness. Keep your usual error handling for missing DLLs or dependencies, and for VST2 initialization failures.
3. Load the bridge DLL by absolute path and resolve its factory by name. Pass your usual VST2 host callback and the **absolute path to the original plugin DLL**. Establish your loading context before calling the factory, just as you would before calling `VSTPluginMain`. The callback can run before the factory returns, and `audioMasterCurrentId` can arrive with a null `AEffect*`.

Use `VST2BridgeCreateW` for a UTF-16 path. `VST2BridgeCreateA` accepts bytes in the current Windows ANSI code page. Both use the VST2 calling convention and return null on failure. The resolved plugin path and the helper path must fit within `MAX_PATH`, including the terminator.

This C++ example shows the fallback after the host has confirmed the bitness mismatch. It uses the bridge's VST2 declarations. A host with its own VST2 declarations can use its existing equivalent types and calling convention.

```cpp
#include <windows.h>
#include "vst2bridge.h"

typedef AEffect* (VSTCALL *BridgeCreateW)(
    AudioMasterCallback, const wchar_t*);

AEffect* LoadBridgedVst2(const wchar_t* bridgeDllPath,
                        const wchar_t* pluginDllPath,
                        AudioMasterCallback hostCallback,
                        HMODULE& bridgeModule)
{
    bridgeModule = NULL;
    HMODULE module = LoadLibraryW(bridgeDllPath);
    if (!module)
        return NULL;

    BridgeCreateW create = reinterpret_cast<BridgeCreateW>(
        GetProcAddress(module, "VST2BridgeCreateW"));
    AEffect* effect = create ? create(hostCallback, pluginDllPath) : NULL;
    if (!effect) {
        FreeLibrary(module);
        return NULL;
    }

    bridgeModule = module;
    return effect;
}
```

On success, retain the returned module reference with the instance and run your normal VST2 initialization, starting with `effOpen`. Pass audio, MIDI, parameters, state, and editor calls through the proxy. On teardown, stop calls into the proxy, close its editor if open, and dispatch `effClose`. Release the module reference with `FreeLibrary` only after `effClose` returns. If you share a single module reference across instances, release it after closing the last instance.

## Diagnose a failed load

- **Bridge DLL fails to load.** Capture `GetLastError()` immediately after `LoadLibraryW` fails. Check the DLL's path and that its bitness matches the host.
- **Factory export is missing.** Check that the deployed DLL supports direct loading and exports the exact name you resolved.
- **Factory returns null.** Check that the matching helper is beside the bridge DLL and that both are from the same build. Look for a fresh entry in `vst2bridge.log` in the Windows temp directory, typically `%TEMP%`. Some early path failures return before a log entry is written.

See the [API header](../bridge/vst2bridge.h) for declarations and the [bridge limitations](limitations.md) for supported calls and limits.
