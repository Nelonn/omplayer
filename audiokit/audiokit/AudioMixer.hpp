#pragma once

#include "SincResampler.h"
#include "ring_buffer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

class AudioMixer {
public:
  // Unique identifier for each audio source
  using SourceId = uint64_t;

  // Configuration for an audio source
  struct SourceConfig {
    SDL_AudioFormat format = SDL_AUDIO_F32;
    int channels = 2;
    int sample_rate = 48000;
    float volume = 1.0f;
    bool paused = false;
  };

  static constexpr int kDefaultOutputChannels = 2;
  static constexpr int kDefaultOutputSampleRate = 48000;
  static constexpr SDL_AudioFormat kDefaultOutputFormat = SDL_AUDIO_F32;
  static constexpr size_t kMaxSources = 16;

  ~AudioMixer() = default;

  // Configure the mixer's output format
  void setOutputFormat(SDL_AudioFormat format, int channels, int sample_rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_format_ = format;
    output_channels_ = channels;
    output_sample_rate_ = sample_rate;
    output_frame_bytes_ = getBytesPerSample(format) * static_cast<size_t>(channels);
  }

  auto outputFormat() const -> SDL_AudioFormat { return output_format_; }
  auto outputChannels() const -> int { return output_channels_; }
  auto outputSampleRate() const -> int { return output_sample_rate_; }

  // Set master volume (0.0 to 1.5)
  void setMasterVolume(float volume) {
    master_volume_ = std::clamp(volume, 0.0f, 1.5f);
  }
  auto masterVolume() const -> float { return master_volume_; }

  // Add a new audio source with the given configuration
  // Returns SourceId on success, or std::nullopt if max sources reached
  auto addSource(const SourceConfig& config) -> std::optional<SourceId> {
    std::lock_guard<std::mutex> lock(mutex_);

    if (sources_.size() >= kMaxSources) {
      return std::nullopt;
    }

    const SourceId id = next_source_id_++;
    Source source;
    source.config = config;
    source.buffer = std::make_unique<RingBuffer<float>>(
        static_cast<size_t>(config.sample_rate) * 2 *
        static_cast<size_t>(config.channels));

    // Create resampler if source sample rate differs from output
    if (config.sample_rate != output_sample_rate_) {
      const double ratio = static_cast<double>(config.sample_rate) /
                           static_cast<double>(output_sample_rate_);
      source.resampler = std::make_unique<media::SincResampler>(
          ratio, media::SincResampler::kDefaultRequestSize,
          [this, id](int frames, float* destination) {
            resamplerReadCallback_(id, frames, destination);
          });
      source.resampler->PrimeWithSilence();
    }

    sources_[id] = std::move(source);
    return id;
  }

  // Remove an audio source
  void removeSource(SourceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sources_.erase(id);
  }

  // Check if a source exists
  auto hasSource(SourceId id) const -> bool {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.count(id) > 0;
  }

  // Set volume for a specific source (0.0 to 1.5)
  void setSourceVolume(SourceId id, float volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sources_.find(id); it != sources_.end()) {
      it->second.config.volume = std::clamp(volume, 0.0f, 1.5f);
    }
  }

  auto sourceVolume(SourceId id) const -> float {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sources_.find(id); it != sources_.end()) {
      return it->second.config.volume;
    }
    return 0.0f;
  }

  // Pause/resume a specific source
  void setSourcePaused(SourceId id, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sources_.find(id); it != sources_.end()) {
      it->second.config.paused = paused;
    }
  }

  auto isSourcePaused(SourceId id) const -> bool {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sources_.find(id); it != sources_.end()) {
      return it->second.config.paused;
    }
    return true;
  }

  // Push interleaved PCM float samples to a source
  // Returns bytes written (may be partial if buffer full)
  auto pushSamples(SourceId id, const float* samples, size_t num_samples) -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.buffer) {
      return 0;
    }

    const size_t bytes_to_write = num_samples * sizeof(float);
    return it->second.buffer->write(reinterpret_cast<const uint8_t*>(samples),
                                    bytes_to_write) /
           sizeof(float);
  }

  // Push interleaved PCM bytes to a source (will be converted to float)
  auto pushPcmBytes(SourceId id, const uint8_t* data, size_t len) -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.buffer) {
      return 0;
    }

    const auto& config = it->second.config;
    const size_t bps = getBytesPerSample(config.format);
    const size_t num_samples = len / bps;

    // Convert to float
    std::vector<float> float_samples(num_samples);
    convertToFloat(data, len, config.format, float_samples.data());

    return pushSamples(id, float_samples.data(), num_samples);
  }

  // Mix audio from all active sources into the output buffer
  // Output is interleaved float samples in the mixer's output format
  void mix(float* output_buffer, size_t num_frames) {
    std::lock_guard<std::mutex> lock(mutex_);

    const size_t num_output_samples = num_frames * static_cast<size_t>(output_channels_);

    // Clear output buffer
    std::fill(output_buffer, output_buffer + num_output_samples, 0.0f);

    if (sources_.empty()) {
      return;
    }

    // Mix each active source
    for (auto& [id, source] : sources_) {
      if (source.config.paused || !source.buffer) {
        continue;
      }

      // Get samples from this source (resampled if needed)
      std::vector<float> source_samples;
      if (source.resampler) {
        // Need resampling
        source_samples.resize(num_frames * static_cast<size_t>(output_channels_));
        source.resampler->Resample(static_cast<int>(num_frames),
                                   source_samples.data());
      } else {
        // No resampling needed - read directly from buffer
        source_samples.resize(num_frames * static_cast<size_t>(output_channels_));
        const size_t bytes_to_read = source_samples.size() * sizeof(float);
        const size_t bytes_read =
            source.buffer->read(reinterpret_cast<uint8_t*>(source_samples.data()),
                                bytes_to_read);
        // Fill remainder with silence if buffer underrun
        if (bytes_read < bytes_to_read) {
          std::fill(source_samples.begin() + bytes_read / sizeof(float),
                    source_samples.end(), 0.0f);
        }
      }

      // Apply source volume and mix into output
      const float source_volume = source.config.volume;
      for (size_t i = 0; i < num_output_samples; ++i) {
        output_buffer[i] += source_samples[i] * source_volume;
      }
    }

    // Apply master volume and clip to [-1.0, 1.0]
    for (size_t i = 0; i < num_output_samples; ++i) {
      output_buffer[i] = std::clamp(output_buffer[i] * master_volume_, -1.0f, 1.0f);
    }
  }

  // Get mixed audio as bytes in the output format
  auto mixToBytes(size_t num_frames) -> std::vector<uint8_t> {
    const size_t num_samples = num_frames * static_cast<size_t>(output_channels_);
    std::vector<float> float_buffer(num_samples);

    mix(float_buffer.data(), num_frames);

    // Convert float to output format
    std::vector<uint8_t> output(num_frames * output_frame_bytes_);
    convertFromFloat(float_buffer.data(), num_samples, output_format_,
                     output.data());

    return output;
  }

  // Clear all buffered data for a source (useful for seeking)
  void clearSourceBuffer(SourceId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sources_.find(id); it != sources_.end()) {
      if (it->second.buffer) {
        it->second.buffer->clear();
      }
      if (it->second.resampler) {
        it->second.resampler->Flush();
        it->second.resampler->PrimeWithSilence();
      }
    }
  }

  // Clear all sources
  void clearAllSources() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, source] : sources_) {
      if (source.buffer) source.buffer->clear();
      if (source.resampler) {
        source.resampler->Flush();
        source.resampler->PrimeWithSilence();
      }
    }
  }

  // Get the number of active sources
  auto sourceCount() const -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    return sources_.size();
  }

private:
  struct Source {
    SourceConfig config;
    std::unique_ptr<RingBuffer<float>> buffer;
    std::unique_ptr<media::SincResampler> resampler;
  };

  SDL_AudioFormat output_format_ = kDefaultOutputFormat;
  int output_channels_ = kDefaultOutputChannels;
  int output_sample_rate_ = kDefaultOutputSampleRate;
  size_t output_frame_bytes_ = 4 * 2; // F32 * 2 channels

  float master_volume_ = 1.0f;

  std::unordered_map<SourceId, Source> sources_;
  SourceId next_source_id_ = 1;

  mutable std::mutex mutex_;

  // Callback for resampler to read from source buffer
  void resamplerReadCallback_(SourceId id, int frames, float* destination) {
    // Note: This is called from within mix(), which already holds the lock
    // We need to be careful not to deadlock
    auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.buffer) {
      std::fill(destination, destination + frames * output_channels_, 0.0f);
      return;
    }

    const size_t samples_needed = static_cast<size_t>(frames) *
                                  static_cast<size_t>(output_channels_);
    const size_t bytes_needed = samples_needed * sizeof(float);

    // Read from buffer (may be less if underrun)
    const size_t bytes_read = it->second.buffer->read(
        reinterpret_cast<uint8_t*>(destination), bytes_needed);

    // Fill remainder with silence
    if (bytes_read < bytes_needed) {
      const size_t samples_read = bytes_read / sizeof(float);
      std::fill(destination + samples_read, destination + samples_needed, 0.0f);
    }
  }

  // Get bytes per sample for a given SDL audio format
  static auto getBytesPerSample(SDL_AudioFormat fmt) -> size_t {
    if (SDL_AUDIO_ISF32(fmt)) return 4;
    if (SDL_AUDIO_ISF64(fmt)) return 8;
    if (SDL_AUDIO_ISS32(fmt)) return 4;
    if (SDL_AUDIO_ISS16(fmt)) return 2;
    if (SDL_AUDIO_ISU8(fmt)) return 1;
    return 4; // Default to F32
  }

  // Convert PCM bytes to float samples
  static void convertToFloat(const uint8_t* src, size_t len,
                             SDL_AudioFormat fmt, float* dst) {
    if (SDL_AUDIO_ISF32(fmt)) {
      std::memcpy(dst, src, len);
      return;
    }

    const size_t num_samples = len / getBytesPerSample(fmt);

    if (SDL_AUDIO_ISS32(fmt)) {
      const auto* s = reinterpret_cast<const int32_t*>(src);
      for (size_t i = 0; i < num_samples; ++i) {
        dst[i] = static_cast<float>(s[i]) / 2147483648.0f;
      }
    } else if (SDL_AUDIO_ISS16(fmt)) {
      const auto* s = reinterpret_cast<const int16_t*>(src);
      for (size_t i = 0; i < num_samples; ++i) {
        dst[i] = static_cast<float>(s[i]) / 32768.0f;
      }
    } else if (SDL_AUDIO_ISU8(fmt)) {
      const auto* s = reinterpret_cast<const uint8_t*>(src);
      for (size_t i = 0; i < num_samples; ++i) {
        dst[i] = (static_cast<float>(s[i]) - 128.0f) / 128.0f;
      }
    } else {
      // Unknown format, copy as-is
      std::memcpy(dst, src, len);
    }
  }

  // Convert float samples to PCM bytes
  static void convertFromFloat(const float* src, size_t num_samples,
                               SDL_AudioFormat fmt, uint8_t* dst) {
    if (SDL_AUDIO_ISF32(fmt)) {
      std::memcpy(dst, src, num_samples * sizeof(float));
      return;
    }

    if (SDL_AUDIO_ISS32(fmt)) {
      auto* d = reinterpret_cast<int32_t*>(dst);
      for (size_t i = 0; i < num_samples; ++i) {
        d[i] = static_cast<int32_t>(std::clamp(src[i], -1.0f, 1.0f) *
                                    2147483647.0f);
      }
    } else if (SDL_AUDIO_ISS16(fmt)) {
      auto* d = reinterpret_cast<int16_t*>(dst);
      for (size_t i = 0; i < num_samples; ++i) {
        d[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
      }
    } else if (SDL_AUDIO_ISU8(fmt)) {
      auto* d = reinterpret_cast<uint8_t*>(dst);
      for (size_t i = 0; i < num_samples; ++i) {
        d[i] = static_cast<uint8_t>(std::clamp(src[i], -1.0f, 1.0f) * 127.0f +
                                    128.0f);
      }
    } else {
      // Unknown format, copy as float
      std::memcpy(dst, src, num_samples * sizeof(float));
    }
  }
};
