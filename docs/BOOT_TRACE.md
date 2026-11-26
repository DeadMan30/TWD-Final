# Boot Trace to Main Menu

Playability-first roadmap tracing the critical path of `WalkingDead4.exe` from `WinMain`
through `_boot.lua`, renderer initialization, audio/input setup, and `GameEngine::Loop`
into the main menu.

Instead of decompiling static third-party libraries (`openssl` 3,293 funcs, `bullet` 1,028 funcs,
`crt` 4,405 funcs, `enlighten` 650 funcs, `curl` 626 funcs, `jsoncpp` 215 funcs, `zlib` 35 funcs,
and remaining internal Lua 5.2 runtime internals), the port links native Android/upstream
equivalents, wraps platform/graphics/input APIs in [runtime/](../runtime/), and stubs PC-only
telemetry, crash reporting, and launcher SDKs.

## 1. Critical-Path Engine Functions

| Stage | Address | Size | Role / Anchors Observed in `WalkingDead4.exe` | Target Strategy |
|---|---:|---:|---|---|
| Entry (`WinMain`) | `0x14002f7b0` | `631B` | Creates host window/context, invokes engine + script boot | Replace / drive via Android `GameActivity` |
| Engine Init | `0x14006d450` | `125B` | Initializes core engine singletons and queues (`0x140042e30`, `0x140057c50`) | Keep/trace init chain |
| Script & VFS Boot | `0x14006d250` | `446B` | Loads `_boot.lua`; calls `0x140066090` & `0x140077c70` | Hook/verify VFS + `_boot.lua` boot |
| VFS / Prefs Mount | `0x140066090` | `1,727B` | `PropertySet`, `ttcache:prefs.prop`, `<Temp>/`, `<DiskCache>/` | Map paths to Android internal storage |
| Lua Script Runner | `0x140077c70` | `617B` | Executes boot script entries (`Run `, `true`, `false`) | Back by Lua C-API (`lua_api.c` / upstream Lua) |
| Render Device Init | `0x1405d9f10` | `1,709B` | Reads `"Window Size"`, `"Fullscreen Size"`, `"Windowed"`, `"FX Brush Quality"` | Drive [gfx_shim.c](../runtime/gfx_shim.c) |
| D3D11 Wrapper | `0x1405d8b60` | `827B` | Only caller of `d3d11.dll::D3D11CreateDevice` (`"DirectX Error"`) | Replaced by `twd_d3d11_create_device_shim` -> Vulkan/GLES |
| Media / Audio Init | `0x1403d38a0` | `184B` | Only caller of `MFPlat.DLL::MFStartup` (alongside `fmodstudio64.dll`) | Handled by `twd_media_startup` + Android FMOD |
| Input Manager Init | `0x14057e2d0` | `495B` | Calls `DirectInput8Create` + `RegisterRawInputDevices` | Bridged by `twd_input_*` (`subsystems.c`) |
| Gamepad Poll | `0x14057bce0` | `3,530B` | Calls `XInputGetState` / `XInputSetState` | Bridged by `twd_input_get_gamepad_state` / rumble |
| Main Loop | `0x140067fd0` | `1,359B` | `gameloop.cpp` (`GameEngine::Loop`, calls Render `0x140068560`, Scene `0x1402323a0`, SaveLoad `0x140222410`) | Drive frame tick, input, present & scene loading |
| Frame Render Tick | `0x140068560` | `1,001B` | `"Render"`, `"Rendering"` tick inside `GameEngine::Loop` | Hook to `twd_gfx_begin_frame` / `present` |
| Scene Lifecycle | `0x1402323a0` | `4,764B` | `EngineOnSceneOpenFinished` (drives menu/scene activation) | Trace for title screen transition |
| Save / Load Manager | `0x140222410` | `3,627B` | `saveloadmanager.cpp` (`"Load Game PatchSet"`) | Needs POSIX save dir (`twd_app_data_dir`) |

## 2. Non-Essential PC Subsystems to Stub

| Address | Size | Strings / Imports | Action |
|---|---:|---|---|
| `0x14035b640` | `1,000B` | `RailNeedRestartAppForCheckingEnvironment`, `RailInitialize`, `RailRegisterEvent`, `RailFireEvents` | Stub Tencent WeGame Rail SDK as no-op |
| `0x1401e60c0` | `1,120B` | `X-TTG-customercohort` | Stub telemetry/cohort HTTP reporting |
| `0x14057e780` | `1,756B` | `ROOT\CIMV2`, `Win32_BaseBoard`, `Win32_BIOS`, `SerialNumber`, `MachineGuid` (`CoCreateInstance`) | Stub WMI hardware fingerprinting (`twd_com_create_instance_stub`) |
| `0x14057f750` | `439B` | `Telltale Games` (`CoCreateInstance`) | Stub PC Shell/COM helper |
| `0x140bbb400` | `1,504B` | `Breakpad/1.0 (Windows)` (`WININET.dll::InternetOpenW`) | Stub Win32 crash upload (`twd_wininet_offline_status`) |
| `0x140ab0d90` | `600B` | `openssl-cmake/crypto/rand/rand_win.c` (`BitBlt` / `GetDeviceCaps`) | Bypassed by linking upstream OpenSSL / BoringSSL (`getentropy`) |
