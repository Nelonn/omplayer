#pragma once

#include <vector>
#include <optional>
#include <audiokit/Macro.hpp>
#include <audiokit/SincResampler.hpp>

namespace audiokit {

class AUDIOKIT_ABI MultiChannelResampler {
public:
  // Callback type for providing more data into the resampler.  Expects AudioBus
  // to be completely filled with data upon return; zero padded if not enough
  // frames are available to satisfy the request.
  using ReadCB = std::function<void(AudioBus* audio_bus)>;

  // Constructs a MultiChannelResampler with the specified |read_cb|, which is
  // used to acquire audio data for resampling.  |io_sample_rate_ratio| is the
  // ratio of input / output sample rates.
  MultiChannelResampler(int channels, double io_sample_rate_ratio,
                        const ReadCB& read_cb);
  virtual ~MultiChannelResampler();

  // Resamples |frames| of data from |read_cb_| into AudioBus.
  void Resample(AudioBus* audio_bus, int frames);

  // Flush all buffered data and reset internal indices.
  void Flush();

private:
  // SincResampler::ReadCB implementation.  ProvideInput() will be called for
  // each channel (in channel order) as SincResampler needs more data.
  void ProvideInput(int channel, float* destination, int frames);

  // Sanity check to ensure that ProvideInput() retrieves the same number of
  // frames for every channel.
  int last_frame_count_;

  // Source of data for resampling.
  ReadCB read_cb_;

  // Each channel has its own high quality resampler.
  std::vector<std::unique_ptr<SincResampler>> resamplers_;

  // Buffers for audio data going into SincResampler from ReadCB.
  scoped_ptr<AudioBus> resampler_audio_bus_;
  scoped_ptr<AudioBus> wrapped_resampler_audio_bus_;
  std::vector<float*> resampler_audio_data_;
};

}
