/**
 * @file        ui/overlay/launcher_overlay.cpp
 *
 * @brief       Pre-runtime launcher: game disc image, display settings dropdowns, Play.
 */

#include <rex/ui/overlay/launcher_overlay.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <mutex>

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>
#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_INT32(launcher_autoplay_ms, 0, "UI",
                     "Testing: press the launcher's Play by itself this many ms after it first "
                     "shows (0 = off)")
    .range(0, 600000);

REXCVAR_DEFINE_INT32(options_menu_focus_row, -1, "UI",
                     "Testing: the row of the open tab that has the focus when the menu opens "
                     "(-1 = the Play / Back to game button)")
    .range(-1, 16)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(options_menu_tab, 0, "UI",
                     "Options menu tab shown when it opens: 0 Display, 1 Graphics, 2 Audio, "
                     "3 Controls (follows the last tab used)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {

// Game resolution: the video mode the game is told about (720p or 1080p, both modes of the console
// game). Render scale: the GPU emulation draws every target that many times larger
// (draw_resolution_scale), so the picture is rendered at the product.
struct ResolutionPreset {
  const char* label;
  const char* tooltip;
  const char* resolution;  // the `resolution` cvar
  int width;
  int height;
};
constexpr ResolutionPreset kResolutions[] = {
    {"1280 x 720 (720p, original)", "The game's 720p mode, as on the console.", "720p", 1280, 720},
    {"1920 x 1080 (1080p)", "The game's own 1080p mode.", "1080p", 1920, 1080},
};
constexpr int kMaxRenderScale = 4;

const char* const kScreenModes[] = {"Windowed", "Borderless fullscreen", "Exclusive fullscreen"};

// Upscaling filter: how the game's picture is fitted to the window (present_effect). Bilinear is
// what the SDK always did, so it stays the default.
struct Upscaler {
  const char* label;
  const char* tooltip;
  const char* cvar;  // present_effect
};
constexpr Upscaler kUpscalers[] = {
    {"Bilinear (original)", "A plain stretch to the window, as before.", "bilinear"},
    {"AMD FSR 1", "Edge-aware upscaling (EASU) plus sharpening (RCAS). Best when the window is "
                  "larger than the render resolution.", "fsr"},
    {"AMD CAS", "Contrast-adaptive sharpening, with light upscaling.", "cas"},
};

// Sharpening for FSR 1 (RCAS reduction in stops, lower = sharper) and CAS (additional sharpness).
// Level 4 is FSR's own default (0.2), level 1 CAS's (0).
struct SharpnessLevel {
  const char* label;
  double fsr_reduction;
  double cas_additional;
};
constexpr SharpnessLevel kSharpness[] = {
    {"1 (softest)", 2.0, 0.0},
    {"2", 1.0, 0.25},
    {"3", 0.5, 0.5},
    {"4", 0.2, 0.75},
    {"5 (sharpest)", 0.0, 1.0},
};
constexpr int kFsrDefaultSharpness = 3;
constexpr int kCasDefaultSharpness = 0;

// Texture filtering: anisotropic_override (-1 = the game's own sampler setting).
struct AnisoLevel {
  const char* label;
  int value;
};
constexpr AnisoLevel kAniso[] = {
    {"Game default (original)", -1}, {"2x anisotropic", 2}, {"4x anisotropic", 3},
    {"8x anisotropic", 4},           {"16x anisotropic", 5},
};

const char* const kFxaa[] = {"none", "fxaa", "fxaa_extreme"};

const char* const kTabs[] = {"Display", "Graphics", "Audio", "Controls"};
constexpr int kTabCount = int(std::size(kTabs));

// The cvars LauncherDialog::ApplyLive sets; saved with the rest.
const std::vector<std::string_view> kLiveCvars = {
    "present_effect",       "present_fsr_sharpness_reduction", "present_cas_additional_sharpness",
    "anisotropic_override", "swap_post_effect",                "show_fps_counter",
    "show_fps_counter_detail"};

// Colours (0-1 RGBA), after FIFA Street 3's own menus: a parchment panel with ragged paint edges,
// dark ink, the selected row in a red pill. Drawn with shapes only (no game art).
constexpr ImVec4 kBackdropTop{0.17f, 0.12f, 0.08f, 1.0f};
constexpr ImVec4 kBackdropBottom{0.06f, 0.045f, 0.035f, 1.0f};
constexpr ImVec4 kPanelTop{0.91f, 0.86f, 0.73f, 1.0f};
constexpr ImVec4 kPanelBottom{0.83f, 0.76f, 0.61f, 1.0f};
constexpr ImVec4 kText{0.17f, 0.13f, 0.10f, 1.0f};
constexpr ImVec4 kTextDim{0.40f, 0.33f, 0.26f, 1.0f};
constexpr ImVec4 kTextLight{0.99f, 0.97f, 0.93f, 1.0f};
constexpr ImVec4 kAccent{0.79f, 0.11f, 0.11f, 1.0f};
constexpr ImVec4 kAccentHover{0.90f, 0.20f, 0.17f, 1.0f};
constexpr ImVec4 kGood{0.12f, 0.43f, 0.17f, 1.0f};
constexpr ImVec4 kBad{0.72f, 0.10f, 0.08f, 1.0f};
constexpr ImVec4 kField{0.36f, 0.26f, 0.16f, 0.14f};
constexpr ImVec4 kFieldHover{0.36f, 0.26f, 0.16f, 0.26f};
constexpr ImVec4 kInk{0.24f, 0.18f, 0.13f, 0.92f};  // dark tabs and the title outline

constexpr double kOpenSeconds = 0.20;
constexpr double kCloseSeconds = 0.14;

ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
  return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                a.w + (b.w - a.w) * t);
}

// A parchment panel with ragged paint edges, a few stains and splatter around it. Deterministic,
// so it doesn't flicker. Colours go through GetColorU32, so the open / close fade applies.
void DrawParchment(ImDrawList* dl, ImVec2 a, ImVec2 b, float s) {
  uint32_t seed = 0x2F6E2B1u;
  auto rnd = [&seed](float lo, float hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
  };
  const float h = std::max(1.0f, b.y - a.y);
  auto col_at = [&](float y, float alpha = 1.0f) {
    ImVec4 c = Lerp(kPanelTop, kPanelBottom, std::clamp((y - a.y) / h, 0.0f, 1.0f));
    c.w *= alpha;
    return ImGui::GetColorU32(c);
  };
  dl->PushClipRectFullScreen();
  // Soft shadow.
  for (int i = 0; i < 3; ++i) {
    const float g = (6.0f + 7.0f * i) * s;
    dl->AddRectFilled(ImVec2(a.x - g * 0.4f, a.y + g * 0.6f), ImVec2(b.x + g * 0.4f, b.y + g),
                      ImGui::GetColorU32(ImVec4(0, 0, 0, 0.10f)), 18.0f * s);
  }
  dl->AddRectFilledMultiColor(a, b, col_at(a.y), col_at(a.y), col_at(b.y), col_at(b.y));
  // Ragged edges: small, uneven dabs of paint along each side, now and then a bigger one.
  auto dab = [&]() { return (rnd(0.0f, 1.0f) < 0.15f ? rnd(7.0f, 11.0f) : rnd(2.0f, 6.0f)) * s; };
  const float step = 7.0f * s;
  for (float x = a.x; x <= b.x; x += step * rnd(0.5f, 1.4f)) {
    dl->AddCircleFilled(ImVec2(x, a.y + rnd(-1.0f, 2.0f) * s), dab(), col_at(a.y), 10);
    dl->AddCircleFilled(ImVec2(x, b.y + rnd(-2.0f, 1.0f) * s), dab(), col_at(b.y), 10);
  }
  for (float y = a.y; y <= b.y; y += step * rnd(0.5f, 1.4f)) {
    dl->AddCircleFilled(ImVec2(a.x + rnd(-1.0f, 2.0f) * s, y), dab(), col_at(y), 10);
    dl->AddCircleFilled(ImVec2(b.x + rnd(-2.0f, 1.0f) * s, y), dab(), col_at(y), 10);
  }
  // Splatter off the top-left and bottom-right corners, and a few drips.
  for (int i = 0; i < 14; ++i) {
    const bool tl = i < 8;
    const ImVec2 c = tl ? a : b;
    const float dx = rnd(10.0f, 70.0f) * s * (tl ? -1.0f : 1.0f);
    const float dy = rnd(-50.0f, 60.0f) * s * (tl ? -0.6f : 0.6f);
    const float y = c.y + dy;
    dl->AddCircleFilled(ImVec2(c.x + dx * 0.6f, y), rnd(2.5f, 11.0f) * s, col_at(y));
  }
  for (int i = 0; i < 5; ++i) {
    const float x = rnd(a.x + 40.0f * s, b.x - 40.0f * s);
    const float len = rnd(8.0f, 26.0f) * s;
    const float w = rnd(3.0f, 6.0f) * s;
    dl->AddRectFilled(ImVec2(x - w * 0.5f, b.y), ImVec2(x + w * 0.5f, b.y + len), col_at(b.y),
                      w * 0.5f);
    dl->AddCircleFilled(ImVec2(x, b.y + len), w * 0.8f, col_at(b.y));
  }
  // Aged paper: the edges a little darker than the middle.
  const float band = 34.0f * s;
  const ImU32 shade = ImGui::GetColorU32(ImVec4(0.50f, 0.37f, 0.22f, 0.16f));
  const ImU32 clear = ImGui::GetColorU32(ImVec4(0.50f, 0.37f, 0.22f, 0.0f));
  dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + band), shade, shade, clear, clear);
  dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - band), b, clear, clear, shade, shade);
  dl->AddRectFilledMultiColor(a, ImVec2(a.x + band, b.y), shade, clear, clear, shade);
  dl->AddRectFilledMultiColor(ImVec2(b.x - band, a.y), b, clear, shade, shade, clear);
  dl->PopClipRect();
}

// Text with a dark outline, like the game's menu titles.
void OutlinedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const char* text,
                  float thickness) {
  const ImU32 outline = ImGui::GetColorU32(kInk);
  for (int i = 0; i < 12; ++i) {
    const float a = float(i) * 6.2831853f / 12.0f;
    dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * thickness, pos.y + std::sin(a) * thickness),
                outline, text);
  }
  dl->AddText(font, size, pos, ImGui::GetColorU32(kTextLight), text);
}

std::string Upper(std::string text) {
  for (char& c : text) {
    c = char(std::toupper(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string FormatHz(float hz) {
  // 59.94 stays 59.94; 60.00 prints as 60.
  const float rounded = std::round(hz);
  if (std::abs(hz - rounded) < 0.02f) {
    return fmt::format("{} Hz", int(rounded));
  }
  return fmt::format("{:.2f} Hz", hz);
}

}  // namespace

// Shared with SDL's file dialog callback, which may run on another thread and after the dialog
// is gone.
struct LauncherDialog::PickerState {
  std::mutex mutex;
  bool open = false;
  bool done = false;
  std::filesystem::path result;
  std::string error;
};

LauncherDialog::LauncherDialog(ImGuiDrawer* drawer, Window* window, Options options,
                               CheckGameCallback check_game, PlayCallback play, QuitCallback quit)
    : ImGuiDialog(drawer),
      window_(window),
      options_(std::move(options)),
      check_game_(std::move(check_game)),
      play_(std::move(play)),
      quit_(std::move(quit)),
      picker_(std::make_shared<PickerState>()) {
  if (!options_.display_font_path.empty() && drawer) {
    display_font_ = drawer->LoadFontFile(options_.display_font_path);
  }
  displays_ = EnumerateDisplays();
  if (displays_.empty()) {
    displays_.push_back({"Default display", 0, 0, 0.0f, {}});
  }
  LoadFromSettings();
  SetGame(options_.game_path);
  if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    pad_opened_ = true;
  }
}

// The owner destroys the dialog (outside the UI drawer) once Play or Quit has run; Close() isn't
// used, because a closed ImGuiDialog deletes itself.
LauncherDialog::~LauncherDialog() {
  OnClose();
  if (pad_opened_) {
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
}

void LauncherDialog::OnClose() {
  if (first_frame_) {
    return;  // never drawn: nothing to restore
  }
  ImGuiIO& io = GetIO();
  io.ConfigFlags = saved_config_flags_;
  io.BackendFlags = saved_backend_flags_;
  first_frame_ = true;
}

void LauncherDialog::LoadFromSettings() {
  const int monitor = rex::cvar::Query<int32_t>("monitor");
  display_index_ = std::clamp(monitor > 0 ? monitor - 1 : 0, 0, int(displays_.size()) - 1);

  const bool fullscreen = rex::cvar::Query<bool>("fullscreen");
  const std::string mode = rex::cvar::Query<std::string>("fullscreen_mode");
  screen_mode_index_ = !fullscreen ? 0 : (mode == "exclusive" ? 2 : 1);

  refresh_index_ = 0;
  const double refresh = rex::cvar::Query<double>("fullscreen_refresh_rate");
  if (refresh > 0.0) {
    const auto& rates = displays_[display_index_].refresh_rates;
    for (size_t i = 0; i < rates.size(); ++i) {
      if (std::abs(rates[i] - float(refresh)) < 0.5f) {
        refresh_index_ = int(i) + 1;
      }
    }
  }

  vsync_index_ = rex::cvar::Query<bool>("vulkan_allow_present_mode_immediate") ? 0 : 1;

  const std::string resolution = rex::cvar::Query<std::string>("resolution");
  resolution_index_ = 0;
  for (size_t i = 0; i < std::size(kResolutions); ++i) {
    if (resolution == kResolutions[i].resolution) {
      resolution_index_ = int(i);
    }
  }
  scale_index_ =
      std::clamp(rex::cvar::Query<int32_t>("draw_resolution_scale_x"), 1, kMaxRenderScale) - 1;

  const std::string effect = rex::cvar::Query<std::string>("present_effect");
  upscale_index_ = 0;
  for (size_t i = 0; i < std::size(kUpscalers); ++i) {
    if (effect == kUpscalers[i].cvar) {
      upscale_index_ = int(i);
    }
  }
  // The level nearest to the active filter's setting.
  const bool cas = std::string_view(kUpscalers[upscale_index_].cvar) == "cas";
  const double sharp = cas ? rex::cvar::Query<double>("present_cas_additional_sharpness")
                           : rex::cvar::Query<double>("present_fsr_sharpness_reduction");
  sharpness_index_ = cas ? kCasDefaultSharpness : kFsrDefaultSharpness;
  double best = 1e9;
  for (size_t i = 0; i < std::size(kSharpness); ++i) {
    const double d =
        std::abs((cas ? kSharpness[i].cas_additional : kSharpness[i].fsr_reduction) - sharp);
    if (d < best - 1e-6) {
      best = d;
      sharpness_index_ = int(i);
    }
  }

  const int aniso = rex::cvar::Query<int32_t>("anisotropic_override");
  aniso_index_ = 0;
  for (size_t i = 0; i < std::size(kAniso); ++i) {
    if (aniso == kAniso[i].value) {
      aniso_index_ = int(i);
    }
  }

  const std::string fxaa = rex::cvar::Query<std::string>("swap_post_effect");
  fxaa_index_ = 0;
  for (size_t i = 0; i < std::size(kFxaa); ++i) {
    if (fxaa == kFxaa[i]) {
      fxaa_index_ = int(i);
    }
  }

  fps_index_ = !rex::cvar::Query<bool>("show_fps_counter")
                   ? 0
                   : 1 + std::clamp(rex::cvar::Query<int32_t>("show_fps_counter_detail"), 0, 2);

  tab_ = std::clamp(rex::cvar::Query<int32_t>("options_menu_tab"), 0, kTabCount - 1);

  if (!options_.update_check_cvar.empty()) {
    update_check_index_ = rex::cvar::Query<bool>(options_.update_check_cvar) ? 1 : 0;
  }
}

void LauncherDialog::ApplyLive() {
  const auto& level = kSharpness[std::clamp(sharpness_index_, 0, int(std::size(kSharpness)) - 1)];
  const std::string_view effect = kUpscalers[upscale_index_].cvar;
  // Sharpening first, so the filter switch below already paints with it. Only the active
  // filter's value changes; the other keeps its own setting.
  if (effect == "fsr") {
    rex::cvar::SetFlagByName("present_fsr_sharpness_reduction",
                             fmt::format("{}", level.fsr_reduction));
  } else if (effect == "cas") {
    rex::cvar::SetFlagByName("present_cas_additional_sharpness",
                             fmt::format("{}", level.cas_additional));
  }
  rex::cvar::SetFlagByName("present_effect", effect);
  rex::cvar::SetFlagByName("anisotropic_override", std::to_string(kAniso[aniso_index_].value));
  rex::cvar::SetFlagByName("swap_post_effect", kFxaa[fxaa_index_]);
  if (fps_index_ > 0) {
    rex::cvar::SetFlagByName("show_fps_counter_detail", std::to_string(fps_index_ - 1));
  }
  rex::cvar::SetFlagByName("show_fps_counter", fps_index_ > 0 ? "true" : "false");
}

void LauncherDialog::SetGame(const std::filesystem::path& path) {
  game_path_ = path;
  if (path.empty()) {
    game_check_ = {false, options_.picker_hint};
  } else if (check_game_) {
    game_check_ = check_game_(path);
  } else {
    game_check_ = {std::filesystem::exists(path), ""};
  }
}

void LauncherDialog::OpenPicker() {
  {
    std::lock_guard lock(picker_->mutex);
    if (picker_->open) {
      return;
    }
    picker_->open = true;
    picker_->done = false;
    picker_->error.clear();
  }
  static const SDL_DialogFileFilter kFilters[] = {
      {"Xbox 360 disc image (*.iso)", "iso"},
      {"All files", "*"},
  };
  auto* sdl_window = dynamic_cast<WindowSDL*>(window_);
  // SDL owns a copy of nothing: the userdata must outlive the dialog, hence a heap shared_ptr.
  auto* holder = new std::shared_ptr<PickerState>(picker_);
  std::string start;
  if (!game_path_.empty()) {
    start = (std::filesystem::is_directory(game_path_) ? game_path_ : game_path_.parent_path())
                .string();
  }
  SDL_ShowOpenFileDialog(
      [](void* userdata, const char* const* files, int) {
        auto* state_holder = static_cast<std::shared_ptr<PickerState>*>(userdata);
        auto& state = **state_holder;
        {
          std::lock_guard lock(state.mutex);
          if (!files) {
            state.error = SDL_GetError();
          } else if (files[0]) {
            state.result = std::filesystem::u8path(files[0]);
          }
          state.open = false;
          state.done = true;
        }
        delete state_holder;
      },
      holder, sdl_window ? sdl_window->sdl_window() : nullptr, kFilters, int(std::size(kFilters)),
      start.empty() ? nullptr : start.c_str(), false);
}

void LauncherDialog::PollPicker() {
  std::filesystem::path picked;
  std::string error;
  {
    std::lock_guard lock(picker_->mutex);
    if (!picker_->done) {
      return;
    }
    picker_->done = false;
    picked = std::move(picker_->result);
    picker_->result.clear();
    error = std::move(picker_->error);
  }
  // The modal picker swallows the release of whatever activated it.
  ImGuiIO& io = GetIO();
  io.AddMouseButtonEvent(0, false);
  io.AddKeyEvent(ImGuiKey_Enter, false);
  io.AddKeyEvent(ImGuiKey_Space, false);
  if (!picked.empty()) {
    SetGame(picked);
  } else if (!error.empty()) {
    REXLOG_WARN("Launcher: file picker unavailable: {}", error);
    game_check_ = {false,
                   "The file picker isn't available here. Put the .iso next to the game and "
                   "start it again."};
  }
}

void LauncherDialog::ApplyDisplay() {
  std::string monitor = std::to_string(display_index_ + 1);
  std::string mode = screen_mode_index_ == 2 ? "exclusive" : "borderless";
  std::string refresh = "0";
  const auto& rates = displays_[display_index_].refresh_rates;
  if (screen_mode_index_ == 2 && refresh_index_ > 0 && refresh_index_ <= int(rates.size())) {
    refresh = fmt::format("{:.3f}", rates[refresh_index_ - 1]);
  }
  std::string fullscreen = screen_mode_index_ != 0 ? "true" : "false";
  auto apply = [monitor, mode, refresh, fullscreen, window = window_]() {
    rex::cvar::SetFlagByName("monitor", monitor);
    rex::cvar::SetFlagByName("fullscreen_mode", mode);
    rex::cvar::SetFlagByName("fullscreen_refresh_rate", refresh);
    // The fullscreen change callback resizes the window.
    rex::cvar::SetFlagByName("fullscreen", fullscreen);
    if (window) {
      window->ApplyDisplaySettings();
    }
  };
  // Not from inside the UI drawer: moving and resizing the window reconnects the presenter.
  if (window_) {
    window_->app_context().CallInUIThreadDeferred(std::move(apply));
  } else {
    apply();
  }
}

// In-game: what the running game reads only at start goes straight to the settings file; the
// display settings (applied live already) are saved from their current values.
void LauncherDialog::SaveForNextStart() {
  if (options_.settings_path.empty()) {
    return;
  }
  std::vector<std::string_view> live = {"monitor", "fullscreen", "fullscreen_mode",
                                        "fullscreen_refresh_rate"};
  live.insert(live.end(), kLiveCvars.begin(), kLiveCvars.end());
  if (!options_.update_check_cvar.empty()) {
    live.push_back(options_.update_check_cvar);
  }
  rex::cvar::SaveConfigValues(options_.settings_path, live);
  toml::table table;
  try {
    if (std::filesystem::exists(options_.settings_path)) {
      table = toml::parse_file(options_.settings_path.string());
    }
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("Options: {} unreadable ({}), rewriting the settings it holds",
                options_.settings_path.string(), e.description());
  }
  const bool vsync = vsync_index_ == 1;
  const int scale = scale_index_ + 1;
  table.insert_or_assign("vulkan_allow_present_mode_immediate", !vsync);
  table.insert_or_assign("vulkan_allow_present_mode_mailbox", !vsync);
  table.insert_or_assign("resolution", std::string(kResolutions[resolution_index_].resolution));
  table.insert_or_assign("draw_resolution_scale_x", int64_t(scale));
  table.insert_or_assign("draw_resolution_scale_y", int64_t(scale));
  std::error_code ec;
  const auto temp = options_.settings_path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "# Auto-generated cvar configuration\n" << table << "\n";
    if (!out) {
      REXLOG_ERROR("Options: cannot write {}", temp);
      return;
    }
  }
  std::filesystem::rename(temp, options_.settings_path, ec);
  if (ec) {
    REXLOG_ERROR("Options: cannot replace {}: {}", options_.settings_path.string(), ec.message());
  }
}

void LauncherDialog::SaveAll() {
  if (options_.in_game) {
    SaveForNextStart();
    return;
  }
  const bool vsync = vsync_index_ == 1;
  const bool vsync_changed = rex::cvar::Query<bool>("vulkan_allow_present_mode_immediate") == vsync;
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", vsync ? "false" : "true");
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", vsync ? "false" : "true");
  const auto& preset = kResolutions[resolution_index_];
  rex::cvar::SetFlagByName("resolution", preset.resolution);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(scale_index_ + 1));
  rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(scale_index_ + 1));
  if (!game_path_.empty()) {
    rex::cvar::SetFlagByName("game_data_root", game_path_.string());
  }
  if (!options_.settings_path.empty()) {
    std::vector<std::string_view> names = {"monitor",
                                           "fullscreen",
                                           "fullscreen_mode",
                                           "fullscreen_refresh_rate",
                                           "vulkan_allow_present_mode_immediate",
                                           "vulkan_allow_present_mode_mailbox",
                                           "resolution",
                                           "draw_resolution_scale_x",
                                           "draw_resolution_scale_y"};
    if (!game_path_.empty()) {
      names.push_back("game_data_root");
    }
    names.insert(names.end(), kLiveCvars.begin(), kLiveCvars.end());
    if (!options_.update_check_cvar.empty()) {
      names.push_back(options_.update_check_cvar);
    }
    rex::cvar::SaveConfigValues(options_.settings_path, names);
  }
  if (vsync_changed && window_) {
    Window* window = window_;
    window->app_context().CallInUIThreadDeferred([window]() { window->ReconnectPresenter(); });
  }
}

void LauncherDialog::PollGamepad(ImGuiIO& io) {
  if (!pad_opened_) {
    return;
  }
  // The first connected pad drives the menu.
  SDL_Gamepad* pad = nullptr;
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    if (count > 0) {
      pad = SDL_GetGamepadFromID(ids[0]);
      if (!pad) {
        pad = SDL_OpenGamepad(ids[0]);
      }
    }
    SDL_free(ids);
  }
  if (!pad) {
    return;
  }
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  auto button = [&](ImGuiKey key, SDL_GamepadButton b) {
    io.AddKeyEvent(key, SDL_GetGamepadButton(pad, b));
  };
  button(ImGuiKey_GamepadFaceDown, SDL_GAMEPAD_BUTTON_SOUTH);
  button(ImGuiKey_GamepadFaceRight, SDL_GAMEPAD_BUTTON_EAST);
  button(ImGuiKey_GamepadFaceLeft, SDL_GAMEPAD_BUTTON_WEST);
  button(ImGuiKey_GamepadFaceUp, SDL_GAMEPAD_BUTTON_NORTH);
  button(ImGuiKey_GamepadDpadUp, SDL_GAMEPAD_BUTTON_DPAD_UP);
  button(ImGuiKey_GamepadDpadDown, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
  button(ImGuiKey_GamepadDpadLeft, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
  button(ImGuiKey_GamepadDpadRight, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
  button(ImGuiKey_GamepadStart, SDL_GAMEPAD_BUTTON_START);
  const bool b_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST);
  if (options_.in_game && b_down && !pad_b_was_down_ &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    close_requested_ = true;
  }
  pad_b_was_down_ = b_down;
  // Start + Back closes the in-game menu here too: while the game is paused it doesn't poll the
  // controller, so the input system's own chord detection is idle.
  const bool chord_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START) &&
                          SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
  if (options_.in_game && chord_down && !pad_chord_was_down_) {
    close_requested_ = true;
  }
  pad_chord_was_down_ = chord_down;
  // LB / RB switch tabs.
  const bool lb_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
  const bool rb_down = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
  if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    if (lb_down && !pad_lb_was_down_) {
      tab_request_ = (tab_ + kTabCount - 1) % kTabCount;
    } else if (rb_down && !pad_rb_was_down_) {
      tab_request_ = (tab_ + 1) % kTabCount;
    }
  }
  pad_lb_was_down_ = lb_down;
  pad_rb_was_down_ = rb_down;
  auto axis = [&](ImGuiKey negative, ImGuiKey positive, SDL_GamepadAxis a) {
    const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
    constexpr float kDead = 0.35f;
    const float n = v < -kDead ? std::min(1.0f, (-v - kDead) / (1.0f - kDead)) : 0.0f;
    const float p = v > kDead ? std::min(1.0f, (v - kDead) / (1.0f - kDead)) : 0.0f;
    io.AddKeyAnalogEvent(negative, n > 0.0f, n);
    io.AddKeyAnalogEvent(positive, p > 0.0f, p);
  };
  axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, SDL_GAMEPAD_AXIS_LEFTY);
  axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, SDL_GAMEPAD_AXIS_LEFTX);
}

bool LauncherDialog::DrawCombo(const char* id, int& index, const std::vector<Choice>& choices,
                               bool enabled, float width) {
  bool changed = false;
  index = std::clamp(index, 0, std::max(0, int(choices.size()) - 1));
  ImGui::SetNextItemWidth(width);
  ImGui::BeginDisabled(!enabled || choices.size() < 2);
  const char* preview = choices.empty() ? "" : choices[index].label.c_str();
  // The list gets its own, tighter padding (the panel's would leave a gap and a scrollbar).
  const float s = ImGui::GetStyle().FramePadding.y / 9.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 6.0f * s));
  if (focus_frames_ > 0 && current_row_ >= 0 &&
      current_row_ - first_tab_row_ == REXCVAR_GET(options_menu_focus_row)) {
    ImGui::SetKeyboardFocusHere();
  }
  const bool open = ImGui::BeginCombo(id, preview, ImGuiComboFlags_HeightLarge);
  ImGui::PopStyleVar();
  // The row's label shows the game's red pill while its field is hovered or focused.
  if (current_row_ >= 0 &&
      (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))) {
    hot_row_next_ = current_row_;
  }
  if (open) {
    for (int i = 0; i < int(choices.size()); ++i) {
      const bool selected = i == index;
      if (ImGui::Selectable(choices[i].label.c_str(), selected)) {
        changed = i != index;
        index = i;
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
      if (!choices[i].tooltip.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", choices[i].tooltip.c_str());
      }
    }
    ImGui::EndCombo();
  }
  ImGui::EndDisabled();
  return changed;
}

std::vector<LauncherDialog::Choice> LauncherDialog::RefreshChoices() const {
  const auto& display = displays_[display_index_];
  std::vector<Choice> choices;
  const std::string current =
      display.refresh_hz > 0.0f ? FormatHz(display.refresh_hz) : std::string("current");
  if (screen_mode_index_ != 2) {
    choices.push_back({fmt::format("Desktop ({})", current),
                       "Windowed and borderless use the desktop's refresh rate."});
    return choices;
  }
  choices.push_back({fmt::format("Current ({})", current), ""});
  for (float hz : display.refresh_rates) {
    choices.push_back({FormatHz(hz), ""});
  }
  return choices;
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
  if (first_frame_) {
    saved_config_flags_ = io.ConfigFlags;
    saved_backend_flags_ = io.BackendFlags;
  }
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
  PollGamepad(io);
  PollPicker();

  // Slide and fade in; on Play / Back / Quit, slide out first, then act.
  const double now = ImGui::GetTime();
  if (open_time_ < 0.0) {
    open_time_ = now;
  }
  if (pending_action_ && now - closing_since_ >= kCloseSeconds) {
    const int action = pending_action_;
    pending_action_ = 0;
    if (action == 1) {
      SaveAll();
      PlayCallback play_callback = play_;
      std::filesystem::path game = game_path_;
      if (play_callback) {
        play_callback(game);
      }
    } else if (action == 2 && quit_) {
      quit_();
    }
  }
  float anim = float(std::clamp((now - open_time_) / kOpenSeconds, 0.0, 1.0));
  anim = 1.0f - (1.0f - anim) * (1.0f - anim) * (1.0f - anim);  // ease out
  float slide = 1.0f - anim;
  if (closing_since_ >= 0.0) {
    const float c = float(std::clamp((now - closing_since_) / kCloseSeconds, 0.0, 1.0));
    anim = std::min(anim, 1.0f - c * c);
    slide = -c;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(anim, 0.0f));
  row_counter_ = 0;
  current_row_ = -1;

  const ImVec2 display = io.DisplaySize;
  // Laid out for 1080p; scaled with the window, kept readable on the Deck's 800p panel.
  const float s = std::clamp(display.y / 1080.0f, 0.75f, 3.0f);
  ImGuiDrawer* drawer = imgui_drawer();
  ImFont* font = drawer->ui_font();
  ImFont* font_semibold = drawer->ui_font_semibold() ? drawer->ui_font_semibold() : font;
  ImFont* font_bold = drawer->ui_font_bold() ? drawer->ui_font_bold() : font_semibold;
  // The condensed display face for headings, tabs, row labels and buttons (upper case, as the
  // game sets them); the bold UI font without it.
  ImFont* font_display = display_font_ ? display_font_ : font_bold;
  const bool condensed = display_font_ != nullptr;
  auto caps = [condensed](const char* text) {
    return condensed ? Upper(text) : std::string(text);
  };

  // Backdrop.
  ImDrawList* bg = ImGui::GetBackgroundDrawList();
  // The launcher covers the screen; in the game, the game stays visible behind a dim scrim.
  const float backdrop_alpha = options_.in_game ? 0.55f : 1.0f;
  ImVec4 top = kBackdropTop, bottom = kBackdropBottom;
  top.w = bottom.w = backdrop_alpha;
  bg->AddRectFilledMultiColor(ImVec2(0, 0), display, ImGui::GetColorU32(top),
                              ImGui::GetColorU32(top), ImGui::GetColorU32(bottom),
                              ImGui::GetColorU32(bottom));
  bg->AddRectFilled(ImVec2(0, 0), ImVec2(display.x, 4.0f * s), ImGui::GetColorU32(kAccent));

  const float panel_w = std::min(display.x - 32.0f * s, 860.0f * s);
  ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f - slide * 70.0f * s, display.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(panel_w, 0.0f), ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40.0f * s, 34.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 9.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f * s, 12.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.93f, 0.89f, 0.78f, 0.99f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.36f, 0.26f, 0.16f, 0.35f));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, kTextDim);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_Button, kField);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kFieldHover);
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.40f));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.55f));
  ImGui::PushStyleColor(ImGuiCol_NavCursor, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.36f, 0.26f, 0.16f, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(0.36f, 0.26f, 0.16f, 0.30f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.36f, 0.26f, 0.16f, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kAccent);
  ImGui::PushFont(font, 19.0f * s);

  ImGui::Begin("##launcher", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

  // The parchment, behind everything in the window (its size is last frame's).
  {
    const ImVec2 pos = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    if (size.y > 1.0f) {
      DrawParchment(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + size.x, pos.y + size.y), s);
    }
  }

  // Heading: big, outlined, upper case, like the game's "GAME PAUSED".
  {
    const std::string title = Upper(options_.title);
    const float size = (condensed ? 96.0f : 62.0f) * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 extent = font_display->CalcTextSizeA(size, FLT_MAX, 0.0f, title.c_str());
    // Over the panel's top-left edge, as the game sets its menu titles.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();
    const float lift = (condensed ? 74.0f : 56.0f) * s;
    OutlinedText(dl, font_display, size, ImVec2(at.x - 18.0f * s, at.y - lift), title.c_str(),
                 3.2f * s);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(extent.x, std::max(1.0f, extent.y - 54.0f * s)));
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(options_.subtitle.c_str());
  ImGui::PopStyleColor();
  // A newer release (launcher only): one line and a button that opens its page in the browser.
  if (!options_.in_game && options_.update_notice) {
    std::string version, url;
    {
      std::lock_guard lock(options_.update_notice->mutex);
      version = options_.update_notice->version;
      url = options_.update_notice->url;
    }
    if (!version.empty()) {
      ImGui::Dummy(ImVec2(0, 2.0f * s));
      ImGui::PushStyleColor(ImGuiCol_Text, kGood);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(fmt::format("Version {} is available.", version).c_str());
      ImGui::PopStyleColor();
      ImGui::SameLine();
      if (ImGui::SmallButton("Open the release page") && !url.empty()) {
        if (!SDL_OpenURL(url.c_str())) {
          REXLOG_WARN("Launcher: cannot open {}: {}", url, SDL_GetError());
        }
      }
    }
  }
  ImGui::Dummy(ImVec2(0, 8.0f * s));

  const float content_w = ImGui::GetContentRegionAvail().x;
  const float label_w = 290.0f * s;
  // SameLine / SetCursorPosX positions count from the window edge, padding included.
  const float field_x = ImGui::GetCursorStartPos().x + label_w;
  const float field_w = content_w - label_w;

  auto section = [&](const char* name) {
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImGui::PushFont(font_display, (condensed ? 20.0f : 14.0f) * s);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted(name);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Separator();
  };
  // In the game every row says when it takes effect: NOW (at once) or RESTART (the next time the
  // game starts). The launcher applies everything at Play, so it shows no tags.
  enum class Applies { kNow, kRestart, kNone };
  auto row_label = [&](const char* label, Applies applies = Applies::kNone) {
    const float row_y = ImGui::GetCursorPosY();
    current_row_ = row_counter_++;
    const bool hot = current_row_ == hot_row_;
    if (hot) {
      // The game's selection: a red pill with the (A) button glyph.
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      const float hgt = ImGui::GetFrameHeight();
      const float pill_w = (options_.in_game ? 184.0f : 270.0f) * s;
      dl->AddRectFilled(p0, ImVec2(p0.x + pill_w, p0.y + hgt), ImGui::GetColorU32(kAccent),
                        hgt * 0.5f);
      const ImVec2 c(p0.x + hgt * 0.5f, p0.y + hgt * 0.5f);
      dl->AddCircleFilled(c, 9.0f * s, ImGui::GetColorU32(kTextLight));
      const float gs = 13.0f * s;
      const ImVec2 gsz = font_bold->CalcTextSizeA(gs, FLT_MAX, 0.0f, "A");
      dl->AddText(font_bold, gs, ImVec2(c.x - gsz.x * 0.5f, c.y - gsz.y * 0.5f),
                  ImGui::GetColorU32(kAccent), "A");
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + hgt * 0.5f + 16.0f * s);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextLight);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(condensed ? font_display : font_semibold, condensed ? 27.0f * s : 0.0f);
    ImGui::TextUnformatted(caps(label).c_str());
    ImGui::PopFont();
    if (hot) {
      ImGui::PopStyleColor();
    }
    if (options_.in_game && applies != Applies::kNone) {
      ImGui::SameLine(ImGui::GetCursorStartPos().x + 192.0f * s);
      ImGui::PushFont(font_display, (condensed ? 17.0f : 12.0f) * s);
      const bool now = applies == Applies::kNow;
      ImGui::PushStyleColor(ImGuiCol_Text, hot ? kTextLight
                                           : now ? ImVec4(kGood.x, kGood.y, kGood.z, 0.9f)
                                                 : ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.9f));
      // Centred on the row's text (the tag font is smaller).
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetStyle().FramePadding.y + 1.0f * s);
      ImGui::TextUnformatted(now ? "NOW" : "RESTART");
      ImGui::PopStyleColor();
      ImGui::PopFont();
    }
    // From this window's own start: inside the tab body (a child with no padding) field_x, which
    // counts the panel's padding, would push the field past the edge and scroll the child.
    ImGui::SameLine(ImGui::GetCursorStartPos().x + label_w);
    ImGui::SetCursorPosY(row_y);  // the field lines up with the label, not the tag
  };
  auto note = [&](const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
    ImGui::PushFont(nullptr, 16.0f * s);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
  };

  // Game (the launcher only: the running game keeps its disc).
  if (!options_.in_game) {
  section("GAME");
  row_label("Disc image");
  {
    const float button_w = 170.0f * s;
    std::string shown = game_path_.empty() ? std::string("Not selected")
                                           : game_path_.filename().string();
    if (shown.empty()) {
      shown = game_path_.string();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, game_path_.empty() ? kTextDim : kText);
    ImGui::SetNextItemWidth(field_w - button_w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled();
    ImGui::InputText("##game", shown.data(), shown.size() + 1, ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (!game_path_.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("%s", game_path_.string().c_str());
    }
    ImGui::SameLine();
    bool picker_open;
    {
      std::lock_guard lock(picker_->mutex);
      picker_open = picker_->open;
    }
    ImGui::BeginDisabled(picker_open);
    if (ImGui::Button(picker_open ? "Selecting..." : "Select ISO...", ImVec2(button_w, 0))) {
      OpenPicker();
    }
    ImGui::EndDisabled();
  }
  ImGui::SetCursorPosX(field_x);
  ImGui::PushStyleColor(ImGuiCol_Text, game_check_.ok ? kGood : (game_path_.empty() ? kTextDim : kBad));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + field_w);
  ImGui::PushFont(nullptr, 16.0f * s);
  ImGui::TextUnformatted(game_check_.message.c_str());
  ImGui::PopFont();
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  }

  // Settings, one tab per area. The body has a fixed height so the panel doesn't jump between tabs.
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false)) {
    tab_request_ = (tab_ + kTabCount - 1) % kTabCount;
  } else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) {
    tab_request_ = (tab_ + 1) % kTabCount;
  }
  ImGui::Dummy(ImVec2(0, 4.0f * s));
  ImGui::PushStyleColor(ImGuiCol_Tab, kInk);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_TabSelected, kAccent);
  ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kAccent);
  ImGui::PushStyleColor(ImGuiCol_TabDimmed, kInk);
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, kAccent);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextLight);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(22.0f * s, 9.0f * s));
  ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 16.0f * s);
  ImGui::PushStyleVar(ImGuiStyleVar_TabBarBorderSize, 0.0f);
  ImGui::PushFont(font_display, (condensed ? 27.0f : 18.0f) * s);
  // The first frame opens on the remembered tab. A requested tab (first frame, LB / RB, PgUp /
  // PgDn) is shown at once and kept requested until ImGui reports it selected (it shows the first
  // tab on its first frame); otherwise tab_ follows ImGui (mouse clicks).
  const int request = first_frame_ ? tab_ : tab_request_;
  int imgui_selected = -1;
  if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyShrink)) {
    for (int t = 0; t < kTabCount; ++t) {
      const ImGuiTabItemFlags flags = request == t ? ImGuiTabItemFlags_SetSelected : 0;
      const std::string tab_label = Upper(kTabs[t]) + "###tab" + std::to_string(t);
      if (ImGui::BeginTabItem(tab_label.c_str(), nullptr, flags)) {
        imgui_selected = t;
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }
  const int shown = request >= 0 ? request : (imgui_selected >= 0 ? imgui_selected : tab_);
  tab_request_ = request >= 0 && imgui_selected != request ? request : -1;
  if (shown != tab_) {
    tab_ = shown;
    REXCVAR_SET(options_menu_tab, shown);
  }
  ImGui::PopFont();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(7);

  const float row_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::BeginChild("##tabbody", ImVec2(content_w, row_h * 7.0f + 8.0f * s),
                    ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
  ImGui::Dummy(ImVec2(0, 4.0f * s));
  first_tab_row_ = row_counter_;
  if (tab_ == 0) {
    // Display.
    std::vector<Choice> displays;
    for (size_t i = 0; i < displays_.size(); ++i) {
      const auto& d = displays_[i];
      std::string label = fmt::format("{}: {}", i + 1, d.name);
      if (d.width > 0) {
        label += fmt::format("  ({} x {})", d.width, d.height);
      }
      displays.push_back({std::move(label), ""});
    }
    row_label("Display", Applies::kNow);
    if (DrawCombo("##display", display_index_, displays, true, field_w)) {
      refresh_index_ = 0;
      ApplyDisplay();
    }

    std::vector<Choice> modes;
    for (const char* m : kScreenModes) {
      modes.push_back({m, ""});
    }
    modes[0].tooltip = "A window on the desktop. On Windows the desktop compositor can hold it below 60 FPS; fullscreen is smoother.";
    modes[1].tooltip = "Covers the screen without changing the display mode. Quick to switch.";
    modes[2].tooltip = "The game takes over the display; lets you pick the refresh rate.";
    row_label("Screen mode", Applies::kNow);
    if (DrawCombo("##screenmode", screen_mode_index_, modes, true, field_w)) {
      if (screen_mode_index_ != 2) {
        refresh_index_ = 0;
      }
      ApplyDisplay();
    }

    row_label("Refresh rate", Applies::kNow);
    if (DrawCombo("##refresh", refresh_index_, RefreshChoices(), screen_mode_index_ == 2,
                  field_w)) {
      ApplyDisplay();
    }

    row_label("Vsync", Applies::kRestart);
    DrawCombo("##vsync", vsync_index_,
              {{"Off", "Lowest latency; the picture may tear."},
               {"On", "No tearing; slightly more latency."}},
              true, field_w);

    row_label("FPS counter", Applies::kNow);
    if (DrawCombo("##fps", fps_index_,
                  {{"Off (original)", "No counter (F2 toggles it too)."},
                   {"FPS", "Frames per second, top right."},
                   {"FPS + frame time", "Also the time of the last frame, in ms."},
                   {"Detailed", "Also the GPU wait and GPU time breakdown (for testing)."}},
                  true, field_w)) {
      ApplyLive();
    }

    if (!options_.update_check_cvar.empty()) {
      row_label("Check for updates", Applies::kRestart);
      if (DrawCombo("##updatecheck", update_check_index_,
                    {{"Off", "Never asks GitHub for a newer version."},
                     {"On", "At start, the launcher asks GitHub once whether a newer version is "
                            "out and shows a link to it. Nothing is downloaded."}},
                    true, field_w)) {
        rex::cvar::SetFlagByName(options_.update_check_cvar,
                                 update_check_index_ ? "true" : "false");
      }
    }
  } else if (tab_ == 1) {
    // Graphics.
    std::vector<Choice> resolutions;
    for (const auto& r : kResolutions) {
      resolutions.push_back({r.label, r.tooltip});
    }
    row_label("Game resolution", Applies::kRestart);
    DrawCombo("##resolution", resolution_index_, resolutions, true, field_w);

    const auto& base = kResolutions[resolution_index_];
    std::vector<Choice> scales;
    for (int scale = 1; scale <= kMaxRenderScale; ++scale) {
      std::string label = fmt::format("{}x  ({} x {}{})", scale, base.width * scale,
                                      base.height * scale, scale == 1 ? ", native" : "");
      scales.push_back({std::move(label),
                        scale == 1 ? "Draws at the game resolution."
                                   : "Draws everything larger, then fits it to the screen: a "
                                     "sharper picture for more GPU work."});
    }
    row_label("Render scale", Applies::kRestart);
    DrawCombo("##scale", scale_index_, scales, true, field_w);

    std::vector<Choice> upscalers;
    for (const auto& u : kUpscalers) {
      upscalers.push_back({u.label, u.tooltip});
    }
    row_label("Upscaling filter", Applies::kNow);
    if (DrawCombo("##upscale", upscale_index_, upscalers, true, field_w)) {
      // Each filter starts at its own default sharpness.
      sharpness_index_ = std::string_view(kUpscalers[upscale_index_].cvar) == "cas"
                             ? kCasDefaultSharpness
                             : kFsrDefaultSharpness;
      ApplyLive();
    }

    const std::string_view effect = kUpscalers[upscale_index_].cvar;
    const int default_sharpness = effect == "cas" ? kCasDefaultSharpness : kFsrDefaultSharpness;
    std::vector<Choice> sharpness;
    for (int i = 0; i < int(std::size(kSharpness)); ++i) {
      std::string label = kSharpness[i].label;
      if (i == default_sharpness) {
        label += effect == "cas" ? "  - CAS default" : "  - FSR default";
      }
      sharpness.push_back({std::move(label), ""});
    }
    if (effect == "bilinear") {
      sharpness = {{"Only with FSR 1 or CAS", ""}};
    }
    row_label("Sharpening", Applies::kNow);
    int sharpness_shown = effect == "bilinear" ? 0 : sharpness_index_;
    if (DrawCombo("##sharpness", sharpness_shown, sharpness, effect != "bilinear", field_w) &&
        effect != "bilinear") {
      sharpness_index_ = sharpness_shown;
      ApplyLive();
    }

    std::vector<Choice> aniso;
    for (const auto& a : kAniso) {
      aniso.push_back({a.label, a.value < 0 ? "Each texture filtered as the game asks."
                                            : "Sharper ground and walls at shallow angles."});
    }
    row_label("Texture filtering", Applies::kNow);
    if (DrawCombo("##aniso", aniso_index_, aniso, true, field_w)) {
      ApplyLive();
    }

    row_label("Anti-aliasing", Applies::kNow);
    if (DrawCombo("##fxaa", fxaa_index_,
                  {{"Off (original)", "The game's own picture."},
                   {"FXAA", "Smooths jagged edges, slightly softer picture."},
                   {"FXAA (extreme)", "Stronger smoothing, softer still."}},
                  true, field_w)) {
      ApplyLive();
    }

    row_label("Frame rate", Applies::kNone);
    DrawCombo("##framerate", frame_rate_index_,
              {{"60 FPS (locked, as on the console)", "The game's speed is tied to 60 FPS."}},
              true, field_w);
  } else if (tab_ == 2) {
    // Audio.
    note("No audio options here yet.");
  } else {
    // Controls.
    note("Xbox-style controller, as on the console. Keyboard and mouse, and remapping, are "
         "planned for 0.3.0.");
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    note("Options menu: F1, Esc, or Start + Back on the controller.");
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Buttons.
  ImGui::Dummy(ImVec2(0, 16.0f * s));
  const float play_w = 220.0f * s;
  const float quit_w = (options_.in_game ? 170.0f : 140.0f) * s;
  const float button_h = 54.0f * s;
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + content_w - play_w - quit_w -
                       ImGui::GetStyle().ItemSpacing.x);
  ImGui::PushStyleColor(ImGuiCol_Button, kInk);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.26f, 0.19f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.34f, 0.26f, 0.19f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_Text, kTextLight);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_h * 0.5f);
  ImGui::PushFont(font_display, (condensed ? 28.0f : 19.0f) * s);
  if (ImGui::Button(options_.in_game ? "QUIT GAME" : "QUIT", ImVec2(quit_w, button_h)) &&
      !played_) {
    played_ = true;
    pending_action_ = 2;
    closing_since_ = ImGui::GetTime();
  }
  ImGui::PopFont();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  ImGui::SameLine();
  const bool play_enabled = options_.in_game || game_check_.ok;
  ImGui::BeginDisabled(!play_enabled);
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextLight);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, button_h * 0.5f);
  ImGui::PushFont(font_display, (condensed ? 33.0f : 22.0f) * s);
  bool play =
      ImGui::Button(options_.in_game ? "BACK TO GAME" : "PLAY", ImVec2(play_w, button_h));
  if (const int autoplay_ms = REXCVAR_GET(launcher_autoplay_ms); autoplay_ms > 0) {
    if (first_frame_) {
      shown_time_ = ImGui::GetTime();
    } else if ((ImGui::GetTime() - shown_time_) * 1000.0 >= autoplay_ms) {
      play = true;
    }
  }
  if (first_frame_ && play_enabled && REXCVAR_GET(options_menu_focus_row) < 0) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetKeyboardFocusHere(-1);
  }
  if (focus_frames_ > 0) {
    --focus_frames_;
  }
  ImGui::PopFont();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();

  // Footer.
  ImGui::Dummy(ImVec2(0, 4.0f * s));
  ImGui::PushFont(nullptr, 15.0f * s);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + content_w);
  if (options_.in_game) {
    const std::string footer = fmt::format(
        "{}NOW: changes at once. RESTART: the next time the game starts. Tabs: LB / RB or "
        "PgUp / PgDn. Close: B, Esc, F1 or Start + Back.",
        options_.in_game_note.empty() ? std::string() : options_.in_game_note + " ");
    ImGui::TextUnformatted(footer.c_str());
  } else {
    ImGui::TextUnformatted(
        "Mouse, keyboard (arrows, Enter, Esc, PgUp / PgDn for tabs) or controller (D-pad, A, B, "
        "LB / RB for tabs). Settings are saved for next time. In the game: F1, Esc or Start + "
        "Back for these options.");
  }
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::PopFont();

  ImGui::End();
  ImGui::PopFont();
  ImGui::PopStyleColor(20);
  ImGui::PopStyleVar(8);
  ImGui::PopStyleVar();  // alpha (open / close animation)

  first_frame_ = false;
  hot_row_ = hot_row_next_;
  hot_row_next_ = -1;
  current_row_ = -1;

  if (close_requested_) {
    close_requested_ = false;
    play = true;
  }
  if (play && play_enabled && !played_) {
    // Slide out first; the next frames run Play / Back to game (see the top of OnDraw).
    played_ = true;
    pending_action_ = 1;
    closing_since_ = ImGui::GetTime();
  }
}

}  // namespace rex::ui
