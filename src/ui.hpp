#pragma once
#include "imgui.h"
#include "media_player.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <string>

// ---------------------------------------------------------------------------
// PlayerUI
// ---------------------------------------------------------------------------
class PlayerUI {
public:
  explicit PlayerUI(MediaPlayer& player, SDL_Window* window)
      : player_(player), window_(window) {
    // Set up callback to constrain window aspect ratio when media dimensions are known
    player_.onMediaSize = [this](uint32_t width, uint32_t height) {
      setWindowAspectRatio(width, height);
    };
  }

  auto handleEvent(const SDL_Event& e) -> bool {
    switch (e.type) {
      case SDL_EVENT_QUIT:
        return false;

      case SDL_EVENT_DROP_FILE:
        player_.play(e.drop.data);
        break;

      case SDL_EVENT_MOUSE_BUTTON_UP:
        // Right click to toggle pause/play
        if (e.button.button == SDL_BUTTON_RIGHT && player_.isActive()) {
          player_.togglePause();
        }
        // Left click on control bar area for seeking
        else if (e.button.button == SDL_BUTTON_LEFT && player_.isActive()) {
          int win_w, win_h;
          SDL_GetWindowSize(window_, &win_w, &win_h);
          constexpr float bar_height = 48.0f;
          constexpr float bar_margin = 12.0f;
          constexpr float progress_bar_height = 4.0f;
          constexpr float padding = 12.0f;
          constexpr float time_width = 60.0f;
          constexpr float control_bar_max_width = 600.0f;
          
          float bar_width = std::min(static_cast<float>(win_w) - bar_margin * 2.0f, control_bar_max_width);
          float bar_x = (static_cast<float>(win_w) - bar_width) * 0.5f;
          float bar_y = static_cast<float>(win_h) - bar_height - bar_margin;
          float progress_bar_x = bar_x + padding + time_width;
          float progress_bar_w = bar_width - padding * 2.0f - time_width * 2.0f;
          float progress_bar_y = bar_y + (bar_height - progress_bar_height) * 0.5f;
          
          if (e.button.y >= progress_bar_y && e.button.y <= static_cast<int>(progress_bar_y + progress_bar_height)) {
            if (e.button.x >= progress_bar_x && e.button.x <= static_cast<int>(progress_bar_x + progress_bar_w)) {
              float click_x = static_cast<float>(e.button.x) - progress_bar_x;
              float progress = std::clamp(click_x / progress_bar_w, 0.0f, 1.0f);
              player_.seek(progress);
            }
          }
        }
        break;

      case SDL_EVENT_MOUSE_WHEEL:
        // Scroll to change volume in 2% steps (round to integer for discrete steps)
        if (player_.isActive()) {
          float current_volume = player_.getVolume();
          int scroll_steps = static_cast<int>(std::round(static_cast<double>(e.wheel.y)));
          float step = 0.02f * static_cast<float>(scroll_steps);
          float new_volume = std::clamp(current_volume + step, 0.0f, 1.5f);
          player_.setVolume(new_volume);
        }
        break;

      case SDL_EVENT_AUDIO_DEVICE_ADDED:

        break;

      case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        break;

      case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        break;

      default: break;
    }
    return true;
  }

  void render(SDL_Window* window) {
    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);

    drawMedia(win_w, win_h);

    if (player_.isActive()) {
      drawControlBar(win_w, win_h);
    }
  }

private:
  void setWindowAspectRatio(uint32_t media_w, uint32_t media_h) {
    // Set window aspect ratio constraint using SDL3's built-in support
    // This forces the window to maintain the media's aspect ratio during resize
    float ratio = static_cast<float>(media_w) / static_cast<float>(media_h);
    SDL_SetWindowAspectRatio(window_, ratio, ratio);
  }

  void drawControlBar(int win_w, int win_h) {
    constexpr float bar_height = 48.0f;
    constexpr float bar_margin = 12.0f;
    constexpr float bar_radius = 8.0f;
    constexpr float progress_bar_height = 4.0f;
    constexpr float padding = 12.0f;
    constexpr float time_width = 60.0f;
    constexpr float control_bar_max_width = 600.0f;

    // Calculate bar width (centered, max width)
    float bar_width = std::min(static_cast<float>(win_w) - bar_margin * 2.0f, control_bar_max_width);
    float bar_x = (static_cast<float>(win_w) - bar_width) * 0.5f;
    float bar_y = static_cast<float>(win_h) - bar_height - bar_margin;

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Draw rounded rectangle background
    ImU32 bg_color = IM_COL32(30, 30, 30, 230);
    draw_list->AddRectFilled(
        ImVec2(bar_x, bar_y),
        ImVec2(bar_x + bar_width, bar_y + bar_height),
        bg_color,
        bar_radius
    );

    // Time text
    std::string current_time = player_.getProgressString();
    size_t sep_pos = current_time.find(" / ");
    std::string cur = (sep_pos != std::string::npos) ? current_time.substr(0, sep_pos) : current_time;
    std::string dur = (sep_pos != std::string::npos) ? current_time.substr(sep_pos + 3) : "00:00";

    ImVec2 cur_text_size = ImGui::CalcTextSize(cur.c_str());
    ImVec2 dur_text_size = ImGui::CalcTextSize(dur.c_str());

    // Progress bar area (between time labels)
    float progress_bar_x = bar_x + padding + time_width;
    float progress_bar_w = bar_width - padding * 2.0f - time_width * 2.0f;
    float progress_bar_y = bar_y + (bar_height - progress_bar_height) * 0.5f;

    // Draw progress bar background
    ImU32 progress_bg = IM_COL32(80, 80, 80, 255);
    draw_list->AddRectFilled(
        ImVec2(progress_bar_x, progress_bar_y),
        ImVec2(progress_bar_x + progress_bar_w, progress_bar_y + progress_bar_height),
        progress_bg,
        progress_bar_height * 0.5f
    );

    // Draw progress bar fill
    float progress = player_.getProgress();
    float fill_width = progress_bar_w * progress;
    ImU32 progress_fill = IM_COL32(255, 255, 255, 255);
    draw_list->AddRectFilled(
        ImVec2(progress_bar_x, progress_bar_y),
        ImVec2(progress_bar_x + fill_width, progress_bar_y + progress_bar_height),
        progress_fill,
        progress_bar_height * 0.5f
    );

    // Draw current time (left) - vertically centered
    draw_list->AddText(
        ImVec2(bar_x + padding, bar_y + (bar_height - cur_text_size.y) * 0.5f),
        IM_COL32(255, 255, 255, 255),
        cur.c_str()
    );

    // Draw duration (right) - vertically centered
    draw_list->AddText(
        ImVec2(bar_x + bar_width - padding - dur_text_size.x, bar_y + (bar_height - dur_text_size.y) * 0.5f),
        IM_COL32(255, 255, 255, 255),
        dur.c_str()
    );
  }

  void drawMedia(int win_w, int win_h) {
    SDL_GPUTexture* tex = nullptr;
    uint32_t mw = 0, mh = 0;

    if (player_.hasVideo()) {
      tex = player_.getVideoTexture();
      std::tie(mw, mh) = player_.getVideoSize();
    } else if (player_.hasImage()) {
      tex = player_.getImageTexture();
      std::tie(mw, mh) = player_.getImageSize();
    }

    if (tex && mw > 0 && mh > 0) {
      const float scale = std::min(float(win_w) / float(mw), float(win_h) / float(mh));
      const float dw = mw * scale;
      const float dh = mh * scale;
      const float dx = (win_w - dw) * 0.5f;
      const float dy = (win_h - dh) * 0.5f;

      ImGui::GetBackgroundDrawList()->AddImage(
          (ImTextureID) tex,
          ImVec2(dx, dy),
          ImVec2(dx + dw, dy + dh));
    }
  }

  MediaPlayer& player_;
  SDL_Window* window_;
};
