# Player timing and rendering

These measurements use the USA/Europe Xbox executable with SHA-256 `6B2451C01E2432BAB5E3AA0C9DFE2EB03F842C67F1417CBA158DD7A5C4AED75B`. The disc contains the English, French and German release.

## Timing evidence

The engine creates its frame interval at 0x00018B32. The normal interval is 1/30 second. The comparison at 0x00018EC2 gates the CPU wait at 0x00018EE1. Windowed play replaces that wait with host pacing at guest scanout. Each gameplay presentation displays a frame produced by the original draw commands.

The engine samples its own clock at 0x000183C0, caps a step at 50 milliseconds, smooths ten samples, and stores elapsed seconds per update at engine offset 0x290 (0x0001855F). The game loop passes this value to the scene and game logic. The engine counter increments at 0x00018EC9. Diagnostics record this counter, accumulated simulation time, guest flips, movie overlays and window presentations separately. For display caps below 20 Hz, the engine runs at a multiple of the selected cap and the player displays complete frames at the requested rate.

Three verified instructions at 0x00023CC7, 0x00023D8A and 0x00024098 push a fixed 0.033 seconds into effect and interface updates. One downstream consumer at 0x00076EBA subtracts this argument from an object's timer. At custom rates the generated code scales the authored step by 30 times the current engine delta. The original literal remains in use at the 30 Hz setting and in offline CPU checks. XMV overlays follow the selected Xbox display mode independently of the gameplay cap.

Holding the left stick to the right for one second in the library produced 30 updates at 30 Hz and 60 updates at 60 Hz. Their simulation clocks advanced 999.8 and 1000.0 milliseconds. The stationary camera was at (−158.649, 1.879, 1.543). After moving, its positions were (−156.579, 2.571, 0.288) at 30 Hz and (−156.633, 2.617, 0.320) at 60 Hz. The camera follows the characters, and the snapshots were taken two rendered frames after release, so the settling intervals differ by one thirtieth of a second. These measurements cover this movement in the library.

### Library ghosts and collectibles

The library's `level\EP1\L01\gen\EP1L01.xbr` contains the original motion routines. In `medghostlevelone`, `_Waving` adds a signed increment at 0x8389 to the hover phase, reverses direction at ±1, and at 0x849A sets the ghost's height relative to its base. The height offset is ±0.15 × (1 − phase²) in world units. In `coll_level1`, `_Coll_BaseFUNC_Spin` loads the angle at 0x22C7, converts it to radians and sends it to the object's rotation method at 0x22E6. Both inputs were authored per gameplay update at 30 Hz. The recompilation scales those inputs using elapsed simulation time. The named routines and corresponding instructions were verified in 31 ghost and 37 collectible module variants on the disc.

On the scripted first library ghost encounter, a temporary trace recorded individual actors' hover phase, height, and rotation arguments. At 120 Hz, the old per-update inputs advanced the hover at four times its 30 Hz rate and rotated the sampled pickups at 480°/s. With elapsed-time scaling, the median pickup speed was 120°/s at both 30 and 120 Hz. The hover advanced by 30 original increments per second at both rates, and its measured height stayed within ±0.15 world units. Measurements used consecutive updates of the same actor and excluded direction reversals when comparing phase increments.

## Widescreen verification

The SDK creates its display color surfaces at 0x0011A0A8–0x0011A0E2. Each entry in the device's color-surface table points to an Xbox surface with a physical color address at offset 4. The player widens the final horizontal projection for draws to those surfaces and keeps shadow maps at their original shape. The RenderWare perspective frustum builder at 0x000A2990 reads its horizontal extent at 0x000A29F4, 0x000A29FA and 0x000A29FD. These extents widen for the scene cameras in the two slots processed by 0x00024060. Movie overlays are composed at the original scanout resolution before the player fits them to the display.

Stationary library captures at scanout 1162 had the same camera position and orientation in both aspect modes. The 854 × 480 capture showed more of the room on both sides. In the original widescreen implementation the centered 640 × 480 region had mean absolute 8-bit color differences of 0.82 for Shaggy and 0.51 for a region of scenery against the 4:3 reference.

The engine also builds two sets of visibility planes in 0x000307F0 and 0x0004EDD0. Both get their horizontal extent from 0x000464D0. In the November 22 library capture, actor 0x2A0000A3 uses the texture `SCOOBYSNACK2`. Its origin projects to x=16 in the 640-pixel widescreen backbuffer. Running the original object test at 0x00039060 on the captured actor and camera returned 0 with the old planes and 1 with the widened planes. Replaying the camera pose in the native player rendered the snack box inside the purple glow.

All 37 indexed disc panels with `INGAME_MAIN_WIN` and `PAUSE_MAIN_WIN` assign their HUD rectangles before calling the naming stub at 0x00048300 through HUD API slot 0x28. The title panel registers `TITLE_MAIN_WIN` through the same slot. The renderer associates each widget's anchor with the draw commands emitted by 0x00077090. Nested radar markers inherit their parent's anchor. The library portrait, radar, and door prompt match their original edge margins in 640 × 480 and 854 × 480 captures. The pause panel uses its original 4:3 presentation. At 854 × 480 the centered pause image had 107 black pixels on each side and 99.8% of its pixels matched the 4:3 capture exactly.

The main menu draws a 3D room behind its logo and controls. It uses the same widescreen projection as gameplay. At scanout 925 the 854 × 480 capture showed more of the doorway and wall on either side than the 640 × 480 capture. The purple part of the logo measured 301 × 94 pixels in both, and the highlighted “Start a New Game” label measured 168 × 14 pixels in both.

### Ultrawide 21:9

The 21:9 setting uses the 64:27 shape of 2560 × 1080 displays. It reports the same Xbox widescreen video flag as 16:9. The frustum extents above widen by 16/9 instead of 4/3. Draws to the display color surfaces narrow to 9/16 of the 640-pixel target, and an anchored widget moves 7/16 of the half-width toward its edge. The native target stays 640 × 480, so at 1x the 4:3 part of the scene covers 360 target pixels instead of 480.

Captures at 1138 × 480 used the same scripted routes as the 16:9 captures:

- **Library at scanout 1162:** the portrait matched the 16:9 capture best at the same left offset. The compass matched best 284 pixels further right (1138 − 854), which preserves its right margin.
- **Door at scanout 1256:** the A OPEN prompt kept its left margin.
- **Pause panel:** it kept its 4:3 presentation with wider black bars.
- **Title room at scanouts 840 and 960:** the room continues to the right edge. Its geometry ends about 48 pixels from the left edge, where the frame shows the black clear color beside a near-black wall. The logo, menu and Recomp Options pages stay centered at their 4:3 size.
- **Recomp Options:** every one of the 198 `font0.ft` files on the disc has the digits and ':' needed for the 4:3, 16:9 and 21:9 labels. Changing the setting from 16:9 to 21:9 on the Display page applied at once and saved `aspect_ratio=21:9`.

Area transitions draw their fade and cinematic bars as untextured HUD quads of pre-transformed vertices. The SDK program for these vertices is at 0x00121AC0 in the D3D section (file offset 0x112D40, 11 instructions). It writes oPos = v0 × c0 + c1. Leaving the library through the double doors (scanouts 1280–1340 of the Ghost route), every such quad had c0.x = 1 and c1.x = 0.53125 and spanned screen x −129.5 to 770.5. A 0.75 squeeze still covers 640 pixels, but 0.5625 covers only 506, so at 21:9 the scene showed at both sides of the fade and bars. Draws with that program that reach both edges of the 640-pixel surface and sample no artwork now skip the squeeze. Rendered surfaces do not count as artwork. In the same captures the fade and bars cover the full 1138-pixel frame. No draw in the library, door, title or pause captures met the condition.

The title panel in `LEVEL/INTRO/GEN/INTRO.XBR` shares the `TITLEWIN02_BGRD_ANIM` widget across options pages. In the 16:9 Recomp Options capture at scanout 6028, draw 38 is its dimming layer. The four vertices form an untextured triangle strip with a single packed color, `0xCA393939`. The patterned artwork follows in a separate 478-vertex draw. At 16:9 and 21:9 only the dimming strip in this widget uses the full display width.

Two uncapped Library benchmark pairs (`tools/Benchmark.ps1 -Scene Library -RefreshRate 0 -Frames 360 -Aspect …`) ran at 640 × 480 native resolution on the reference machine. The 16:9 runs had medians of 2.48 and 2.40 ms and 99th percentiles of 3.60 and 3.21 ms. The 21:9 runs had medians of 2.61 and 2.45 ms and 99th percentiles of 3.70 and 3.35 ms. Every run counted 360 game updates and 360 guest flips. With the wider frustum, more objects pass culling at the sides.

## Upscaled HUD

The HUD draw at 0x0004A70D–0x0004A730 passes the HUD root to 0x00077090. That routine draws each visible widget and its children. Its hook at 0x000770CA runs after visibility and clipping and before the widget's own draws. The hook at 0x0004A733 runs after 0x00077090 returns. When **Upscaled HUD** is on, the player records the push-buffer position at each hook. Textures sampled by draws between the first widget and 0x0004A733 are HUD, menu or title art. Rendered surfaces, cube maps, volume textures and images larger than 1024 texels are excluded.

Each marked texture is identified by an FNV-1a hash of its format, dimensions, mip count, linear pitch, texels and palette. It is enlarged once on the GPU with Real-ESRGAN realesr-animevideov3, or with a clamped Catmull-Rom filter for soft or very small images. The enlargement factor is min(internal resolution, 4), and the option has no effect at 1x. Enlargements are kept by content up to 256 MiB, so artwork reloaded into a new allocation reuses its result. The driver, shaders and model reader were adapted from the Super Monkey Ball Deluxe recompilation, which validated the GPU path against a float reference to within 3/255. Work runs after each presentation for 12% of the measured interval (0.25 to 2 ms).

On the first draw of a new texture, magnification uses four neighbouring source texels, weighting colour by opacity. The finished texture takes its opacity and 55% of its colour from the clamped Catmull-Rom reconstruction, with the remaining 45% of colour supplied by the network. Transparent assets therefore need a single network pass. The renderer blends in a newly finished texture over 100 ms. A disk hit uses the finished texture immediately. Soft and tiny assets use only the clamped reconstruction.

A 128 × 128 transparent line-art sample at 3x on an RTX 5060 Ti took 7.94 ms for a complete uncached enlargement and readback, down from 12.24 ms with full-strength colour and neural alpha (median of seven runs after warm-up, with the frame budget disabled for measurement). An isolated draw comparison confirmed that filtering was already visible before the first background step. The common and general texture shaders produced the same test image.

A 600-frame Library capture with a Ryzen 5 5600X and RTX 5060 Ti used 21:9, 3x internal resolution, a 360 Hz cap and VSync off. With an empty cache the updated game completed all ten textures and averaged 359.3 presentations/s (median frame 2.70 ms, p99 3.59 ms). On the repeat visit all ten textures came from disk. The updated warm run averaged 350.2 presentations/s (2.85 ms median, 4.02 ms p99) and the previous release averaged 353.1 (2.83 ms median, 3.76 ms p99).

The original full-strength implementation was measured on the reference machine at 3x internal resolution.

- **Title screen:** the 512 × 256 A8R8G8B8 logo and the 512 × 64 indexed prompt strip were marked and enlarged by the network, within about one second of first appearing. In 1920 × 1080 window captures, their outlines and lettering were sharper than the original bilinear magnification.
- **Library:** 10 textures were enlarged: 9 by the network and 1 by the filter, using 22.3 MB. Comparing captures at the same scanout with the option on and off, 36.1% of the portrait and fear-meter pixels and 8.8% of the compass pixels changed. Walls, display cases and floor changed by at most 0.1%. The characters changed by 2.0%, consistent with animation timing.
- **Library benchmark at 360 Hz** (two run pairs): medians were 2.90 and 2.80 ms with the option on, against 2.79 and 2.77 ms off. 99th percentiles were 4.10 and 3.63 ms on, against 3.64 and 3.50 ms off. The first run measured while enlargement was still running.

Finished enlargements are also kept on disk in the storage folder's `hud-cache`, one file per content hash and factor (`<hash>-<factor>-v2.hud`). Each file has a 60-byte header (magic `SCMMHUD1`, version, factor, size, network flag, model SHA-256) followed by level 0 as RGBA8; mips are regenerated on load. A file whose header differs from the expected one is ignored. Saves copy to a staging texture and are written once the copy is ready, through a temporary file and a rename. A load refreshes the file's write time, and the folder is pruned to 384 MiB by oldest write time when it exceeds 512 MiB. Pending saves are flushed when the renderer closes. The uninstaller removes the folder's `.hud` and `.tmp` files and the folder itself.

Two hidden Library runs at 3x shared one storage folder and captured scanout 1150. The cold run computed all 10 enlargements (9 network, 1 filter) and saved 10 files (16.7 MB). The warm run loaded all 10 from disk and computed none. The HUD regions of the two captures matched: the compass cell was identical and the portrait cell differed by at most 1 level. The scattered differences elsewhere in the scene (at most 26 levels) come from timing-dependent scene animation between runs.

## Library benchmark

Run `./tools/Benchmark.ps1 -Scene Library -RefreshRate 0 -Frames 360` from the project root on Windows. The script creates an isolated profile, navigates at 30 Hz, switches rate at scanout 1100, and measures from scanout 1130. It saves the scene image, session report and individual frame samples. A rate of zero selects unlimited.

The development machine has a Ryzen 5 5600X and a GeForce RTX 5060 Ti. Native resolution is 640 × 480 with an 854 × 480 output, V-sync disabled, and audio processing enabled with output muted. These development samples use an optimized executable core and renderer. Disc logic modules in these three runs still use the earlier unoptimized build. A two-worker build ran in the background.

| Library run | Guest frames | FPS | Median frame, ms | 99th percentile, ms |
| --- | ---: | ---: | ---: | ---: |
| Original per-draw uploads | 240 | 105.7 | 8.95 | 13.57 |
| Append vertex and index uploads until the buffers wrap | 360 | 115.6 | 8.27 | 14.05 |
| Preserve GPU command order without a CPU wait at every idle packet | 360 | 144.6 | 6.51 | 11.11 |

In the third run, 360 engine updates produced 360 guest flips and 360 presentations. The engine advanced 2495 ms during 2490 ms of measured wall time. The 360 Hz frame budget is 2.778 ms.

An isolated replay used 213 draws per frame from the library command buffer at 0x82087BA0–0x820A7C50. Across 256 iterations, median host submission time fell from 9.24 ms to 1.88 ms with append uploads and to 0.96 ms with ordered idle packets. The final raster matched in every pixel. This test times the submission call and waits for GPU completion before comparing the final image.

An uncapped sample of the game thread attributed 143 of 421 samples to RAM address translation and page checks. The player now takes a short path for accesses contained in one committed RAM page and retains the full checks for aliases, page crossings and invalid addresses.

The next measurements include optimized native code for all 48 disc logic modules loaded in the library. They use the verified widescreen projection and the same hardware, render size and output size as above. The compiler was idle for all three runs. Each title or library sample counted one game update and one guest flip per measured scanout.

| Scene and cap | Game updates | Presentations | Presented FPS | Median update interval, ms | 99th percentile, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Title menu, unlimited | 720 | 720 | 1049.7 | 0.879 | 2.085 |
| Library, unlimited | 1080 | 1080 | 312.7 | 3.162 | 4.488 |
| Library, 10 Hz | 60 | 20 | 10.00 | 33.333 | 33.894 |

The 10 Hz run advanced the engine clock by 2000.0 ms in 1999.6 ms of wall time. The original title menu benchmark uses the `title_camera` module and starts at scanout 830. An earlier route reached an episode title card. At a 360 Hz cap with a compiler running, the title menu produced 480 updates and presentations at 359.4 FPS.

`./tools/Benchmark.ps1 -Scene Walk -RefreshRate 60 -Frames 120` holds the left stick to the right throughout sampling. The engine advanced 1999.9 ms in 120 updates. An unlimited run of this route produced 720 presentations at 317.3 FPS, with a median interval of 3.112 ms and a 99th percentile of 4.069 ms. Use the reported simulation duration when comparing paths at different caps.

In the 213-draw replay, batching the packed FIFO index payloads reduced the median submission time across three runs from 0.83 to 0.77 ms. Reusing unchanged shader constants and batching transform uploads brought the best median to 0.67 ms. Each version produced the same final pixels after 54,528 draws.

Of 204 indexed draws in that replay, 162 started above vertex zero. Uploading the span between the lowest and highest referenced index reduced the total submitted vertex ranges from 51,631 to 17,872 vertices. The median of three replay medians fell from 0.754 to 0.629 ms, with identical final pixels. In a separate 1,440-frame library comparison, median submission time fell from 0.938 to 0.898 ms. Overall frame rate varied between 259 and 265 FPS while two compiler workers ran.

Holding the same stick direction for 30, 60 and 120 updates at the matching rate advanced the engine by 999.9, 1000.0 and 1000.0 ms. The player generated and presented every frame. A 240-update run with two compiler workers averaged 227.2 FPS. It advanced the engine by 1059.8 ms in 1056.6 ms of wall time.

With one compiler running, a 1,440-frame library sample averaged 276.7 FPS before ordinary RAM reads and writes were separated from device access, and 293.5 FPS afterward. Median game-thread cycles fell from 12.30 to 11.73 million. The same native checks covered RAM aliases, cross-page accesses, guarded pages and device reads and writes.

A later Windows build combined the renderer and RAM changes with recompilation in guest-address order. The same machine, with the compiler stopped, produced the following results at 854 × 480 with V-sync off.

| Scene and cap | Frames | Presented FPS | Median interval, ms | 99th percentile, ms |
| --- | ---: | ---: | ---: | ---: |
| Title menu, 360 Hz | 1080 | 357.4 | 2.785 | 3.414 |
| Library, 360 Hz | 2160 | 338.1 | 2.878 | 4.689 |
| Library, unlimited | 2160 | 346.1 | 2.846 | 4.207 |
| Moving in the library, unlimited | 2160 | 350.3 | 2.696 | 5.934 |
| First blue ghost encounter, unlimited | 1080 | 187.7 | 4.995 | 9.544 |

`-Scene Ghost` walks through the library's double doors, dismisses the two save-camera messages and approaches the blue ghost between the staircases. In this capture the encounter averaged 454 draws and 44,242 submitted vertices per frame. The engine advanced 5758.7 ms during 5754.0 ms of wall time. The captured image shows the encounter before the heroes obtain the Tome.

The Windows player requests tearing support when the adapter exposes it. It carries that flag through swap-chain resize and uses it for presentations with V-sync off. This also enables variable-refresh presentation on compatible displays. [Microsoft's DXGI presentation requirements](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays)

## GPU waits in gameplay

At 0x001191B0 the SDK reads the most recently issued fence number from device offset 0x2C and the completed number through offset 0x30. The unsigned comparison at 0x001191CC returns immediately when the requested fence has already completed, including across a counter wrap. During 1,440 frames of the first blue ghost encounter, all 5,759 calls to that routine requested completed fences. The previous host binding spent 607 ms waiting on those calls. The FIFO reservation at 0x001193B9 checks the command reader's position before reusing storage. The native reader has consumed the submitted commands and advanced its GET cursor by the time submission returns.

The following uncapped runs used the same Windows hardware and 854 × 480 output described above, with the compiler idle. Each update produced one guest flip and one presentation.

| Scene | Frames | Earlier FPS | FPS with the SDK fence check | Median interval, ms | 99th percentile, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stationary library | 2160 | 344.3 | 335.5 | 2.898 | 4.365 |
| Moving in the library | 2160 | 370.3 | 375.6 | 2.590 | 3.751 |
| First blue ghost encounter | 1440 | 194.0 | 206.7 | 4.722 | 6.231 |

The encounter averaged 4.3 submissions per frame before the change and 1.3 afterward. Both runs averaged 454 draws per frame. A separate 240 Hz walking test produced 240 game updates and 240 presentations in 1001.8 ms. The engine advanced 1005.1 ms.

## First-frame startup

On the same machine, launching the Windows release with an empty shader cache, a fresh profile, a hidden window and a one-scanout limit took 50.37 seconds. Repeating that command with the populated cache took 0.45 seconds. That build compiled the 26 built-in HLSL shaders and embedded 1,248,752 bytes of shader data in the player. The same fresh-cache run took 0.63 seconds and created zero shader cache files. A repeat took 0.47 seconds. All 26 embedded shader blobs matched the bytes compiled on the original cold run. That release still compiled guest-supplied vertex programs on first use.

## Vertex inputs and indirect calls

The renderer previously bound 17 vertex-buffer slots for each streamed draw because constant attributes occupied the last slot. Moving constant attributes to slot zero lets each draw bind just its declared streams and the constants. Four paired runs of the 213-draw library replay alternated the order of the builds. The median of the four submission medians fell from 0.638 to 0.575 ms. Each run issued 54,528 draws and all eight final images were identical.

The playtest ending at Xbox PC `0x00118FB0` after 7,696 scanouts placed a pair of 16-bit values (Xbox format `0x25`) at byte offset 2 after two 8-bit values (`0x24`). Replaying that declaration with material shader 26 reproduced the crash. The D3D11 debug layer identified the four-byte alignment required for `R16G16_UNORM`. The renderer now splits fields whose offsets violate the native format's alignment and pads uploaded vertex strides. The reported draw completed on the RTX 5060 Ti and on WARP. Eight indexed and non-indexed comparisons per device produced identical pixels from packed and aligned data, with no debug-layer warnings. [Direct3D 11.3 functional specification, section 4.4.6](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#4.4.6%20Element%20Alignment)

A trace of 131,072 indirect calls from the first blue ghost encounter included 1,681 destinations outside the executable's 6,694 compiled entries. A 256-entry cache hit on 98.0% of the full trace. Replaying the addresses against the same target table reduced lookup time from 6.24 to 0.83 ns per call. The cache checks the exact address and only retains functions from the immutable executable. Disc modules still go through the live module resolver so an unload or replacement takes effect.

## GPU notifications

Immediately before a gameplay flip the SDK issues a semaphore write and an NV097 wait-for-idle packet. The graphics device must finish its earlier work in order. The game thread can continue while the native GPU does that work. The scheduler now polls for completed writes. Command buffers, textures, palettes and vertex streams that overlap the notification memory wait before reading it, including through a RAM alias. Video overlays and diagnostic RAM captures also synchronize their reads.

| Uncapped scene | Frames | FPS with a host wait | FPS with polled notifications | Median interval with polling, ms | 99th percentile with polling, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stationary library | 2160 | 331.1 | 363.9 | 2.620 | 4.083 |
| Moving in the library | 2160 | 361.9 | 411.4 | 2.287 | 3.867 |
| First blue ghost encounter | 1440 | 201.5 | 205.3 | 4.748 | 7.483 |

The compared players used the same link inputs apart from the notification binding. Another run with the Windows release averaged 404.9, 418.8 and 227.8 FPS in those three scenes. Each release run produced one presentation per engine update. All three scenes still missed the 2.778 ms budget at the 99th percentile. Native checks covered FIFO commands overwritten by a semaphore, a later submission reading that write through an alias, overlapping and adjacent DMA reads, and notification delivery while the CPU polls.

## Vertex transforms

The loops at 0x000A1F70 and 0x000A2030 transform three-component vertices. The Windows x64 release checks the input, output, matrix and scratch memory ranges once per vertex. It then executes the original floating-point operations in order. If a range check fails or the game enables floating-point traps, the player runs the original instruction-by-instruction path.

For each loop, 256 comparisons against the previous generated code matched guest memory, CPU registers, flags, floating-point state, faults and return address. Cases included overlapping buffers, page faults, rounding modes, infinities and NaNs. In six paired trials of 768,000 vertices each on the Ryzen 5 5600X, the first loop went from 221–223 to 40–41 CPU cycles per vertex. The second went from 195–207 to 39–41 cycles per vertex.

The following uncapped Windows release runs used the same scene routes, native resolution, widescreen output and V-sync setting as the previous release. The before and after columns each contain a single run per scene.

| Scene | Frames | Previous FPS | Final FPS | Final median, ms | Final 99th percentile, ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Stationary library | 2160 | 404.9 | 409.2 | 2.337 | 3.146 |
| Walking in the library | 2160 | 418.8 | 434.7 | 2.192 | 3.105 |
| First blue ghost encounter | 1440 | 227.8 | 224.0 | 4.314 | 6.482 |

The final runs counted one game update, one guest flip and one presentation for every measured frame. Simulation time tracked elapsed time to within 3.1 ms in each run. The loop-level speedup did not produce a measurable FPS gain in the ghost encounter. All three scenes exceeded the 2.778 ms target at the 99th percentile.

## Movie playback timing

The Xbox device initializer selects its display mode at 0x0011A6CB. The Windows binding previously omitted that call. As a result, the display query at 0x00113E60 reported 50 Hz while the recomp delivered 60 vertical blanks per second. XMV used 20 ms per vertical blank to schedule pictures and periodically corrected against its audio clock. The gameplay cap could discard additional movie pictures. The binding now calls the original mode selector, drives the vertical blank clock from its result, and presents every movie overlay. The selected NTSC mode uses 16.68335 ms per field.

On the Windows reference machine, scanouts 101–200 of an uninterrupted boot fall in `MOVIES/WBPH.XMV`. The disc timestamps in this passage advance by 33 or 34 ms. With a 30 FPS gameplay cap and V-sync on, the previous release presented 66 of 100 overlays at 19.70 FPS. The fixed release presented all 100 at 29.97 FPS. The longest interval between presentations fell from 183.31 to 50.05 ms. The median interval in the fixed release was 33.37 ms. Separate runs with gameplay capped at 10 FPS and set to unlimited also displayed all 100 overlays. Run `./tools/Benchmark.ps1 -Scene Movie -Frames 100 -RefreshRate 30` to measure the same passage with V-sync off.

A trace at the original decode call (0x000EBB67) matched the disc timestamp sequence for all 2,477 pictures in LEGAL, WBPH, THQPH, A2MPH and OPENING. In OPENING, the interval from the first picture to the final movie output was 62.289 seconds against the authored 62.295 seconds. Of 1,793 consecutive output intervals with a 32–34 ms timestamp step, 1,759 were between 25 and 42 ms. The longest of those intervals was 50.05 ms. The game subsequently reached its title scene. A separate run skipped the movies, entered the library and produced 240 updates and 240 presentations while walking.

The movie object's play method at 0x00050270 formats `D:\%s%s.xmv` (format string at 0x00150B70) from a folder and the name at offset 4 of the object, and passes the path to the player at 0x0004FE40. The player's first step, 0x0001BCC0, opens the file and returns its size. When the file cannot be opened, the player returns before creating a decoder, and the boot sequence continues. The disc holds the boot movies as `MOVIES\LEGAL`, `WBPH`, `THQPH`, `A2MPH` and `OPENING.xmv`, with French and German `OPENING.xmv` in `MOVIES\FR` and `MOVIES\DE`. The optional **Skip intro movies** enhancement answers the first open of each boot movie as missing. In a hidden boot with no input, scanout 390 showed the "Please press Start to begin" title screen, with no movie overlays presented. With the option off, scanout 390 fell in THQPH, with 386 overlays presented. Later requests for the same files open them normally.

## Movie framing and filtering

All 46 XMV headers in the supported disc's `MOVIES` directory specify 640 × 480. Samples near 2 and 5 seconds in all 33 episode reels showed an approximately 640 × 320 picture with encoded black bars. In `MOVIES/EP01/EP01A.xmv`, picture content reached row 78. The opening and its French and German versions used the full height. Widescreen playback therefore starts with up to 80 removable rows at each edge for episode reels and retains the full frame for other movies. A decoded row with visible content reduces the crop for the rest of that reel. The Xbox player programs a 639 × 479 overlay at the top left of the 640 × 480 scanout during both the boot logos and `MOVIES/EP01/EP01G.xmv`.

The display scales the composed picture uniformly and fills unused side space with a dim, blurred copy of the shot. At 640 × 480, sampled pixels from `OPENING`, `EP01A` and `EP06C` matched the original YUY2-to-RGB conversion exactly. Before deblocking was added, samples from the same frames at 1920 × 1080 and 2560 × 1080 matched an independent clamped Catmull–Rom calculation to within one 8-bit color level. On the Ryzen 5 5600X and RTX 5060 Ti, the Windows release presented all 100 WBPH overlays at 29.97 FPS in the 1138 × 480 movie benchmark with V-sync off.

Deblocking runs on the decoded overlay before the cubic filter when a full-frame movie is enlarged. It checks both sides of 8-pixel block boundaries, tapers the correction over two pixels on each side, and reduces it near strong edges or textured areas. Chroma corrections apply on the 16-pixel grid. At native size the three test frames were byte-identical to the previous release.

At 2 seconds in each movie, the Windows renderer enlarged the same decoded frame to 1920 × 1440 for both versions. Measurements sampled the original pixel centers, compared horizontal and vertical seams inside the picture, and used the unfiltered frame to select edges. For quiet seams with a brightness step of 2–12 levels and adjacent steps of at most 3, the mean step fell from 3.63 to 1.32 levels in `OPENING`, 3.48 to 1.38 in `EP01A`, and 4.14 to 1.81 in `EP06C`. For steps of at least 32 levels, average contrast changed by less than 0.3% in each frame. On the same Windows hardware, a 21:9 run of `EP01G` presented all 100 measured movie frames at 1138 × 480.

### Library entrance railing

The conversation at the library door is `MOVIES/EP01/EP01B.xmv`. The railing behind Daphne shifts in the decoded disc frames 2442–2451 and 2502–2511. A detail behind Scooby shifts diagonally in frames 2642–2651. Frame numbers start at zero. Matching 12 × 12 luma patches at source coordinates (603, 274) and (546, 308) found a one-pixel displacement followed by a return to the original position. In 447 consecutive end-of-movie samples, a 424 × 240 region of the recompiled Xbox decoder output closely matched an independent disc decode. The median frame's mean absolute difference was 0.006 on the 8-bit luma scale. The displacement was present before presentation.

For enlarged movies the player retains one source-resolution YUY2 picture. After a block has been still for four pictures, it checks for a small local shift against that reference. The comparison includes half-pixel positions and checks surrounding blocks for stationary scenery. Compatible tiles between agreeing neighbors can also retain their previous pixels. A hold expires after at most 12 pictures. A new movie, a change in overlay dimensions, or leaving enlarged movie playback clears the reference. No future frame is required. In the two tracked patches above, mean luma deviation from the picture just before the shift fell from 6.83 and 2.18 levels respectively to zero throughout the ten affected pictures. At 2560 × 1080, the tracked railing samples also stayed fixed in the presented picture for all three intervals. A global one-pixel pan and a hard cut both passed through in a separate check. In a 21:9 Windows release run of EP01B at 1138 × 480, all 100 measured overlays were presented at 29.97 FPS.

The movie shader also reduces isolated low-contrast peaks before scaling. At 2560 × 1080, five frames each from the opening, Daphne's shot and Scooby's shot were compared with the previous release. Among faint isolated brightness peaks with similar neighbors, mean excess fell from 1.30–1.34 to 0.28–0.33 levels. Mean contrast at measured horizontal edges of at least 32 levels changed by at most 0.1 level in each sample set.

## First-use pauses in gameplay

The Windows player used to call the HLSL compiler on the game thread when it first saw an Xbox vertex program. Starting with an empty shader cache produced 13 new bytecode files on the route from the menu through the library doors to the first blue ghost. Repeating the same route with the cache populated removed most of the delay at the same frames.

The build now runs the verified title's material assembler at `0x000B7720` with 64 combinations of skin weights, lights and material flags used by the draw path at `0x000B912A`. It includes the three SDK transforms selected at `0x00118771–0x0011878F`. All 13 previous cache files matched embedded material bytecode byte for byte. Other supported Xbox programs supply instruction data to a general transform shader that was also compiled during the build. Settings and the FPS overlay use embedded shaders. The Windows player has no import from D3DCompiler.

| Frame on the scripted route | Previous release, empty cache | Previous release, populated cache | Fixed release, empty cache |
| ---: | ---: | ---: | ---: |
| 221 | 319.55 ms | 33.34 ms | 33.33 ms |
| 402 | 894.73 ms | 33.35 ms | 33.34 ms |
| 681 | 1547.77 ms | 51.93 ms | 23.99 ms |
| 1288 | 404.52 ms | 33.34 ms | 33.32 ms |
| 1663 | 568.69 ms | 33.33 ms | 33.33 ms |

Each run used a fresh profile, the Ghost route from `tools/Benchmark.ps1`, a 30 FPS cap, VSync off, muted audio, a hidden 854 × 480 widescreen window and 640 × 480 native resolution on a Ryzen 5 5600X and GeForce RTX 5060 Ti. Of 2,032 displayed frames, 2,029 contributed to the timing summary. The median was 33.33 ms in all three runs. The 99th percentile was 50.07 ms, 33.69 ms and 33.58 ms respectively. From frame 1,100 onward, the previous cold run had three intervals over 100 ms. The longest of the same 931 measured intervals in the fixed run was 33.73 ms. The fixed run compiled zero graphics shaders, used embedded material shaders for every uploaded program on the route and created no shader cache. An offscreen WARP comparison of the general and specialized transforms matched all 2,304 pixels of a colored triangle and all 768 constant components after a transform launch, including a case with infinite intermediate values.

The separate Xbox audio effects processor previously spent 44 and 47 ms compiling blocks when the title installed its effects on this route. Both disc images are now prepared at player startup. Subsequent downloads compare the decoded image, control program and scratch size and restore the original processor state under the audio lock. The player compiled 523 DSP blocks at startup and zero during gameplay on this run. Across 33 downloads of each of `STREAM/DSP/bidon.bin` and `STREAM/DSP/simple.bin`, all 337,920 stereo output samples per image matched the independent reference. The game still reads levels, sounds and movies from the disc as needed. Frames 43 and 633 took 2.90 s and 0.97 s in the fixed run. They executed 52.4 million and 16.1 million guest checkpoints respectively, with zero engine updates or draws.

A separate uncapped run at the first ghost produced 480 game updates and 480 presentations in 2.36 seconds (203.5 FPS). Its median frame time was 4.85 ms and its 99th percentile was 6.26 ms. The local `build/stutter-investigation/` directory holds the per-frame CSVs and session summaries in `before`, `before-warm`, `verified` and `uncapped-indexed`.
