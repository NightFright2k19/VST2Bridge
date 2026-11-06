# Development

[Back to the README](../README.md)

## Layout

| Path | Contents |
|---|---|
| `bridge/` | The bridge in C: `stub.c` (bridge DLL; on Linux / macOS linked into the CLAP), `clap.c` (CLAP bridge around it), `host.c` (helper exe), `protocol.h` (shared memory), `plat.h` (Windows / POSIX primitives), `marshal.h` (how each opcode's data is copied), `vst2.h` (VST 2.4 declarations written from the public interface description, no Steinberg SDK file), `stub.def` (exports), `clap/` (CLAP headers, MIT, from the CLAP SDK), `Makefile` |
| `bridge/test/` | `fakevst.c` (test plugin), `testhost.c` (VST2 test host), `testclap.c` (CLAP test host, Windows and Linux), `smoke.c` (loads a Linux / macOS CLAP without Wine), `run_tests.sh` (all bridges under Wine), `bench.c` + `run_bench.sh` (timing of one `process` call, not a test) |
| `patcher.py`, `patcher_i18n.py` | The patcher (Tkinter window and `--cli`); texts in 29 languages (the same as SXG-Create-NF; a missing key falls back to English) |
| `utility/icons/` | The patcher's icon: `patcher.ico` (Windows exe), `patcher.icns` (macOS app), `patcher.png` / `patcher_32.png` / `patcher_16.png` (window icon in Tk); taken from SXG-Create-NF, artwork derived from "Vst icon" by SyafriStudio (Magnific) |
| `utility/bridges.py` | Finding plugins (the top level of the patcher's folder, added folders) and writing the bridges and helper next to each plugin |
| `utility/pe.py` | Reading machine type and exports of a DLL |
| `utility/selfupdate.py` | Self-update from the GitHub releases (adapted from SXG-Create-NF) |
| `docs/versions.md` | Version history, shown by the patcher's **What's new?** |

## Building

The bridge is built with mingw-w64 (cross-compiled on Linux; also works in MSYS2 on Windows):

```
sudo apt install gcc-mingw-w64
make -C bridge          # vst2bridge-x64/x86.dll, vst2bridge-x64/x86.clap, vst2bridge-host32.exe, vst2bridge-host64.exe
make -C bridge linux    # vst2bridge-linux.clap (gcc, libxcb1-dev)
make -C bridge macos    # vst2bridge-macos, universal arm64 + x86_64 (clang on a Mac)
```

The helpers are the same on all systems (mingw-w64); the Linux and macOS patchers bundle them with their CLAP. The frozen macOS patcher carries `vst2bridge-macos.gz`: PyInstaller would otherwise thin a bare Mach-O data file to the architecture of the build machine (the `--selftest` checks that both architectures are there). `VB_TEST_SYSTEM=windows|linux|macos` makes the patcher from source act as that system's patcher (CI only).

The patcher runs from source with Python 3.10+ and Tkinter (`python patcher.py`, or `python patcher.py --cli [folder] [--format both|vst2|clap]`; it reads the top level of the folder holding `patcher.py` and the folders added in the window or given on the command line, and writes the bridges and the helper next to each plugin, which is never moved or changed); it takes the bridge files from `bridge/`.

## Tests

```
sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install wine wine64 wine32:i386 xvfb libxcb1-dev
bash bridge/test/run_tests.sh
```

It builds the test plugin and the test host in both bitnesses and checks both directions (32-bit plugin in a 64-bit host, 64-bit plugin in a 32-bit host): strings, structures, parameters, small and large chunks, audio with MIDI and SysEx at the right frame, long blocks, accumulating and double processing, MIDI out, callbacks from the audio thread, the GUI thread and another thread, the editor with nested and GUI-started `sizeWindow`, the host's sample rate reaching the plugin unchanged (32000, 22050, 44100, 96000 Hz). Then the CLAP test host loads the CLAP bridge in both directions: descriptor, ports, parameters (values, text, events in and out with gestures), latency, audio with MIDI, SysEx and CLAP note events, MIDI out, editor with resize requests, state (also a 200 KB chunk), a second instance, and an old instrument without synth flag and `effCanDo` (as Woof! checks it). The same test host, built for Linux, loads the Linux CLAP with a 32-bit and with a 64-bit plugin under Wine (editor moved into an X11 window) and the macOS variant built on Linux (`-DVB_FLOATING_GUI`: floating editor, hide / show). Finally the Linux CLAP and the test host are built again with AddressSanitizer + UndefinedBehaviorSanitizer and the Linux tests run once more (any finding or leak fails the run). ThreadSanitizer is not part of the script (it cannot see the helper under Wine and does not know `__atomic_thread_fence`); for a manual check build with `-fsanitize=thread` and the same flags. The workflow `check.yml` runs it on every pull request, together with the patcher's `--selftest` and `--cli` (as the Windows and the Linux patcher), and on macOS builds the universal CLAP, freezes the patcher, lets it write a bundle, verifies the signature and loads the bundle with `smoke.c`.

## Releases

Every push to `main` that touches the bridge or the patcher runs `build.yml`: bridge build and tests, the patcher frozen with PyInstaller on Windows, Linux (ubuntu-22.04, for an old glibc), macOS Apple Silicon and macOS Intel (each with the bridge files of its system and `docs/versions.md`), `--selftest`, and a release `autobuild-r<N>` with `VST2Bridge-Patcher-r<N>-windows.exe`, `-linux.tar.gz`, `-macos-applesilicon.zip` and `-macos-intel.zip`, plus `VST2Bridge-DevKit-r<N>.zip` for host developers (`x86_host`: `vst2bridge-x86.dll` + `vst2bridge-host64.exe`, `x64_host`: `vst2bridge-x64.dll` + `vst2bridge-host32.exe`; see [Host integration](host-integration.md)). The tag, the asset names and `--selftest-out` are a contract with `utility/selfupdate.py` (Windows: the new exe next to the old one; Linux: the binary is replaced; macOS: the .app is swapped).

Add a row to `docs/versions.md` for every change that ends up in a release: `| next | | <change> |`, newest on top. The workflow replaces `next` with the release number and date.

## License

The project is licensed under the LGPL v3 (`LICENSE`; the GPL v3 text it builds on is included in the same file). The CLAP headers in `bridge/clap/` are MIT-licensed third-party files and keep their own `LICENSE`. New source files need no extra header; the `LICENSE` file covers them.
