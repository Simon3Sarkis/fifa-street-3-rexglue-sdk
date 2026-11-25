/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#if REX_PLATFORM_WIN32
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/native_rhi.h>
#include <rex/graphics/nvidia_app_profile.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(store_shaders, true, "GPU",
                    "Store shaders persistently and load them when loading games to avoid "
                    "runtime spikes and freezes when playing the game not for the first time.");

REXCVAR_DEFINE_BOOL(vblank_host_clock_pacing, false, "GPU",
                    "Pace guest vblank from an absolute host-clock schedule instead of repeatedly "
                    "polling the guest clock")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(vblank_precise_sleep_margin_us, 1000, "GPU",
                     "How early the vblank worker should wake before the target for precise wait")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vblank_wine_precise_sleep, false, "GPU",
                    "Under Wine / Proton: sleep the vblank wait with sub-millisecond precision and "
                    "use vblank_precise_sleep_margin_wine_us as the margin")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(vblank_precise_sleep_margin_wine_us, 250, "GPU",
                     "vblank_precise_sleep_margin_us when running under Wine / Proton, where host "
                     "sleeps are precise (the smaller of the two is used)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(vblank_precise_spin_us, 150, "GPU",
                     "Final vblank wait window that should busy-spin for tighter pacing")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vblank_from_display, false, "GPU",
                    "Windows: keep the guest vblank in step with the desktop compositor's refresh "
                    "(every Nth refresh, N = refresh / guest rate, phase steered just after it) "
                    "so each frame stays on screen the same number of refreshes. Timer otherwise "
                    "and under Wine / Proton");

REXCVAR_DEFINE_INT32(vblank_from_display_offset_us, 500, "GPU",
                     "vblank_from_display: where in the compositor's refresh the guest vblank goes, "
                     "in microseconds after its vblank")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(vblank_from_display_log, false, "GPU",
                    "Log the display lock's worst phase error every 120 frames");

REXCVAR_DEFINE_BOOL(vblank_no_vsync_use_present_limiter, false, "GPU",
                    "Use the host present limiter FPS as the guest no-vsync vblank cadence")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

namespace {

uint64_t TicksToUsInteger(uint64_t ticks, uint64_t frequency) {
  return frequency ? ticks * 1000000ull / frequency : 0;
}

uint64_t UsToTicksInteger(uint64_t us, uint64_t frequency) {
  return us * frequency / 1000000ull;
}

void PreciseWaitUntilHostTick(uint64_t target_host_tick, uint64_t host_frequency) {
  // Under Wine / Proton host sleeps are precise to tens of microseconds, so the window spent polling
  // before the target can be much smaller (the default 1 ms margin kept ~10% of a core busy on the
  // Steam Deck). Opt-in (vblank_wine_precise_sleep) until it has been measured to help.
  const bool wine = REXCVAR_GET(vblank_wine_precise_sleep) && rex::thread::IsRunningUnderWine();
  const int32_t margin_us = wine ? std::min(REXCVAR_GET(vblank_precise_sleep_margin_us),
                                            REXCVAR_GET(vblank_precise_sleep_margin_wine_us))
                                 : REXCVAR_GET(vblank_precise_sleep_margin_us);
  const uint64_t min_sleep_us = wine ? 50 : 1000;
  const uint64_t sleep_margin_ticks =
      UsToTicksInteger(uint64_t(std::max(0, margin_us)), host_frequency);
  const uint64_t spin_ticks = UsToTicksInteger(
      uint64_t(std::max(0, REXCVAR_GET(vblank_precise_spin_us))), host_frequency);

  while (true) {
    uint64_t now = chrono::Clock::QueryHostTickCount();
    if (now >= target_host_tick) {
      return;
    }
    uint64_t remaining_ticks = target_host_tick - now;
    if (remaining_ticks > sleep_margin_ticks && sleep_margin_ticks) {
      uint64_t sleep_ticks = remaining_ticks - sleep_margin_ticks;
      uint64_t sleep_us = TicksToUsInteger(sleep_ticks, host_frequency);
      if (sleep_us >= min_sleep_us) {
        if (wine) {
          rex::thread::SleepPrecise(std::chrono::microseconds(sleep_us));
        } else {
          rex::thread::Sleep(std::chrono::microseconds(sleep_us));
        }
      } else {
        rex::thread::MaybeYield();
      }
    } else if (remaining_ticks > spin_ticks && spin_ticks) {
      rex::thread::MaybeYield();
    } else {
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
  }
}

}  // namespace

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

#if REX_PLATFORM_WIN32
namespace {

// The desktop compositor's refresh grid: the time of its last vblank and the refresh period, both
// in QPC ticks (the host tick clock on Windows). A plain query - nothing waits on the display.
bool GetCompositorVblank(uint64_t& last_vblank, double& period) {
  DWM_TIMING_INFO info = {};
  info.cbSize = sizeof(info);
  if (FAILED(DwmGetCompositionTimingInfo(nullptr, &info)) || !info.qpcVBlank ||
      !info.qpcRefreshPeriod) {
    return false;
  }
  last_vblank = info.qpcVBlank;
  period = double(info.qpcRefreshPeriod);
  return true;
}

}  // namespace
#endif

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::SetupPresentation(ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }

  if (!provider_) {
    // Before the driver loads: make sure our NVIDIA application profile
    // requests maximum performance so load-screen lulls cannot park the GPU
    // in a low P-state for the rest of the session.
    ApplyNvidiaMaxPerformanceProfile();
    CreateProvider(true);
    if (!provider_) {
      REXGPU_ERROR("Unable to create graphics provider");
      return X_STATUS_UNSUCCESSFUL;
    }
    provider_supports_presentation_ = true;
  } else if (!provider_supports_presentation_) {
    // A prior SetupGuestGpu built a headless provider; backends like Vulkan
    // need swapchain support baked in at provider creation time.
    REXGPU_ERROR("SetupPresentation called after headless SetupGuestGpu; call order is reversed");
    return X_STATUS_UNSUCCESSFUL;
  }

  app_context_ = app_context;
  auto loss_cb = [this](bool is_responsible, bool statically_from_ui_thread) {
    OnHostGpuLossFromAnyThread(is_responsible);
  };
  if (app_context_) {
    // Presenter creation must happen on the UI thread.
    app_context_->CallInUIThreadSynchronous(
        [this, loss_cb]() { presenter_ = provider_->CreatePresenter(loss_cb); });
  } else {
    // Offscreen path (e.g. capturing guest output without a window).
    presenter_ = provider_->CreatePresenter(loss_cb);
  }

  if (!presenter_) {
    REXGPU_ERROR("Unable to create presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

X_STATUS GraphicsSystem::SetupGuestGpu(runtime::FunctionDispatcher* function_dispatcher,
                                       system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;

  // Headless path: no one set up presentation, so build a no-presentation
  // provider just for the command processor.
  if (!provider_) {
    CreateProvider(false);
    provider_supports_presentation_ = false;
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t host_tick_frequency = chrono::Clock::QueryHostTickFrequency();
        uint64_t vsync_interval_host_ticks =
            std::max(uint64_t(1), uint64_t(double(host_tick_frequency) / refresh_rate_hz));
        auto get_no_vsync_interval_ticks = [](uint64_t frequency) {
          double no_vsync_rate_hz = 1000.0;
          if (REXCVAR_GET(vblank_no_vsync_use_present_limiter) &&
              rex::cvar::Query<bool>("d3d12_present_frame_limiter")) {
            no_vsync_rate_hz =
                std::clamp(rex::cvar::Query<double>("d3d12_present_frame_limiter_fps"), 1.0,
                           1000.0);
          }
          return std::max(uint64_t(1), uint64_t(double(frequency) / no_vsync_rate_hz));
        };
        uint64_t no_vsync_interval_ticks = get_no_vsync_interval_ticks(guest_tick_frequency);
        uint64_t no_vsync_interval_host_ticks = get_no_vsync_interval_ticks(host_tick_frequency);
        uint64_t initial_interval_ticks =
            REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
        uint64_t next_frame_time = chrono::Clock::QueryGuestTickCount() + initial_interval_ticks;
        uint64_t next_frame_host_tick =
            chrono::Clock::QueryHostTickCount() +
            (REXCVAR_GET(vsync) ? vsync_interval_host_ticks : no_vsync_interval_host_ticks);
#if REX_PLATFORM_WIN32
        // vblank_from_display: the guest vblank stays on the precise host-clock schedule, but its
        // phase is steered (at most 0.25 ms a frame) to land just after a display vblank, and its
        // period is a whole number of the display's refreshes. Every guest frame is then swapped
        // early in a refresh and shown on the same refresh boundary, for exactly N refreshes.
        const bool display_lock_allowed = !rex::thread::IsRunningUnderWine();
        bool display_locked = false;
        uint32_t logged_divider = ~0u;
#endif
        while (vsync_worker_running_) {
#if REX_PLATFORM_WIN32
          uint64_t display_last = 0;
          double display_period = 0.0;
          uint32_t divider = 0;
          if (display_lock_allowed && REXCVAR_GET(vblank_from_display) && REXCVAR_GET(vsync) &&
              GetCompositorVblank(display_last, display_period)) {
            const double n = std::round(double(vsync_interval_host_ticks) / display_period);
            if (n >= 1.0 && n <= 8.0 &&
                std::abs(n * display_period - double(vsync_interval_host_ticks)) <
                    0.025 * double(vsync_interval_host_ticks)) {
              divider = uint32_t(n);
            }
          }
          if (divider != logged_divider) {
            logged_divider = divider;
            if (divider) {
              REXGPU_INFO("Guest vblank locked to the display: {:.3f} Hz, every {} refresh(es)",
                          double(host_tick_frequency) / display_period, divider);
            } else if (REXCVAR_GET(vblank_from_display)) {
              REXGPU_INFO("Guest vblank on the timer (display not usable as the reference)");
            }
          }
          if (divider) {
            const double interval = display_period * divider;
            const uint64_t now = chrono::Clock::QueryHostTickCount();
            if (!display_locked || next_frame_host_tick + uint64_t(interval) < now) {
              next_frame_host_tick = now + uint64_t(interval);
              display_locked = true;
            }
            // Phase error of the scheduled vblank against "0.5 ms after a display vblank".
            const double target_offset =
                double(host_tick_frequency) * 1e-6 * double(REXCVAR_GET(vblank_from_display_offset_us));
            double rel = std::fmod(
                double(int64_t(next_frame_host_tick - display_last)) - target_offset, display_period);
            if (rel < 0.0) {
              rel += display_period;
            }
            if (rel > display_period * 0.5) {
              rel -= display_period;
            }
            if (REXCVAR_GET(vblank_from_display_log)) {
              static double max_abs_rel = 0.0;
              static uint32_t frames = 0;
              max_abs_rel = std::max(max_abs_rel, std::abs(rel));
              if (++frames >= 120) {
                REXGPU_INFO("[display lock] worst phase error over 120 frames: {:.3f} ms",
                            max_abs_rel * 1000.0 / double(host_tick_frequency));
                max_abs_rel = 0.0;
                frames = 0;
              }
            }
            const double max_step = double(host_tick_frequency) * 0.00025;
            next_frame_host_tick -= int64_t(std::clamp(rel, -max_step, max_step));
            PreciseWaitUntilHostTick(next_frame_host_tick, host_tick_frequency);
            MarkVblank();
            next_frame_host_tick += uint64_t(interval);
            next_frame_time = chrono::Clock::QueryGuestTickCount() + vsync_interval_ticks;
            continue;
          }
          display_locked = false;
#endif
          if (REXCVAR_GET(vblank_host_clock_pacing)) {
            bool vsync_enabled = REXCVAR_GET(vsync);
            no_vsync_interval_ticks = get_no_vsync_interval_ticks(guest_tick_frequency);
            no_vsync_interval_host_ticks = get_no_vsync_interval_ticks(host_tick_frequency);
            uint64_t interval_ticks =
                vsync_enabled ? vsync_interval_ticks : no_vsync_interval_ticks;
            uint64_t interval_host_ticks =
                vsync_enabled ? vsync_interval_host_ticks : no_vsync_interval_host_ticks;

            if (vsync_enabled) {
              PreciseWaitUntilHostTick(next_frame_host_tick, host_tick_frequency);
            } else {
              rex::thread::Sleep(std::chrono::milliseconds(1));
            }

            uint64_t current_host_tick = chrono::Clock::QueryHostTickCount();
            if (current_host_tick >= next_frame_host_tick) {
              constexpr uint32_t kMaxVblanksPerWake = 3;
              uint32_t vblanks_sent = 0;
              uint64_t current_guest_tick = chrono::Clock::QueryGuestTickCount();
              do {
                MarkVblank();
                next_frame_time += interval_ticks;
                next_frame_host_tick += interval_host_ticks;
                ++vblanks_sent;
              } while (vsync_worker_running_ && vblanks_sent < kMaxVblanksPerWake &&
                       current_host_tick >= next_frame_host_tick);

              if (current_host_tick >= next_frame_host_tick) {
                next_frame_time = current_guest_tick + interval_ticks;
                next_frame_host_tick = current_host_tick + interval_host_ticks;
              }
            }
            continue;
          }

          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          bool vsync_enabled = REXCVAR_GET(vsync);
          no_vsync_interval_ticks = get_no_vsync_interval_ticks(guest_tick_frequency);
          uint64_t interval_ticks = vsync_enabled ? vsync_interval_ticks : no_vsync_interval_ticks;

          if (current_time >= next_frame_time) {
            constexpr uint32_t kMaxVblanksPerWake = 3;
            uint32_t vblanks_sent = 0;
            do {
              MarkVblank();
              next_frame_time += interval_ticks;
              ++vblanks_sent;
            } while (vsync_worker_running_ && vblanks_sent < kMaxVblanksPerWake &&
                     current_time >= next_frame_time);

            if (current_time >= next_frame_time) {
              next_frame_time = current_time + interval_ticks;
            }
          }

          current_time = chrono::Clock::QueryGuestTickCount();
          if (!vsync_enabled) {
            rex::thread::Sleep(std::chrono::milliseconds(1));
          } else if (next_frame_time > current_time) {
            uint64_t sleep_ticks = next_frame_time - current_time;
            uint64_t sleep_us = sleep_ticks * 1000000 / guest_tick_frequency;
            if (sleep_us > 2000) {
              rex::thread::Sleep(std::chrono::microseconds(sleep_us - 1000));
            } else {
              rex::thread::MaybeYield();
            }
          } else {
            rex::thread::MaybeYield();
          }
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  shutting_down_.store(true, std::memory_order_release);

  if (command_processor_) {
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_DEBUG("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (shutting_down_.load(std::memory_order_acquire) || !interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  last_vblank_steady_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count(),
                               std::memory_order_relaxed);
  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  // Native-render RHI shader bytecode cache: the D3D12 backend runtime-
  // compiles its HLSL, and the largest scene shader costs seconds on the
  // render thread at first-frame pipeline creation; cached bytecode turns
  // every launch after the first into a file read. The host reaches the plugin
  // through IGraphicsSystem only, so the directory is derived here, before the
  // guest module starts drawing.
  if (!cache_root.empty()) {
    nrhi::SetShaderBytecodeCacheDirectory((cache_root / "nrhi_shaders").string().c_str());
  }
  if (!REXCVAR_GET(store_shaders)) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
