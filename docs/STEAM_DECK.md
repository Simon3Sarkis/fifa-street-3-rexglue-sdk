# Steam Deck and native Linux: what this fork changes, and what it measured

This fork carries the SDK changes made while bringing a FIFA Street 3 recomp to a locked 60 FPS on the
Steam Deck (1280x720), first under Proton and then as a **native Linux build** (no Wine). Most of them
are general: any ReXGlue project running on the Deck or on Linux can use them. Every number below is a
measurement, not an estimate; how each was measured is at the end.

## Results

| Steam Deck, full matches with play                       | FPS          | notes |
|----------------------------------------------------------|--------------|-------|
| Proton, before the fixes (paused-scene benchmark)        | 49.5 +- 15   | bimodal: half the runs locked at 30.0 |
| Proton, after the fixes (paused-scene benchmark)         | 59.6 +- 0.6  | CPU 3.3 GHz, APU 13-14 W |
| Proton, full match                                       | 52.2         | |
| **Native Linux**, full match (same session, interleaved) | **59.9**     | paired +7.7 +- 0.6 over Proton; 60 held in 97-100% of 2 s windows |
| Native, heaviest arena                                   | 59.2         | 31.2 under Proton |
| Native, second heaviest arena                            | 59.8         | 45.5 under Proton |
| Native at a **10 W** TDP                                 | 59.6         | ~9.2 W APU; 99.7-99.9% of frames on time with the command thread on its own core (2-12 missed per ~3600) |

Audio, native: from ~100 silent 5 ms frames a minute (audible crackle) and clipping on a docked Deck to
0 gaps, 0 clipping, exactly real time.

## The changes, and what each one did

### Native Linux

- **Write-watch faults parsed `/proc/self/maps` (the big one).** Every guest write to a GPU-watched
  page ended in `MMIOHandler::ExceptionCallback -> QueryProtect -> getline(/proc/self/maps)`: the
  render thread spent a third of its time parsing the maps file. `memory_posix.cpp` now keeps an
  interval map of the protections the memory layer itself sets (alloc, free, protect, file views) and
  answers `QueryProtect` from it, with `/proc/self/maps` only as a fallback.
  **Native paused scene 30 -> 59.8 FPS; main draws 17.7 -> 7.1 ms a frame.**
- **POSIX multi-object waits spun.** The poll sleep in `WaitMultiple` was truncated to whole
  milliseconds, so inside 1 ms alertable slices it was always `sleep_for(0)`: the audio worker sat at
  ~70% of a core. Sub-millisecond remainders are kept and alertable timeouts round up
  (`std::chrono::ceil`). Audio stays exactly real time.
- **UI paints unthrottled off Windows** (no vblank tick source): the UI thread re-presented flat out,
  ~52% of a core. `ui_tick_rate_hz` (default 60) spaces UI paints.
- `IsRunningUnderWine()` / `SleepPrecise()` in `rex/thread.h` (nanosleep on Linux, high-resolution
  waitable timers on Windows).

### Steam Deck CPU and GPU emulation (Proton and native)

- **CPU clock trap.** The Deck's CPU (amd-pstate-epp, balance_performance) clocks each core by its own
  activity. Once a frame slips to 30 FPS every thread idles half the time, the clocks drop to
  ~2.35 GHz and the game stays at 30. Letting the GPU command thread poll through its per-frame
  `WAIT_REG_MEM` wait keeps the clock up: `wait_reg_mem_spin_wine_us` (applies under Wine and on
  Linux; the native Windows PC keeps the short spin - on NVIDIA the long spin made `vkQueueSubmit`
  block). **6 of 6 runs at 57-60 FPS instead of half at 30.**
- **`gpu_command_thread_cpu`** (Linux): pins the command thread to one CPU and keeps the game's other
  threads off that core (re-applied every 5 s). At 10 W the missed frames were the command thread's
  own work taking twice as long (preempted or sharing its core); **missed frames per match 10/11/9 ->
  5/2/2.**
- **`vulkan_memexport_barrier_elision`**: tracks the exact ranges GPU skinning (memexport) reads and
  writes and only barriers on a real hazard, instead of ending the render pass around every skinning
  draw. **Render passes per frame 182 -> 82, pixel-identical.**
- **`input_device_refresh_ms`**: enumerate controllers at most this often instead of on every input
  call. Under Proton each enumeration is a blocking device I/O round trip: **the game's main thread
  was 65% blocked in `NtDeviceIoControlFile`.** Pad state is still read fresh on every call.
- **`dxgi_ui_tick_timer`**: Proton emulates `WaitForVBlank` by polling (~40% of a core for the UI
  tick thread); a high-resolution timer at the display rate costs ~5%.
- `vulkan_direct_resolve_fast32`, `vulkan_deferred_resolve_clears`,
  `vulkan_reuse_transient_texture_descriptors`: smaller resolve and descriptor cuts, each
  pixel-identical.
- A known-register bitset instead of `GetRegisterInfo`'s switch on every register write.

### Audio

- **`audio_force_stereo`**: fold 5.1 to stereo in the SDK (centre/surrounds -3 dB, LFE dropped,
  normalised). A docked Deck's stereo HDMI sink reports 6 channels to SDL; passing 5.1 through left the
  downmix to PipeWire: **~5 dB hotter, clipping, bass-heavy**.
- **`audio_device_sample_frames = 256`** (one guest audio frame, 5.3 ms): natively PipeWire otherwise
  ran the stream at a 1024 quantum, each callback drained four guest frames at once and the XMA decoder
  (polling every 2 ms) fell behind inside the burst. **~100 silent frames a minute -> 0**, 0 xruns;
  also 0 on Windows.
- `audio_stats` counts `guest_zero_frames` (frames the game itself produced silent).

### Also in this fork

- A pre-runtime **launcher** (`rex::ui::LauncherDialog`): pick the disc image, dropdowns for display,
  screen mode (windowed / borderless / exclusive with a refresh rate), vsync, game resolution and
  render scale. Mouse, keyboard and controller.
- **Play straight from an `.iso`**: a `game_data_root` that is a file is mounted as a disc image
  (`DiscImageDevice`), read-only like the console's disc.
- `fullscreen_mode` (`borderless` / `exclusive`) and `fullscreen_refresh_rate`.
- `draw_resolution_scaled_half_pixel_offset`: the SDK's default (half a *host* pixel) leaves the last
  host row/column of every predicated-tiling tile uncovered in games whose full-screen quads start on
  half pixels - a seam across the screen at 2x. Such games want it off.
- Diagnostics: `debug_log_frame_timing` (per-frame CPU waits, work counts, GPU busy time),
  per-submission GPU timestamps, an experimental compositor-locked vblank (`vblank_from_display`, off).

## What is game-specific (not for other projects as is)

The FIFA Street 3 project's own hooks (its yield and worker-thread fixes) and its tuned values (the
50 ms spin, pinning to CPU 0) live in that project. Another game should pick its values with its own
A/B runs.

## How it was measured

- **Interleaved A/B only.** On the Deck, batch-to-batch drift (2-3 FPS over an hour) is bigger than
  the spread inside a batch, so A and B alternate run by run and the paired difference is reported.
  Separate batches can't decide an A/B.
- Full matches with scripted play for results; a paused-match frozen scene for quick checks.
- 1:1 checks for GPU options: freeze a frame under the pause menu, snap, switch the option on, snap
  again, compare pixels. This test is blind to memexport (a paused pose never changes), so skinning
  changes were checked by counters and live-play snaps too.
- Thread CPU and per-core clocks sampled every 4 s; stack samples with `eu-stack` natively; per-frame
  timing from the frame timing log; audio from a recording of the output sink's monitor.
- Steam Deck on SteamOS 3.9 (RADV), 15 W and 10 W TDP; the PC checks on Windows with an RTX 4090.
