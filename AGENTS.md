# Scooby-Doo! Mystery Mayhem recompilation

Follow `DESIGN.txt` for project requirements. Target Windows x64, Linux x86-64,
and macOS on Intel and Apple Silicon.

1. **Performance.** Target sustained 360 FPS or higher at 1x native render
   resolution in widescreen with VSync off (2.778 ms per frame). Record reference
   hardware, render dimensions, and repeatable menu, exploration, ghost capture,
   and minigame scenes. Measure CPU and GPU time, median and 99th-percentile
   frame times, loading times, and peak memory. Count rendered frames separately
   from game updates and preserve the original gameplay timing.
   Keep expensive work out of gameplay and optimize measured bottlenecks.

2. **Recompilation.** Use C++23, C where useful, HLSL, and CMake. Generation,
   builds, gameplay, and diagnostics must work without Python. Verify
   `default.xbe` from the supplied XISO against `knowledge/supported_target.json`
   before generation. Compile ahead of time with verified call targets. Preserve
   Xbox addresses, layouts, calling conventions, and CPU behavior. Resolve
   unknowns from executable addresses and bytes. Bind Xbox APIs to native
   services, preserve their effects and errors, and identify those bindings in
   analysis reports.

3. **Accuracy.** Tie research in `knowledge/` to the verified executable and
   named disc resources. Use original Xbox captures with scene, timestamp, and
   known display settings. Establish gameplay rules from executable and asset
   evidence. Cover character switching, puzzles, Tome of Doom captures,
   minigames, cutscenes, save/reload, and the disc's language modes.

4. **Offline diagnostics.** Share the gameplay core with a native command-line
   tool for code and asset inspection, isolated CPU and service checks, offscreen
   D3D11 replay, image comparison, audio inspection, and performance measurement.
   Support focused cases before the first boot. Return structured results and
   failure exit codes. Tie failures to addresses, draws, or assets. Keep tracing
   opt-in and mute automated runs while preserving audio processing and timing.

5. **Simplicity.** Keep two native executables, the game with settings and the
   recompilation and diagnostics tool. Keep gameplay in one process, minimize
   dependencies, and bound caches. Fix root causes and reuse existing checks.
   Create tests only when necessary. Create Markdown files only for research
   or when explicitly requested.

6. **Rendering and platform services.** Use SDL3 for windows, events, controllers,
   and audio. Share the D3D11/HLSL renderer across native Windows D3D11 and
   Linux DXVK Native over Vulkan. Validate native DXMT/Metal on Intel and Apple
   Silicon before adopting it. Linux and macOS must run without Wine. Verify
   Xbox floating-point behavior in the native Apple Silicon build.

7. **Player settings.** Use the Dear ImGui SDL3 and D3D11 backends. Provide disc
   validation, display mode, resolution, aspect ratio, VSync, audio, and
   controller remapping. Support keyboard and controller navigation, persistent
   settings, restore defaults, and clear errors. Put diagnostics in an
   advanced view.

Follow the implementation order in `DESIGN.txt`. Start with executable and asset
verification, followed by native generation, offline checks, and render replay.
Integrate and play on Windows first, then validate gameplay, visual parity, and
performance on Linux and macOS.

Batch related work and reuse indexed results. Use at most three workers and run
memory-intensive commands one at a time. Use bounded or streaming reads and
avoid whole-file Python or Capstone scans over large binaries or dumps. After a
heavy or timed-out command, check for surviving child processes and stop only
those created by that command.

Keep disc media read-only. Keep disc media, extracted assets, and
generated outputs out of version control. Use computer control and subagents
only with the user's permission.
