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

      default: break;
    }
    return true;
  }

  void render(SDL_Window* window) {
    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);

    drawMedia(win_w, win_h);

    if (player_.isActive()) {
      drawBottomBar(win_w, win_h);
    }
  }

private:
  void setWindowAspectRatio(uint32_t media_w, uint32_t media_h) {
    // Set window aspect ratio constraint using SDL3's built-in support
    // This forces the window to maintain the media's aspect ratio during resize
    float ratio = static_cast<float>(media_w) / static_cast<float>(media_h);
    SDL_SetWindowAspectRatio(window_, ratio, ratio);
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

  void drawBottomBar(int win_w, int win_h) {
    ImGui::SetNextWindowPos(ImVec2(10, win_h - 70));
    ImGui::SetNextWindowSize(ImVec2(win_w - 20, 60));
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    // Progress bar
    float progress = player_.getProgress();
    if (ImGui::SliderFloat("##Progress", &progress, 0.0f, 1.0f, player_.getProgressString().c_str())) {
      player_.seek(progress);
    }

    // Volume
    float volume = player_.getVolume();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.5f, "%.2f")) {
      player_.setVolume(volume);
    }

    ImGui::End();
  }

  MediaPlayer& player_;
  SDL_Window* window_;
};
