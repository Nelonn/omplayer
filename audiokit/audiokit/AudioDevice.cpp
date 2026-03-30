#include "AudioDevice.hpp"

namespace audiokit {

auto AudioDevice::openDevice() -> bool {
  SDL_AudioSpec src_spec = {};
  src_spec.format = fmt_.sdl_fmt;
  src_spec.channels = fmt_.channels;
  src_spec.freq = fmt_.sample_rate;

  device_id_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!device_id_) {
    SDL_Log("[AudioDevice] SDL_OpenAudioDevice: %s", SDL_GetError());
    return false;
  }

  SDL_AudioSpec hw_spec {};
  if (!SDL_GetAudioDeviceFormat(device_id_, &hw_spec, nullptr)) {
    SDL_Log("[AudioDevice] SDL_GetAudioDeviceFormat: %s", SDL_GetError());
  } else {
    SDL_Log("[AudioDevice] HW spec: fmt=%d ch=%d freq=%d",
            hw_spec.format, hw_spec.channels, hw_spec.freq);
  }

  const SDL_AudioSpec* dst = hw_spec.freq ? &hw_spec : &src_spec;
  stream_ = SDL_CreateAudioStream(&src_spec, dst);
  if (!stream_) {
    SDL_Log("[AudioDevice] SDL_CreateAudioStream: %s", SDL_GetError());
    SDL_CloseAudioDevice(device_id_);
    device_id_ = 0;
    return false;
  }

  SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);

  if (!SDL_BindAudioStream(device_id_, stream_)) {
    SDL_Log("[AudioDevice] SDL_BindAudioStream: %s", SDL_GetError());
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
    SDL_CloseAudioDevice(device_id_);
    device_id_ = 0;
    return false;
  }

  SDL_SetAudioDeviceGain(device_id_, gain_);

  if (!ring_) {
    ring_ = std::make_unique<RingBuffer<uint8_t>>(
        static_cast<size_t>(fmt_.sample_rate) * 2 * frame_bytes_);
  }

  SDL_PauseAudioDevice(device_id_);
  open_ = true;
  SDL_Log("[AudioDevice] Opened (device_id=%u)", device_id_);
  return true;
}

void AudioDevice::destroyStream() {
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
  if (device_id_) {
    SDL_CloseAudioDevice(device_id_);
    device_id_ = 0;
  }
}

void AudioDevice::reopen(const char* reason) {
  SDL_Log("[AudioDevice] Reopening - %s", reason);
  const bool was_started = started_;

  if (stream_) SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);

  destroyStream();
  open_ = false;
  started_ = false;

  if (!openDevice()) {
    SDL_Log("[AudioDevice] Reopen failed.");
    return;
  }

  if (was_started) {
    started_ = true;
    SDL_ResumeAudioDevice(device_id_);
  }

  if (reopen_cb_) reopen_cb_();
}

void AudioDevice::onDeviceFormatChanged(const SDL_AudioDeviceEvent& ev) {
  if (ev.recording) return;

  if (ev.which != device_id_) return;

  SDL_Log("[AudioDevice] Device %u format changed – recreating stream.", ev.which);

  if (stream_) SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
  if (stream_) {
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }

  SDL_AudioSpec src_spec {fmt_.sdl_fmt, fmt_.channels, fmt_.sample_rate};
  SDL_AudioSpec hw_spec {};
  SDL_GetAudioDeviceFormat(device_id_, &hw_spec, nullptr);
  const SDL_AudioSpec* dst = hw_spec.freq ? &hw_spec : &src_spec;

  stream_ = SDL_CreateAudioStream(&src_spec, dst);
  if (!stream_) {
    SDL_Log("[AudioDevice] Recreate stream after format change: %s", SDL_GetError());
    return;
  }
  SDL_SetAudioStreamGetCallback(stream_, audioCallbackS, this);
  SDL_BindAudioStream(device_id_, stream_);
  SDL_SetAudioDeviceGain(device_id_, gain_);

  if (started_) SDL_ResumeAudioDevice(device_id_);
  SDL_Log("[AudioDevice] Stream recreated after format change.");
}

void AudioDevice::audioCallback(SDL_AudioStream* stream, int need) {
  if (!ring_ || !clock_) return;

  const size_t available = ring_->currentSize();
  const size_t to_read = std::min(available, static_cast<size_t>(need));

  if (to_read > 0) {
    tmp_buf_.resize(to_read);
    const size_t n = ring_->read(tmp_buf_.data(), to_read);
    SDL_PutAudioStreamData(stream, tmp_buf_.data(), static_cast<int>(n));
    bytes_played_.fetch_add(n, std::memory_order_relaxed);
  }

  const size_t gap = static_cast<size_t>(need) - to_read;
  if (gap > 0) {
    silence_buf_.assign(gap, 0);
    SDL_PutAudioStreamData(stream, silence_buf_.data(), static_cast<int>(gap));
    // Do NOT advance bytes_played_ for silence – keep clock honest.
  }

  advanceClock();
}

}
