#pragma once

#include "av_clock.hpp"
#include "frame_queue.hpp"

#include <SDL3/SDL.h>
#include <mutex>
#include <algorithm>
#include <vector>
#include <cstring>

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
  static constexpr double kFutureThresh = 0.010; // 10 ms
  // Seconds: a frame more than this far behind is dropped.
  static constexpr double kDropThresh = 0.100; // 100 ms

  ~VideoRenderer() { destroyTexture(); }

  void setDevice(SDL_GPUDevice* d) { device_ = d; }

  // Called once per render-loop iteration from the main thread.
  // `clock` – the master clock this player uses.
  // Returns true if a new frame was uploaded (texture is dirty).
  auto tick(FrameQueue& queue, const AVClock& clock) -> bool {
    if (!device_) return false;

    const double master = clock.masterSeconds();
    bool uploaded = false;

    // Process frames until we either display one or run out of due frames.
    while (true) {
      auto opt = queue.peekPop([&](double pts_sec) {
        return (pts_sec - master) <= kFutureThresh;
      });

      if (!opt) break;

      const double diff = opt->pts_sec - master;

      if (diff < -kDropThresh) {
        dropped_count_++;
        continue;
      }

      uploadFrame(*opt);
      last_pts_sec_ = opt->pts_sec;
      uploaded = true;
      break;
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

    if (texture_) {
        // Convert YUV to RGBA on CPU (simplest for migration)
        std::vector<uint32_t> rgba(vf.width * vf.height);
        for (uint32_t y = 0; y < vf.height; ++y) {
            for (uint32_t x = 0; x < vf.width; ++x) {
                int py = vf.y_plane[y * vf.y_stride + x];
                int pu = vf.u_plane[(y / 2) * vf.u_stride + (x / 2)];
                int pv = vf.v_plane[(y / 2) * vf.v_stride + (x / 2)];

                int c = py - 16;
                int d = pu - 128;
                int e = pv - 128;

                int r = std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255);
                int g = std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255);
                int b = std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255);

                rgba[y * vf.width + x] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            }
        }

        uint32_t size = vf.width * vf.height * 4;
        SDL_GPUTransferBufferCreateInfo tb_info = {};
        tb_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tb_info.size = size;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device_, &tb_info);
        
        void* data = SDL_MapGPUTransferBuffer(device_, tb, false);
        std::memcpy(data, rgba.data(), size);
        SDL_UnmapGPUTransferBuffer(device_, tb);

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        
        SDL_GPUTextureTransferInfo src = {};
        src.transfer_buffer = tb;
        
        SDL_GPUTextureRegion dst = {};
        dst.texture = texture_;
        dst.w = vf.width;
        dst.h = vf.height;
        dst.d = 1;
        
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(cmd);
        
        SDL_ReleaseGPUTransferBuffer(device_, tb);
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
  mutable std::mutex mutex_;

  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  uint64_t dropped_count_ = 0;
  double last_pts_sec_ = 0.0;
};
