#pragma once

#include <audiokit/Macro.hpp>
#include <memory>
#include <array>

#if defined(_MSC_VER)
#include <malloc.h> // for _aligned_malloc
#endif

namespace audiokit {

class AUDIOKIT_ABI AudioData {
private:
  float* data_ = nullptr;
  std::array<float*, 8> channels_ = {};
};

}
