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
#include <boost/endian.hpp>
#include <RtAudio.h>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

namespace speech = ::google::cloud::speech;
namespace gc = ::google::cloud;

class AudioSource {
 public:
  virtual ~AudioSource() = 0;

  /**
   * The configuration callback.
   *
   * Use this function to return the encoding, sampling rate, expected language,
   * and any other configuration parameters required by Cloud Speech.
   */
  virtual ::google::cloud::speech::v1::RecognitionConfig Config() = 0;

  /**
   * The start of stream callback.
   *
   * @note the stream may be interrupted multiple times during its lifetime, if
   *     you are using an encoding other than `LINEAR16` you may need to resend
   *     any encoding headers in this request.
   *
   * @return the encoded bytes to send with the stream. Note that some encodings
   *   require byte order transformations. If the returned string is empty,
   *   the transcriber will use `Backoff()` to try again.
   */
  virtual std::string StartStream() = 0;

  /**
   * The audio data callback.
   *
   * @return the encoded bytes to send with the stream. Note that some encodings
   *   require byte order transformations. If the returned string is empty,
   *   the transcriber will use `Backoff()` to try again.
   */
  virtual std::string GetAudio() = 0;

  /**
   * Return the time to wait before trying to get more audio.
   *
   * TODO(coryan): consider returning std::variant<> from GetAudio.
   */
  virtual std::chrono::milliseconds Backoff() = 0;
};

class MicrophoneExample : public AudioSource {
 public:
  MicrophoneExample() = default;
  ~MicrophoneExample() override;

  void RunBackground();

  ::google::cloud::speech::v1::RecognitionConfig Config() override;
  std::string StartStream() override;
  std::string GetAudio() override;
  std::chrono::milliseconds Backoff() override;

 private:
  int OnRecord(void* input_buffer, unsigned int buffer_frames);

  static int record_callback(void* /*output_buffer*/, void* input_buffer,
                             unsigned int buffer_frames, double /*stream_time*/,
                             RtAudioStreamStatus /*status*/, void* user_data);

  RtAudio adc_;

  std::mutex mu_;
  std::string buffer_;
};

// Cloud Speech recommends at least 16 Khz sampling. Since we are not using
// any form of compression, it seems better to
auto constexpr kSampleRate = static_cast<unsigned int>(16000);
auto constexpr kBufferTime = std::chrono::seconds(2);

class Transcribe {
 public:
  Transcribe(std::unique_ptr<AudioSource> source, gc::CompletionQueue cq) : source_(std::move(source)), cq_(std::move(cq)) {}

  void Run() {
    auto background =
        std::thread([](gc::CompletionQueue cq) { cq.Run(); }, cq_);

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
  }

  std::unique_ptr<AudioSource> source_;
  gc::CompletionQueue cq_;

  speech::SpeechClient client_ =
      speech::SpeechClient(speech::MakeSpeechConnection(
          gc::Options{}.set<gc::GrpcCompletionQueueOption>(cq_)));

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

MicrophoneExample::~MicrophoneExample()  {
  adc_.stopStream();
  if (adc_.isStreamOpen()) adc_.closeStream();

}

void MicrophoneExample::RunBackground() {
  if (adc_.getDeviceCount() < 1) {
    throw std::runtime_error("No audio devices found");
  }

  RtAudio::StreamParameters parameters;
  parameters.deviceId = adc_.getDefaultInputDevice();
  parameters.nChannels = 1;
  parameters.firstChannel = 0;

  auto buffer_frames = static_cast<unsigned int>(
      std::chrono::seconds(kBufferTime).count() * kSampleRate);
  adc_.openStream(nullptr, &parameters, RTAUDIO_SINT16, kSampleRate,
                  &buffer_frames, &record_callback);

  adc_.startStream();

}

speech::v1::RecognitionConfig MicrophoneExample::Config() {
  speech::v1::RecognitionConfig config;
  config.set_language_code("en-US");
  config.set_encoding(speech::v1::RecognitionConfig::LINEAR16);
  config.set_sample_rate_hertz(kSampleRate);
  return config;
}

std::string MicrophoneExample::StartStream() { return GetAudio(); }

std::string MicrophoneExample::GetAudio() {
  std::lock_guard lk(mu_);
  std::string tmp;
  tmp.swap(buffer_);
  return tmp;
}

std::chrono::milliseconds MicrophoneExample::Backoff() { return kBufferTime; }

int MicrophoneExample::OnRecord(void* input_buffer, unsigned int buffer_frames) {
  auto input = std::span<std::int16_t const>(static_cast<std::int16_t const*>(input_buffer), buffer_frames);
  auto output = std::vector<boost::endian::little_int16_buf_t>(input.size());
  std::copy(input.begin(), input.end(), output.begin());

  std::span<char> bytes(reinterpret_cast<char*>(output.data()), sizeof(output[0]) * output.size());

  std::lock_guard lk(mu_);
  buffer_.append(bytes.begin(), bytes.end());

  return 0;
}

int MicrophoneExample::record_callback(void* /*output_buffer*/, void* input_buffer,
                           unsigned int buffer_frames, double /*stream_time*/,
                           RtAudioStreamStatus /*status*/, void* user_data) {
  return reinterpret_cast<MicrophoneExample*>(user_data)->OnRecord(input_buffer, buffer_frames);
}

