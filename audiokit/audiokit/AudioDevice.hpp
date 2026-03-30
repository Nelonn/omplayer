#pragma once

#pragma once

#include "av_clock.hpp"
#include "ring_buffer.hpp"

#include <SDL3/SDL.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace audiokit {

class AudioDevice {
public:
  struct Format {
    SDL_AudioFormat sdl_fmt = SDL_AUDIO_F32;
    int channels = 2;
    int sample_rate = 48000;
    size_t bytes_per_sample = 4; // sizeof(float) for F32
  };

  using ReopenCallback = std::function<void()>;

private:
  AVClock* clock_ = nullptr;
  SDL_AudioDeviceID device_id_ = 0;
  SDL_AudioStream* stream_ = nullptr;

  Format fmt_ = {};
  size_t frame_bytes_ = 0;

  mutable std::mutex sync_mutex_;
  double start_pts_ = -1.0; // PTS of first pushed byte
  uint64_t total_bytes_pushed_ = 0;
  std::atomic<uint64_t> bytes_played_ {0};

  float gain_ = 1.0f;
  bool open_ = false;
  bool started_ = false;

  std::vector<uint8_t> tmp_buf_;
  std::vector<uint8_t> silence_buf_;

  ReopenCallback reopen_cb_;

public:

  ~AudioDevice() { close(); }

  auto open(const Format& fmt, AVClock* clock) -> bool {
    close();

    clock_ = clock;
    fmt_ = fmt;
    frame_bytes_ = fmt_.bytes_per_sample * static_cast<size_t>(fmt_.channels);

    return openDevice();
  }

  void close() {
    destroyStream();
    open_ = false;
    started_ = false;
  }

  void handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
      case SDL_EVENT_AUDIO_DEVICE_ADDED:
        onDeviceAdded(ev.adevice);
        break;
      case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        onDeviceRemoved(ev.adevice);
        break;
      case SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED:
        onDeviceFormatChanged(ev.adevice);
        break;
      default:
        break;
    }
  }

  void pause() {
    if (device_id_) SDL_PauseAudioDevice(device_id_);
  }

  void resume() {
    if (device_id_ && started_) SDL_ResumeAudioDevice(device_id_);
  }

  void setGain(float g) {
    gain_ = std::clamp(g, 0.0f, 1.5f);
    if (device_id_) SDL_SetAudioDeviceGain(device_id_, gain_);
  }
  auto gain() const -> float { return gain_; }

  auto isOpen() const -> bool { return open_; }
  auto isStarted() const -> bool { return started_; }

  void setReopenCallback(ReopenCallback cb) {
    reopen_cb_ = std::move(cb);
  }

private:
  auto openDevice() -> bool;

  void destroyStream();

  void reopen(const char* reason);

  void onDeviceAdded(const SDL_AudioDeviceEvent& ev) {
    if (ev.recording) return;

    if (!open_) {
      SDL_Log("[AudioDevice] New playback device %u detected while closed – opening.", ev.which);
      openDevice();
    }
  }

  void onDeviceRemoved(const SDL_AudioDeviceEvent& ev) {
    if (ev.recording) return;

    if (ev.which != device_id_) return;

    SDL_Log("[AudioDevice] Our device %u removed – reopening on new default.", ev.which);
    reopen("device removed");
  }

  void onDeviceFormatChanged(const SDL_AudioDeviceEvent& ev);

  static void SDLCALL audioCallbackS(void* userdata, SDL_AudioStream* stream,
                                      int need, int /*available*/) {
    static_cast<AudioDevice*>(userdata)->audioCallback(stream, need);
  }

  void audioCallback(SDL_AudioStream* stream, int need);

  void advanceClock() {
    double start;
    {
      std::lock_guard<std::mutex> lk(sync_mutex_);
      start = start_pts_;
    }
    if (start < 0.0 || fmt_.sample_rate == 0 || frame_bytes_ == 0) return;

    const double clock_sec =
        start + static_cast<double>(bytes_played_.load(std::memory_order_relaxed)) / (static_cast<double>(fmt_.sample_rate) * static_cast<double>(frame_bytes_));

    clock_->setAudioSeconds(clock_sec);
  }
};

}
