# Status and handoff

Read this first when resuming work. It records what is done, how it was measured, and the next concrete steps.

Target: Pokémon Legends: Arceus v1.1.1, main build ID `AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E`.

## Where it stands (2026-10-16)

**The recompiled game reaches Hisui.** On Linux, through Ark's NVN-on-Vulkan layer (tested on lavapipe): logo,
first-boot language menu, Arceus's realm with dialogue text, appearance selection, name entry (the swkbd HLE answers
"Ark"), the cutscene, the title logo, and then the field: Prelude Beach with terrain, sky, buildings, effects and the
first dialogue ("Are you alive, my boy?!"), around frame 4,300. Scripted A presses (`LARECOMP_INPUT`) drive it.
Characters and Pokémon were invisible everywhere: the game skins them with transform feedback, now emulated (below).
On Windows (RX 9070 XT) the user plays in the field; it runs at 6-11 fps there, and the renderer's own CPU time is
small (~35 ms per second), so the cost is in the game's threads (being profiled with `LARECOMP_PROF_SVC`).

| Milestone | State |
|---|---|
| 1. Translate all code to C, compile it | **done** |
| 2. Headless boot to the first GPU call | **done** (Linux x86-64) |
| 3. NVN on Vulkan + shader translation: first frame | **done**: logo, language menu, opening scene with text |
| 4. Audio out, input, saves, the field | later |

## 60 fps (ARK_60FPS=1), what is known (2026-10-18)

- **Clock:** the game steps by a frame time from a table of present-interval durations (main+0x397E8D0, entry for
  interval 2 = 33,333,334 ns). Ark halves that entry and ignores the game's SetPresentInterval(2). Probes at 60 fps
  show every `ns / 1e9` conversion in main (about 60 sites) receiving 16,666,667 ns.
- **Second table (main+0x39848E0) stays at 1/30 s.** Its only reader (main+0x2B43450) computes game-frame steps as
  measured frame time / table[interval]; halving it ran those counters (model/material animations, UI) at double speed.
  This was the "flame, water, text boxes, some animations too fast" report. `ARK_60FPS_TABLE2=1` restores the old
  behaviour for comparison. Let's Go recomp found the same formula in its engine (raulzrrs/letsgo-recomp,
  docs/PESQUISA-60FPS.md).
- **Skeletal animation is authored at 60 fps** (player +4 = 60; main+0x27C248C adds dt * 60 * speed), so it steps 2
  frames per frame at 30 fps and 1 at 60: same speed. Particles step in 1/60 s units (main+0x2B90024): also right.
- **Dialogue text printer** (main+0xFBFBD0, prints N characters per call and counts wait frames) is per call: variant
  groups 64/65 skip it every other frame.
- **Fixed 1/30 s steps in code**: 42 MOVK sites switched at run time (variants.py groups 1/2/4/8).
- **Method:** `ARK_TIMESCAN=<frame>` or F10 lists floats that change by the same step every frame; the same scene at
  30 and 60 fps should show every step halved at 60. On the beach (30: frame ~4400, 60: ~8800) all matched groups
  halve after the table fix. `ARK_TIMESCAN_WATCH`, `ARK_WATCH`, `ARK_GEN_PROBES` + `ARK_PROBE_*` name the code
  writing a value (Linux watchpoints single-step and log old -> new).
- **Not yet checked:** battles, captures and thrown items (Let's Go advances these per update, so its 60 fps mode
  keeps them at 30).

## Milestone 1: translation

| Module | Functions | Resolved switch tables | Functions on the "any instruction" fallback | Generated C |
|---|---|---|---|---|
| main | 161,320 | 1,805 | 39 | ~800 MB |
| sdk | 38,295 | 574 | 31 | ~95 MB |
| multimedia (subsdk0) | 13,691 | 330 | 2 | ~55 MB |
| glslc (subsdk1) | 13,944 | 1,195 | 30 | ~100 MB |

- **Untranslatable instructions: 0.** Ark added 64-bit `LDXP`/`STXP` (and acquire/release forms), ~3,200 sites in
  main (`lifter.py`; `cpu.h: ldxp64/stxp64`, 128-bit `cmpxchg16b`; unit-tested for success, stale reservation and
  interference).
- **Translator correctness:** `tools/lifter/test_lift.py` runs each translated instruction in C and in Unicorn from
  random states and compares registers, flags, SIMD registers and memory. On this game's own instructions: main 20,000
  encodings, sdk 10,000, multimedia 6,000, glslc 6,000. That's ~126,000 cases, **0 mismatches**. Exclusives are skipped
  by that tester, as in LAS.
- **Switch tables** (Ark changes to `gen.py`): PLA's switches load the index from memory before the bounds check
  (pages are now mapped on demand during the emulated replay), some have no bounds check (masked index), and all use
  `adr` for the table base, which LAS's generator took for a function start, splitting each switch from its own cases.
  Resolved tables in main went from 1 to 1,805.
- **Fallback only where needed:** LAS gives every function with a jump table a label at every instruction plus a
  switch over all of them. Ark does that only for functions with an *unresolved* table (102 in all). A resolved table
  whose target isn't one of its verified cases stops with `rt_jt_miss` (`ARK_TABLE_FALLBACK=1` restores LAS's
  behaviour). One glslc function went from over 5 minutes of compile time to 2 seconds.
- **Compile:** `make -j objs` builds all 1,796 files with clang `-O1`: **0 errors**, 38 minutes on 2 cores (370 MB of
  objects). Windows: chunks of all four modules cross-compile with llvm-mingw clang 18 without errors (spot check;
  the full Windows build is not done yet).

## Milestone 2: headless boot

`make -j ark` links runtime + game into `build/ark` (311 MB). Run: `build/ark <game dir>` with
`<game dir>/exefs/` (main, sdk, subsdk0, subsdk1, main.npdm) and `<game dir>/romfs.bin`.

The runtime is LAS Recomp's (MIT), adapted:

- **Loader:** four modules (`main`, `multimedia`, `glslc`, `sdk`) at the `tools/layout.py` bases; rtld's job done
  by `loader.c`.
- **Entry:** this SDK's `nn::init::Start` takes three callbacks (`_ZN2nn4init5StartEmmPFvvES2_S2_`). The order, read
  from the translated code: `_init_libc0`, `nnosInitialize`, **x2** (init sdk, multimedia, glslc), `_init_libc1`,
  `nninitInitializeSdkModule`, `nninitStartup`, `_init_libc2`, **x3** (init main), `nnMain`, **x4** (fini),
  `nninitFinalizeSdkModule`, `nnosQuickExit`.
- **Virtual-address memory** (the game's heap): `main.npdm` has a 16 MB system resource size, so the SDK's
  `nn::mem::StandardAllocator` runs in VAMM mode and reserves **32 GB** of address space from the *alias region*
  (`svcGetInfo` 2/3). Changes:
  1. Guest window 12.5 GB → 64 GB (`GUEST_HI` = `0x80_0000_0000`).
  2. Alias region 4 GB → 48 GB at `0x74_0000_0000`.
  3. `svcGetInfo` 16/17 report the system resource size.
  4. `svcMapPhysicalMemory` keeps the contents of pages already mapped (LAS zeroed the whole range, which wiped the
     heap's own header), `svcUnmapPhysicalMemory` discards pages (`plat_discard`).
  5. Physical mappings are merged in the region table (LAS's 1,024-entry table would overflow).
- **Headless:** `-DARK_HEADLESS`, with a stand-in for the few SDL3 audio calls (`runtime/headless/SDL3/SDL.h`): no
  output device, the audio services still work. The log goes to stderr (`ARK_LOG_DIR=<dir>` writes `ark.log` there).
- LAS-only features (randomizer, save editor, overlay menu) are no-ops in `runtime/stubs.c`.

### The boot log, in order

Services: `lm`, `apm`, `appletOE`, `aoc:u`, `ectx:aw` (logging stub), `pctl` (+`:s :a :r`), `time:u`, `hid`,
`fsp-srv`. Then `init main`, 9 threads, RomFS opened and indexed, then NVN: `nvnDeviceBuilderSetDefaults`,
`nvnDeviceBuilderSetFlags`, `nvnDeviceInitialize`, then `nvnTextureBuilder*` (SetDefaults, SetDevice, SetFlags,
SetLevels, SetFormat, SetDepthStencilMode, SetStride, SetSwizzle, SetTarget, SetSize2D, GetStorageAlignment,
GetStorageSize). It crashes right after, because the stubbed `GetStorageSize` returned 0 and the game used the
result. That is the start of milestone 3.

## Milestone 3: NVN on Vulkan (in progress)

- **NVN layer:** LAS Recomp's `runtime/nvn/` (objects, command buffers, Maxwell shader translation, Vulkan backend).
  The game asks for 524 NVN functions; 495 have implementations, and the rest log once and return 0.
  `vulkan/device.c` enables only the device features that are present (`ARK_FEAT`).
- **Shaders:** Maxwell → GLSL (`maxwell/translate.c`) → SPIR-V (shaderc). Ark added:
  - `RED` (global reductions), `VMNMX`, `TMML` (LOD query; the effect shaders use it), `LEA` / `LEA.HI`.
  - `SUST`/`SURED` are accepted, but **their writes are dropped**: storage images aren't bound to Vulkan yet
    (`CMD_BIND_IMAGES` is recorded but not used). The programs that hit this are a per-frame depth / tile pass
    (480x270, no colour output); they are logged as "image stores dropped". `SULD` isn't implemented.
- **Shader global memory (LDG/STG/RED) and in-place vertex data** (the big one). LAS imported its game's heap
  (`HEAP_BASE`) as one GPU buffer. Legends: Arceus never uses that heap: its SDK runs in virtual-address-memory mode
  (main.npdm system resource; without it `svcBreak`), so everything lives in `svcMapPhysicalMemory` mappings in the
  alias region, in two clusters: ~600 maps of 256-512 KB from `0x74_0000_0000` (span ~1.25 GB) and one 3.2 GB map at
  `0x7b_77b0_0000`. The text renderer's vertex shader reads its per-glyph data with LDG from there, so text was
  invisible. Now (`exec.c gmem_update`):
  - the two clusters are **arenas**, imported (`VK_EXT_external_memory_host`) in **chunks** of the device's buffer
    limit (largest power of two within `maxStorageBufferRange` and `maxMemoryAllocationSize`, at most 1 GB: 128 MB
    on lavapipe);
  - every program sees them through one global descriptor set (`GMEM_SET` = 2): an array of chunk buffers indexed
    `nonuniformEXT`, and a small uniform with the arena bases / sizes (`maxwell/shader.h`);
  - vertex / index buffers inside a chunk are bound in place (no per-draw copy).
  - The kernel exposes the mappings (`kernel_phys_regions`, `kernel_phys_gen`); arena 0 grows chunk by chunk.
    Memory cost: imports pin their pages, including the unmapped gaps inside arena 0 (~1 GB).
- **Transform feedback** (characters, Pokémon): the game skins models by drawing their vertices as points with a
  vertex-only program, rasterizer discard and transform feedback, then draws the captured buffers. The layout comes
  from the program's NVN control section (`exec.c xfb_layout`: flag at 0x6ED, per-buffer enables at 0x6E8, u16 strides
  at 0x6DC, varyings at 0xDC + 0x80 b + 4 j with component counts 0x400 later); a vertex key with `xfb` makes the
  translator append stores of those outputs through global memory (`XfbInfo`, set 0 binding 20, has each buffer's
  address for the draw's first vertex). Capture draws run in an attachment-less pass with a discarding pipeline; the
  next pass's barrier publishes them. Begin/End/Pause/Resume keep per-buffer offsets in the control buffer (Ark's own
  format), which `DrawTransformFeedback` reads. Vertex-only programs without capture are drawn with an empty fragment
  shader.
- **CPU-written textures:** `nvnMemoryPoolFlushMappedRange` / `nvnBufferFlushMappedRange` ranges now mark
  overlapping textures for a full re-hash at their next use (`textures.c backend_pool_written`).
- **Translator fixes found on the way** (`tools/lifter/gen.py`): jump-table bases resolved along control-flow
  predecessors (an `adrp` on another path had overwritten one); masked tables found up to 48 instructions back and
  with `ubfx` indexes; an `adr` feeding a `br` on the same register within 48 instructions is a table base, not a
  function start (main: 1,817 tables, 29 all-label fallbacks).
- **Kernel fixes found on the way:**
  - A timed condition-variable wait that was signalled while its mutex was held now waits for the mutex without a
    deadline (Horizon dequeues it at the signal). Timing out there dropped the signal, and every guest thread ended
    up asleep (seen once in the opening).
  - A timed condition-variable wait that times out returns without re-acquiring the mutex (Horizon semantics; the
    SDK re-locks). LAS re-acquired it, which deadlocked.
  - A wake that races the timeout is kept.
  - A condition-variable wait raises the key's has-waiters flag before releasing the mutex, as Horizon does. The SDK
    only calls SignalProcessWideKey when that flag is set, so releasing first lost wakeups: the game froze on a black
    screen after the language menu, its main thread asleep on a semaphore whose count was already 1.
- **Hang watchdog** (`runtime/main.c`): no frame presented for 20 s logs every live guest thread's wait state and
  frame-pointer chain (`ARK_WATCHDOG=0` turns it off; `kill -USR1` does the same on Linux).
- **setjmp/longjmp** (the game's Lua): each guest `setjmp` call site gets a host `setjmp` checkpoint keyed by the
  guest `jmp_buf`, and the guest `longjmp` is replaced (`runtime/hle/setjmp.c`, `gen.py: find_setjmp_entries`).
- **Services added:**
  - `hwopus` on libopus
  - `friend:*` (offline: empty lists)
  - `csrng`
- **Measured:** first logo frame at about 5 s. On lavapipe (CPU Vulkan, 2 cores): about 4.6 fps, 130 draws/frame,
  0 shader translation failures.

### Testing without a GPU

The build machine has no GPU. Tests use **lavapipe** (Mesa 25.0.7's CPU Vulkan driver) with `SDL_VIDEO_DRIVER=offscreen`.
SwiftShader lacks `shaderInt64` and crashes on the shaders. Building lavapipe here took:
- LLVM 18.1.8 (release tarball)
- flex 2.6.4
- a stub `libtinfo.so.5`
- libdrm 2.4.97 with its version bumped to 2.4.124, plus stubs
- `-Dcpp_rtti=false`
- a one-line patch to `lvp_device.c` so its WSI platform is always on, for a headless surface

See [TESTING-lavapipe.md](TESTING-lavapipe.md). `LARECOMP_SHOT_AT=5,40` saves those frames to `build/frames/` in the game folder, `LARECOMP_SAVE_MSL=1` keeps the
generated GLSL in `build/glsl/`, `ARK_RUN_SECONDS=N` exits after N seconds, and `ARK_LOG_OPEN=1` logs every file open.

### Next

1. **RomFS:** the test image has boot, sound (minus `Default.pck`), core, UI, resident, title, story and Pokémon
   and field folders (~3.7 GB of 6.4); `tools/pack_romfs.ps1` packs folders into upload-sized zips. Missing: battle
   effects (1.9 GB), `Default.pck` (sound), credits. `romfs_field` (`bin\archive\field bin\field\model bin\effect\field`) and `romfs_pokemon`
   (`bin\archive\pokemon bin\appli\res_pokemon`) are requested; battle effects and `Default.pck` after that.
2. **Storage images:** bind `st.img` as Vulkan storage images and translate `SUST`/`SULD`/`SURED`/`SUATOM`.
3. **Speed on Windows**: 6-11 fps in the field with the renderer mostly idle. `LARECOMP_PROF_SVC=1` now logs each
   guest thread's CPU time beside its svc wait times (`[cpuprof]`, `[svcprof]`), to tell slow game code from threads
   waiting on each other.
4. `pl:u` and `ectx:aw` are logging stubs. The game draws its text from its own fonts, so this hasn't mattered yet.

## Design decisions so far

- **Guest addresses are host addresses** (LAS's model). The modules sit at fixed bases: main `0x7000000000`,
  multimedia `0x7008000000`, glslc `0x700A000000`, sdk `0x700C000000`. The other regions (`runtime/rt.h`): HLE
  stubs `0x7080…`, TLS `0x7090…`, stacks `0x72_0000_0000`, heap `0x72_4000_0000` (3.25 GB), alias
  `0x74_0000_0000` (48 GB). The whole window `0x70_0000_0000`–`0x80_0000_0000` is reserved at start-up.
- **All four modules are translated, the SDK included**, as LAS does. System behaviour comes from below the SDK
  (SVCs, service IPC) plus host replacements for chosen SDK functions (`HLE_FUNC`, bound by symbol name at load).
- **x86-64 hosts use the portable C SIMD lowering** (`--simd c`, `simd.h`); AArch64 hosts can use inline asm.
- **License:** Ark is MIT. LAS-derived files keep LAS's notice (THIRD_PARTY.md). ssbu-recomp is GPLv3 and is not
  copied.
- **No game-derived bytes in git.** `generated/`, `build/` and dumps are ignored. Docs carry measurements and names
  only.

## Rebuilding the environment (for a new session)

This environment is ephemeral. To get back to the current state:

1. Get the exefs (the user's dump; never commit it). The user can re-send `exefs.rar`. For a boot test, a RomFS image:
   `hactool --romfs=romfs.bin` on the update NCA, or `build/mkromfs <romfs folder> romfs.bin` (`make mkromfs`).
   Without the full romfs, a partial image of a few files still boots to the NVN stop.
2. Unicorn: `pip install unicorn`, or `source tools/setup_unicorn.sh` (builds 2.1.1 from GitHub; ~5 min).
3. `make gen EXEFS=<exefs>` (~2 min), `make -j objs` (~40 min on 2 cores), `make -j ark`.
4. Windows cross-compiler: llvm-mingw from
   `https://github.com/mstorsjo/llvm-mingw/releases/download/20240619/llvm-mingw-20240619-ucrt-ubuntu-20.04-x86_64.tar.xz`.
5. Wine for running Windows test builds:
   `https://github.com/Kron4ek/Wine-Builds/releases/download/9.20/wine-9.20-amd64-wow64.tar.xz`.
6. Useful while debugging: `gdb --args build/ark <dir>`, break on `f_<guest addr>`; the `Cpu *` is in `rdi`.
   Symbol names for guest addresses come from the NSOs' symbol tables (the SDK's exports name almost everything).

## 2026-11-03: name entry, character self-shadowing, speed toggle

- **Name entry** (`runtime/hle/swkbd.c`, `runtime/ui_prompt.c`): the game wants the keyboard's answer in **UTF-16**
  (`KeyboardConfig.isUseUtf8`, +0x3bd, is 0). Ark wrote UTF-8, so "Ark" was read as U+7241 '牁' + 'k': the
  "one kanji" name. The answer now follows the flag and is cut to `textMaxLength` (UTF-16 units, whole characters).
  Windowed builds ask with a native text box on the window's thread (`SDL_RunOnMainThread`): an NSAlert with a
  keyboard symbol on macOS (through the Objective-C runtime; links `objc` + AppKit), an in-memory dialog template on
  Windows, zenity / kdialog on Linux. IME input works. The box shows the game's header / guide text ("What is your
  name?") and starts from its initial text; closing it gives the game window the keyboard back (`SDL_RaiseWindow`).
  Cancel or an empty answer keeps the initial text (else "Ark"). `ARK_KEYBOARD_TEXT=<name>` answers without asking
  (scripted runs, headless builds, where `ui_prompt.c` is left out).
- **Characters shadowed themselves** (white clothes and the starter Pokémon covered in flickering grey facets and
  streaks). Legends: Arceus shades in a deferred pass; its screen-space shadows (`8244a26000`/`8244a26200`) compare
  each pixel's depth with the cascades (D32F 1024x1024x2, filtered by the game's own 16-tap compare) and also fetch
  the casters' **object ids** from an R32_UINT array beside them (format 0x03, written by the casters' `c1`): a
  surface of class 2 / 3 (G-buffer alpha bits 6..7) is not shadowed by its own id. Two translator bugs broke that:
  1. **TLDS 2D-array LOD-zero** (texs info 8) took x from Ra+1 and y from Rb; Maxwell's layout is layer Ra, x Rb,
     y Rb+1 (Ryujinx `TldsTarget.Texture2DArrayLodZero`). Every id lookup read the wrong texel, so the self-shadow
     check failed everywhere: not specific to macOS (`maxwell/translate.c glsl_texs` and the MSL twin).
  2. **Integer textures read through float samplers** (undefined in Vulkan; Maxwell returns the raw bits, MoltenVK on
     Apple GPUs need not). `ShaderKey.tex_uint` / `tex_sint` (per handle slot, filled per draw in `exec.c draw_` from
     the bound textures' formats, part of the program hash) make those slots `usampler` / `isampler` with `uvec4` /
     `ivec4` results (`tex_int_kind`).
  Ruled out on the way, and not kept: polygon offset (the casters draw without it, D32F), MoltenVK fast math, filter
  precision (in-shader exact bilinear), sampler compare / filter support, depth snapshot reuse, cascade layers,
  skinning data. Diagnosis that worked: hiding the shadow map, then a test bias on the cascade reads (0.01 needed:
  far beyond precision), then reading the screen-space pass's GLSL from an F9 dump.
- **Diagnostics kept:** `ARK_LOG_SHADOW=1` logs each kind of draw into a square 512..4096 target (the cascades) once,
  with formats, stages, polygon offset and why a draw was skipped; F9 also saves each cascade layer after its pass
  (`build/shadow_L<n>.pgm`, raw floats in `.f32`).
- **Game speed** (`runtime/patches.c ark_speed`): F8 cycles x1 -> x2 -> x4 -> unlimited (window title), `ARK_SPEED=
  1|2|4|0` at start. Scales Ark's present pacing (`nvn/objects.c present_pace`) and the 60 Hz vsync timer
  (`hle/vi.c`); audio follows the game's pace.
- **CMake headless builds** leave `ui_prompt.c` out (the regex in `CMakeLists.txt` of the PLARecomp folder).
