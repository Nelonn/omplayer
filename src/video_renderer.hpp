#pragma once

#include "av_clock.hpp"
#include "frame_queue.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

// VideoRenderer
//
// Consumes VideoFrames from a FrameQueue, compares each frame's pts_sec
// against the master AVClock, and uploads/displays when the frame is due.
//
// Frame pacing policy
// -------------------
//   diff = frame_pts_sec - clock_sec
//
//   diff >  FUTURE_THRESH  → frame is in the future; skip this tick
//   diff < -DROP_THRESH    → frame is very late; drop it, try next
//   otherwise              → display (the "just right" window)
//
// Only ONE frame is displayed per tick so the render loop controls pacing.
class VideoRenderer {
public:
  // Seconds: a frame more than this far ahead is held back.
  static constexpr double kFutureThresh = 0.040; // 40 ms
  // Seconds: a frame more than this far behind is dropped.
  static constexpr double kDropThresh = 0.150; // 150 ms

  ~VideoRenderer() {
    destroyTexture();
    if (transfer_buffer_) {
      SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
      transfer_buffer_ = nullptr;
    }
  }

  void setDevice(SDL_GPUDevice* d) { device_ = d; }

  // Called once per render-loop iteration from the main thread.
  // `clock` – the master clock this player uses.
  // Returns true if a new frame was uploaded (texture is dirty).
  auto tick(FrameQueue& queue, const AVClock& clock) -> bool {
    if (!device_) return false;

    const double master = clock.masterSeconds();
    bool uploaded = false;
    std::optional<VideoFrame> best_frame;

    // Process frames until we find the most recent one that is due.
    while (true) {
      auto front_pts = queue.frontPtsSec();
      if (!front_pts) break;

      const double diff = *front_pts - master;
      if (diff > kFutureThresh) break;

      auto opt = queue.tryPop();
      if (!opt) break;

      if (diff < -kDropThresh) {
        dropped_count_++;
        continue;
      }

      // Keep this frame as the best candidate to display.
      // If multiple frames are due, we only care about the latest one.
      best_frame = std::move(opt);
    }

    if (best_frame) {
      uploadFrame(*best_frame);
      last_pts_sec_ = best_frame->pts_sec;
      uploaded = true;
    }

    return uploaded;
  }

  auto texture() -> SDL_GPUTexture* {
    std::lock_guard lock(mutex_);
    return texture_;
  }

  auto textureWidth() const -> uint32_t { return tex_w_; }
  auto textureHeight() const -> uint32_t { return tex_h_; }
  auto droppedCount() const -> uint64_t { return dropped_count_; }
  auto lastPtsSec() const -> double { return last_pts_sec_; }

  void reset() {
    std::lock_guard lock(mutex_);
    destroyTextureUnsafe();
    last_pts_sec_ = 0.0;
    dropped_count_ = 0;
  }

private:
  void uploadFrame(const VideoFrame& vf) {
    std::lock_guard lock(mutex_);

    if (!texture_ || tex_w_ != vf.width || tex_h_ != vf.height) {
      destroyTextureUnsafe();
      SDL_GPUTextureCreateInfo texture_info = {};
      texture_info.type = SDL_GPU_TEXTURETYPE_2D;
      texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
      texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
      texture_info.width = vf.width;
      texture_info.height = vf.height;
      texture_info.layer_count_or_depth = 1;
      texture_info.num_levels = 1;
      texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
      texture_ = SDL_CreateGPUTexture(device_, &texture_info);
      tex_w_ = vf.width;
      tex_h_ = vf.height;
    }

    if (texture_ && !vf.rgba_plane.empty()) {
      uint32_t size = vf.width * vf.height * 4;

      if (!transfer_buffer_ || transfer_buffer_size_ < size) {
        if (transfer_buffer_) SDL_ReleaseGPUTransferBuffer(device_, transfer_buffer_);
        SDL_GPUTransferBufferCreateInfo tb_info = {};
        tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb_info.size = size;
        transfer_buffer_ = SDL_CreateGPUTransferBuffer(device_, &tb_info);
        transfer_buffer_size_ = size;
      }

      // Use cycle=false to ensure we wait for the buffer to be available
      void* data = SDL_MapGPUTransferBuffer(device_, transfer_buffer_, false);
      if (data) {
        std::memcpy(data, vf.rgba_plane.data(), size);
        SDL_UnmapGPUTransferBuffer(device_, transfer_buffer_);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd) {
          SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);

          SDL_GPUTextureTransferInfo src = {};
          src.transfer_buffer = transfer_buffer_;

          SDL_GPUTextureRegion dst = {};
          dst.texture = texture_;
          dst.w = vf.width;
          dst.h = vf.height;
          dst.d = 1;

          SDL_UploadToGPUTexture(copy, &src, &dst, false);
          SDL_EndGPUCopyPass(copy);
          SDL_SubmitGPUCommandBuffer(cmd);
        }
      }
    }
  }

  void destroyTexture() {
    std::lock_guard lock(mutex_);
    destroyTextureUnsafe();
  }

  void destroyTextureUnsafe() {
    if (texture_) {
      SDL_ReleaseGPUTexture(device_, texture_);
      texture_ = nullptr;
    }
    tex_w_ = tex_h_ = 0;
  }

  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  SDL_GPUTransferBuffer* transfer_buffer_ = nullptr;
  uint32_t transfer_buffer_size_ = 0;
  mutable std::mutex mutex_;

  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  uint64_t dropped_count_ = 0;
  double last_pts_sec_ = 0.0;
};
