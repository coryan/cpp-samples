// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <google/cloud/grpc_options.h>
#include <google/cloud/speech/speech_client.h>
#include <RtAudio.h>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

namespace speech = ::google::cloud::speech;
namespace gc = ::google::cloud;

class PrintWords {
 public:
  PrintWords() = default;

  void Run() {
    auto background =
        std::thread([](gc::CompletionQueue cq) { cq.Run(); }, cq_);
    if (adc_.getDeviceCount() < 1) {
      throw std::runtime_error("No audio devices found");
    }
    RtAudio::StreamParameters parameters;
    parameters.deviceId = adc_.getDefaultInputDevice();
    parameters.nChannels = 1;
    parameters.firstChannel = 0;
    auto constexpr kSampleRate = static_cast<unsigned int>(44100);
    auto buffer_frames = static_cast<unsigned int>(256);
    adc_.openStream(nullptr, &parameters, RTAUDIO_SINT16, kSampleRate,
                    &buffer_frames, &record_callback);

    adc_.startStream();

    StartRecognitionStream(std::chrono::milliseconds(500));
  }

 private:
  using RecognitionStream = std::unique_ptr<
      gc::AsyncStreamingReadWriteRpc<speech::v1::StreamingRecognizeRequest,
                                     speech::v1::StreamingRecognizeResponse>>;

  void StartRecognitionStream(std::chrono::milliseconds backoff) {
    auto stream = client_.AsyncStreamingRecognize();
    auto start = stream->Start();
    start.then([this, backoff, s = std::move(stream)](auto f) mutable {
      this->OnStart(std::move(s), backoff, f.get());
    });
  }

  void OnStart(RecognitionStream stream, std::chrono::milliseconds backoff,
               bool ok) {
    if (!ok) {
      stream->Finish().then(
          [this, backoff](auto f) { return OnStartError(f.get(), backoff); });
      return;
    }
  }

  void OnStartError(gc::Status const& status,
                    std::chrono::milliseconds backoff) {
    if (status.code() != gc::StatusCode::kUnavailable) {
      std::cerr << "Unrecoverable error starting recognition stream: " << status
                << "\n";
      return Shutdown();
    }
    cq_.MakeRelativeTimer(backoff).then([this, b = 2 * backoff](auto f) {
      auto status = f.get().status();
      if (status.ok()) return OnTimerError(std::move(status));
      StartRecognitionStream(b);
    });
  }

  void OnTimerError(gc::Status const& status) {
    std::cerr << "Unrecoverable error in backoff timer: " << status << "\n";
    return Shutdown();
  }

  void Shutdown() {
    adc_.stopStream();
    if (adc_.isStreamOpen()) adc_.closeStream();
  }

  int OnRecord(std::span<std::int16_t const> buffer) {
    std::vector<char> bytes(buffer.size() * 2);
    // std::span<boost::endian::little_int16_t> little(bytes.begin(),
    // bytes.end());
    std::span<std::int16_t> dest(reinterpret_cast<std::int16_t*>(bytes.data()),
                                 buffer.size());
    std::copy(buffer.begin(), buffer.end(), dest.begin());
    queue_.mutable_audio_content()->append(bytes.begin(), bytes.end());

    return 0;
  }

  static int record_callback(void* /*output_buffer*/, void* input_buffer,
                             unsigned int buffer_frames, double /*stream_time*/,
                             RtAudioStreamStatus /*status*/, void* user_data) {
    auto const* buffer = static_cast<std::int16_t const*>(input_buffer);
    return reinterpret_cast<PrintWords*>(user_data)->OnRecord(
        std::span<std::int16_t const>(buffer, buffer_frames));
  }

  gc::CompletionQueue cq_;
  speech::SpeechClient client_ =
      speech::SpeechClient(speech::MakeSpeechConnection(
          gc::Options{}.set<gc::GrpcCompletionQueueOption>(cq_)));
  RtAudio adc_;

  speech::v1::StreamingRecognizeRequest queue_;
  std::mutex mu_;
  RecognitionStream stream_;
  bool pending_read_ = false;
  bool pending_write_ = false;
};

int main(int argc, char* argv[]) try {
  if (argc > 1) {
    std::cerr << "Usage: " << argv[0] << "\n";
    return 1;
  }

  RtAudio adc;

  std::cout << "\nRecording ... press <enter> to quit.\n";
  std::cin.get();

  auto client = speech::SpeechClient(speech::MakeSpeechConnection());

  google::cloud::speech::v1::RecognitionConfig config;
  config.set_language_code("en-US");
  // google::cloud::speech::v1::RecognitionAudio audio;
  // audio.set_uri(uri);
  // auto response = client.Recognize(config, audio);
  // if (!response) throw std::runtime_error(response.status().message());
  // std::cout << response->DebugString() << "\n";

  return 0;
} catch (RtAudioError& e) {
  std::cerr << "RtAudioError thrown ";
  e.printMessage();
  return 1;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception raised: " << ex.what() << "\n";
  return 1;
}
