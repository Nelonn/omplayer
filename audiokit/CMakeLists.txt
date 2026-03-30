cmake_minimum_required(VERSION 3.22)

project(audiokit)

add_library(audiokit STATIC
        audiokit/AudioData.hpp
        audiokit/AudioDevice.cpp
        audiokit/AudioDevice.hpp
        audiokit/AudioMixer.hpp
        audiokit/Macro.hpp
        audiokit/MultiChannelResampler.cpp
        audiokit/MultiChannelResampler.hpp
        audiokit/SincResampler.cpp
        audiokit/SincResampler.hpp
)
target_include_directories(audiokit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
