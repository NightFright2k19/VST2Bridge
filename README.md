# VST2Bridge

A free, universal VST2 bridge for Windows: use **32-bit VST2 plugins in 64-bit hosts** and **64-bit plugins in 32-bit hosts**, as VST2 or as **CLAP**. On **Linux and macOS** (Apple Silicon and Intel) it makes Windows VST2 plugins usable as **CLAP** plugins, run under Wine. No installation, no registry, free for everybody (LGPL-3.0).

## How it works

The patcher writes a small bridge DLL and a CLAP bridge for each plugin into the folder the plugin lies in (the plugin itself stays untouched). The host loads the bridge like any plugin; the bridge starts a helper process of the plugin's bitness, which runs the real plugin, and passes everything between the two: audio, MIDI and SysEx, parameters, presets, the plugin's own window and the plugin's calls to the host.

| Plugin | Bridge written next to the plugin | For |
|---|---|---|
| `syxg50.dll` (32-bit) | `syxg50_x64.dll` (VST2) + `syxg50_x64.clap` (CLAP) + `vst2bridge-host32.exe` | 64-bit hosts |
| `Emu-SC8850.dll` (64-bit) | `Emu-SC8850_x86.dll` (VST2) + `Emu-SC8850_x86.clap` (CLAP) + `vst2bridge-host64.exe` | 32-bit hosts |

The CLAP bridge has the VST2 bridge built in, so it works on its own: `<name>_x64.clap` needs only `<name>.dll` and the helper exe in the same folder, not `<name>_x64.dll`. It offers the same plugin with the same features to CLAP hosts.

Windows VST2 hosts with [direct-loading support](docs/host-integration.md) can use one bridge DLL to open plugins by path; host developers get the bridge DLLs and helpers as `VST2Bridge-DevKit-r<N>.zip` from the release page.

### Linux and macOS

Linux and macOS cannot load Windows DLLs, so there the patcher writes **only CLAP bridges**, for plugins of either bitness. The CLAP is a native plugin (Linux x86-64; macOS universal, Apple Silicon and Intel) that runs the Windows plugin in the same helper programs under **Wine**:

| Plugin | Written next to the plugin (Linux / macOS) |
|---|---|
| `syxg50.dll` (32-bit) | `syxg50.clap` + `vst2bridge-host32.exe` |
| `Emu-SC8850.dll` (64-bit) | `Emu-SC8850.clap` + `vst2bridge-host64.exe` |

On macOS the `.clap` is a bundle (a folder), signed ad hoc by the patcher. **Wine must be installed** (Wine 9 or newer; on Apple Silicon Wine runs under Rosetta 2): `wine` in the path, on macOS also the usual install folders (Homebrew, Wine Stable / Staging / Devel apps, CrossOver), or set `VB_WINE=/path/to/wine`. Start Wine once by hand first so that its prefix (`~/.wine` or `WINEPREFIX`) exists. The plugin's window is embedded in the host's window on Linux (X11 hosts; XWayland works, Wayland-native embedding does not); on macOS it opens as a floating window of its own, because a window of another process cannot be embedded there.

## Usage

1. Download the patcher from the [latest release](https://github.com/NightFright2k19/VST2Bridge/releases/latest): `VST2Bridge-Patcher-r<N>-windows.exe`, `...-linux.tar.gz` (extract, run `VST2Bridge-Patcher`), `...-macos-applesilicon.zip` (M1 and newer) or `...-macos-intel.zip` (extract, open the app).
2. Put the patcher into a folder together with your VST2 plugin DLLs (plugin DLLs lying right next to the patcher, without subfolders, are found). Plugins elsewhere: click **Add folder…**; added folders are searched including their subfolders.
3. The patcher lists the plugins it finds and tells whether each is 32-bit or 64-bit. Every plugin has a checkbox in front of it (all are ticked at the start); click an entry to tick or untick it, only ticked plugins are converted. On Windows, **Output format** picks the bridge types: *VST2 + CLAP* (default), *VST2 only* or *CLAP only*. Click **Create bridges**.
   The bridge file(s) and the helper exe are written into the folder each plugin lies in, next to the plugin. The original plugins are never moved, copied or changed. Plugins that already have bridges are listed too, so a newer patcher can refresh them.
4. Windows: load `<plugin>_x64.dll` / `<plugin>_x64.clap` in a 64-bit host or `<plugin>_x86.dll` / `<plugin>_x86.clap` in a 32-bit host. CLAP hosts look in `C:\Program Files\Common Files\CLAP` by default: add the plugin folder to the host's CLAP folders. Keep the plugin (`<name>.dll`, the name before `_x64` / `_x86`) and the helper exe in the same folder as the bridge files (the patcher puts them next to the plugin); the `.clap` finds its plugin by its own file name, so do not rename it. If your host looks elsewhere, copy the plugin, the bridge files and the helper exe there together.
   Linux / macOS: load `<plugin>.clap`; add the plugin folder to the host's CLAP folders (default folders: `~/.clap` on Linux, `~/Library/Audio/Plug-Ins/CLAP` on macOS; `CLAP_PATH` also works). Keep the `.clap`, the plugin DLL and the helper exe together. If a plugin does not load, the reason is in `vst2bridge.log` in the temp folder (`$TMPDIR` or `/tmp`); `VB_DEBUG=1` shows Wine's own messages.

The bridge leaves the sample rate to the host: the plugin always gets the rate the host sets (see [Sample rates](docs/technical.md#sample-rates)).

The patcher speaks 29 languages (the system language, or pick one at the top right), updates itself (**Update** appears when a new release is out) and shows the changes with **What's new?**. Without a window: `VST2Bridge-Patcher.exe --cli [folder] [--dry-run] [--format both|vst2|clap]` (Linux / macOS: `VST2Bridge-Patcher --cli …`; `--format` counts on Windows only). The command line searches the patcher's folder and the folder you give.

The patcher's icon is the one of the SXG-Create-NF patcher; its artwork is derived from "Vst icon" by SyafriStudio (Magnific).

## More documentation

| Document | Contents |
|---|---|
| [Host integration](docs/host-integration.md) | Adding direct VST2Bridge loading to a Windows VST2 host |
| [How the bridge works](docs/technical.md) | Processes, shared memory, threads, which VST2 calls are passed and how |
| [Limitations](docs/limitations.md) | What does not work (yet) |
| [Development](docs/development.md) | Building, tests, releases, project layout |
| [Version history](docs/versions.md) | Changes of every release |

## License

VST2Bridge is free software under the [GNU Lesser General Public License, version 3](LICENSE) (LGPL-3.0). The bundled CLAP headers in `bridge/clap/` come from the CLAP SDK and stay under their own MIT license (`bridge/clap/LICENSE`).
