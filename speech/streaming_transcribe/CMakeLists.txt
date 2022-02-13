# Copyright 2022 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

set(VCPKG_OVERLAY_PORTS ${CMAKE_SOURCE_DIR}/vcpkg-overlay)

cmake_minimum_required(VERSION 3.16)
project(google-cloud-cpp-speech-quickstart CXX)

find_package(google_cloud_cpp_speech REQUIRED)
find_package(RtAudio CONFIG REQUIRED)

# MSVC requires some additional code to select the correct runtime library
if (VCPKG_TARGET_TRIPLET MATCHES "-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else ()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif ()

# Define your targets.
add_executable(streaming_transcribe infinite_transcribe.cc)
target_link_libraries(streaming_transcribe PRIVATE google-cloud-cpp::experimental-speech RtAudio::rtaudio)
target_compile_features(streaming_transcribe PRIVATE cxx_std_20)
