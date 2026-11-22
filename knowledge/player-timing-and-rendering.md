# Player timing and rendering

These measurements use the USA/Europe Xbox executable with SHA-256 `6B2451C01E2432BAB5E3AA0C9DFE2EB03F842C67F1417CBA158DD7A5C4AED75B`. The disc contains the English, French and German release.

## Timing evidence

The engine creates its frame interval at 0x00018B32. The normal interval is 1/30 second. The comparison at 0x00018EC2 gates the CPU wait at 0x00018EE1. Windowed play replaces that wait with host pacing at guest scanout. Each gameplay presentation displays a frame produced by the original draw commands.

The engine samples its own clock at 0x000183C0, caps a step at 50 milliseconds, smooths ten samples, and stores elapsed seconds per update at engine offset 0x290 (0x0001855F). The game loop passes this value to the scene and game logic. The engine counter increments at 0x00018EC9. Diagnostics record this counter, accumulated simulation time, guest flips, movie overlays and window presentations separately. For display caps below 20 Hz, the engine runs at a multiple of the selected cap and the player displays complete frames at the requested rate.

Three verified instructions at 0x00023CC7, 0x00023D8A and 0x00024098 push a fixed 0.033 seconds into effect and interface updates. One downstream consumer at 0x00076EBA subtracts this argument from an object's timer. At custom rates the generated code scales the authored step by 30 times the current engine delta. The original literal remains in use at the 30 Hz setting and in offline CPU checks. XMV overlays follow the guest 60 Hz vertical blank clock independently of the gameplay cap.

Holding the left stick to the right for one second in the library produced 30 updates at 30 Hz and 60 updates at 60 Hz. Their simulation clocks advanced 999.8 and 1000.0 milliseconds. The stationary camera was at (−158.649, 1.879, 1.543). After moving, its positions were (−156.579, 2.571, 0.288) at 30 Hz and (−156.633, 2.617, 0.320) at 60 Hz. The camera follows the characters, and the snapshots were taken two rendered frames after release, so the settling intervals differ by one thirtieth of a second. These measurements cover this movement in the library.

## Widescreen verification

The SDK creates its display color surfaces at 0x0011A0A8–0x0011A0E2. Each entry in the device's color-surface table points to an Xbox surface with a physical color address at offset 4. The player widens the final horizontal projection for draws to those surfaces and keeps shadow maps at their original shape. The RenderWare perspective frustum builder at 0x000A2990 reads its horizontal extent at 0x000A29F4, 0x000A29FA and 0x000A29FD. These extents widen for the scene cameras in the two slots processed by 0x00024060. Movie playback keeps the original 4:3 frame.

Stationary library captures at scanout 1162 had the same camera position and orientation in both aspect modes. The 854 × 480 capture showed more of the room on both sides. In its centered 640 × 480 region, the mean absolute 8-bit color differences against the 4:3 reference were 1.99 for the HUD portrait, 1.09 for the compass, 0.82 for Shaggy, and 0.51 for a region of scenery. These are native player comparisons against its 4:3 output.

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

On the same machine, launching the Windows release with an empty shader cache, a fresh profile, a hidden window and a one-scanout limit took 50.37 seconds. Repeating that command with the populated cache took 0.45 seconds. The build now compiles the 26 built-in HLSL shaders and embeds 1,248,752 bytes of shader data in the player. The same fresh-cache run took 0.63 seconds and created zero shader cache files. A repeat took 0.47 seconds. All 26 embedded shader blobs matched the bytes compiled on the original cold run. Guest-supplied vertex programs continue to use their separate source-keyed cache.

## Vertex inputs and indirect calls

The renderer previously bound 17 vertex-buffer slots for each streamed draw because constant attributes occupied the last slot. Moving constant attributes to slot zero lets each draw bind just its declared streams and the constants. Four paired runs of the 213-draw library replay alternated the order of the builds. The median of the four submission medians fell from 0.638 to 0.575 ms. Each run issued 54,528 draws and all eight final images were identical.

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
