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
#include <fmt/format.h>
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
  virtual ~AudioSource() = default;

  /**
   * The configuration callback.
   *
   * Use this function to return the encoding, sampling rate, expected language,
   * and any other configuration parameters required by Cloud Speech.
   *
   * @note the stream may be interrupted multiple times during its lifetime. If
   *     the encoding requires it, the next call to `GetAudio()` needs to
   *     include any encoding header data.
   */
  virtual ::google::cloud::speech::v1::StreamingRecognitionConfig Config() = 0;

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
  MicrophoneExample();
  ~MicrophoneExample() override;

  void RunBackground();

  ::google::cloud::speech::v1::StreamingRecognitionConfig Config() override;
  std::string GetAudio() override;
  std::chrono::milliseconds Backoff() override;

 private:
  int OnRecord(void* input_buffer, unsigned int buffer_frames);

  static int record_callback(void* /*output_buffer*/, void* input_buffer,
                             unsigned int buffer_frames, double /*stream_time*/,
                             RtAudioStreamStatus /*status*/, void* user_data);

  RtAudio adc_;
  unsigned int sample_rate_ = 0;

  std::mutex mu_;
  std::string buffer_;
};

// Cloud Speech recommends at least 16 Khz sampling. Since we are not using
// any form of compression, it seems we should use the minimum recommended rate
// to save bandwidth.
auto constexpr kMinimumSampleRate = static_cast<unsigned int>(16000);
auto constexpr kBufferTime = std::chrono::seconds(1);
auto constexpr kInitialBackoff = std::chrono::milliseconds(500);

class Transcribe {
 public:
  Transcribe(std::unique_ptr<AudioSource> source, gc::CompletionQueue cq,
             speech::SpeechClient client)
      : source_(std::move(source)),
        cq_(std::move(cq)),
        client_(std::move(client)) {}

  gc::future<void> Run() {
    StartRecognitionStream(kInitialBackoff);
    return done_.get_future();
  }

 private:
  using RecognitionStream =
      gc::AsyncStreamingReadWriteRpc<speech::v1::StreamingRecognizeRequest,
                                     speech::v1::StreamingRecognizeResponse>;

  void StartRecognitionStream(std::chrono::milliseconds backoff) {
    auto stream = client_.AsyncStreamingRecognize();
    auto start = stream->Start();
    start.then([this, backoff, s = std::move(stream)](auto f) mutable {
      this->OnStart(std::move(s), backoff, f.get());
    });
  }

  void OnStart(std::unique_ptr<RecognitionStream> s,
               std::chrono::milliseconds backoff, bool ok) {
    if (!ok) {
      s->Finish().then(
          [this, backoff](auto f) { return OnStartError(f.get(), backoff); });
      return;
    }
    std::unique_lock lk(mu_);
    stream_ = std::move(s);
    auto stream = PrepareWrite(std::move(lk));
    if (!stream) return;

    speech::v1::StreamingRecognizeRequest request;
    *request.mutable_streaming_config() = source_->Config();
    stream->Write(request, grpc::WriteOptions()).then([this](auto f) {
      OnWriteConfig(f.get());
    });
  }

  void OnStartError(gc::Status const& status,
                    std::chrono::milliseconds backoff) {
    if (status.code() != gc::StatusCode::kUnavailable) {
      std::cerr << "Unrecoverable error starting recognition stream: " << status
                << "\n";
      return Shutdown(std::unique_lock(mu_));
    }
    cq_.MakeRelativeTimer(backoff).then([this, b = 2 * backoff](auto f) {
      auto status = f.get().status();
      if (status.ok()) return OnTimerError(std::move(status));
      StartRecognitionStream(b);
    });
  }

  void OnTimerError(gc::Status const& status) {
    std::cerr << "Unrecoverable error in backoff timer: " << status << "\n";
    return Shutdown(std::unique_lock(mu_));
  }

  void OnWriteConfig(bool ok) {
    std::unique_lock lk(mu_);
    pending_write_ = false;
    if (!ok) return Reset(std::move(lk));
    auto stream = PrepareRead(std::move(lk));
    if (!stream) return;

    stream->Read().then([this](auto f) { OnRead(f.get()); });
    WriteAudio();
  }

  void OnWriteBackoff(gc::Status const& status) {
    if (!status.ok()) return Reset(std::unique_lock(mu_));
    WriteAudio();
  }

  void WriteAudio() {
    auto audio = source_->GetAudio();
    if (audio.empty()) {
      cq_.MakeRelativeTimer(source_->Backoff()).then([this](auto f) {
        OnWriteBackoff(f.get().status());
      });
      return;
    }
    std::unique_lock lk(mu_);
    sample_count_ += audio.size();
    auto stream = PrepareWrite(std::move(lk));
    if (!stream) return;

    speech::v1::StreamingRecognizeRequest request;
    request.set_audio_content(std::move(audio));
    stream->Write(request, grpc::WriteOptions()).then([this](auto f) {
      OnWrite(f.get());
    });
  }

  void OnRead(absl::optional<speech::v1::StreamingRecognizeResponse> response) {
    std::unique_lock lk(mu_);
    pending_read_ = false;
    if (!response.has_value()) return Reset(std::move(lk));
    auto count = sample_count_;
    sample_count_ = 0;

    for (auto const& r : response->results()) {
      if (r.alternatives().empty()) continue;
      for (auto const& a : r.alternatives()) {
        std::cout << "[" << count << ":" << a.confidence() << "] "
                  << a.transcript() << "\n";
        auto cmd = a.transcript();
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](auto c) {
          return static_cast<char>(std::tolower(c));
        });
        auto l = cmd.find("stop stop stop");
        if (l != std::string::npos && a.confidence() > 0.90) {
          std::cerr << "*** stopping " << std::endl;
          return Shutdown(std::move(lk));
        } else {
          std::cout << "DEBUG: " << l << "\n";
        }
      }
    }

    auto stream = PrepareRead(std::move(lk));
    if (!stream) return;
    stream->Read().then([this](auto f) { OnRead(f.get()); });
  }

  void OnWrite(bool ok) {
    {
      std::unique_lock lk(mu_);
      pending_write_ = false;
      if (!ok) return Reset(std::move(lk));
    }
    WriteAudio();
  }

  void Reset(std::unique_lock<std::mutex> lk) {
    std::cout << "DEBUG: Reset() - pending_read=" << pending_read_
              << " pending_write=" << pending_write_ << std::endl;
    if (pending_write_ or pending_read_) return;
    auto stream = std::move(stream_);
    lk.unlock();
    if (!stream) return;
    stream->Finish().then([this](auto f) { OnReset(f.get()); });
  }

  void OnReset(gc::Status const& status) {
    std::unique_lock lk(mu_);
    if (shutdown_) {
      done_.set_value();
      return;
    }
    lk.unlock();
    if (status.code() != gc::StatusCode::kUnavailable) {
      // TODO(coryan) - consider ignoring all errors after a successful
      //     read+write.
      std::cerr << "Unrecoverable error during read/write " << status << "\n";
    }
    std::cout << "Recovering from connection reset " << status << "\n";
    return StartRecognitionStream(kInitialBackoff);
  }

  void Shutdown(std::unique_lock<std::mutex> lk) {
    std::cout << "DEBUG: Shutdown() - pending_read=" << pending_read_
              << " pending_write=" << pending_write_ << std::endl;
    shutdown_ = true;
    Reset(std::move(lk));
  }

  std::shared_ptr<RecognitionStream> PrepareRead(
      std::unique_lock<std::mutex> lk) {
    if (shutdown_) {
      Reset(std::move(lk));
      return {};
    }
    pending_read_ = true;
    return stream_;
  }

  std::shared_ptr<RecognitionStream> PrepareWrite(
      std::unique_lock<std::mutex> lk) {
    if (shutdown_) {
      Reset(std::move(lk));
      return {};
    }
    pending_write_ = true;
    return stream_;
  }

  std::unique_ptr<AudioSource> source_;
  gc::CompletionQueue cq_;
  gc::promise<void> done_;

  speech::SpeechClient client_;

  std::mutex mu_;
  std::shared_ptr<RecognitionStream> stream_;
  bool pending_read_ = false;
  bool pending_write_ = false;
  bool shutdown_ = false;
  std::uint64_t sample_count_ = 0;
};

int main(int argc, char* argv[]) try {
  if (argc > 1) {
    std::cerr << "Usage: " << argv[0] << "\n";
    return 1;
  }

  auto mic = std::make_unique<MicrophoneExample>();
  mic->RunBackground();

  gc::CompletionQueue cq;
  auto background = std::thread([](auto cq) { cq.Run(); }, cq);

  auto client = speech::SpeechClient(speech::MakeSpeechConnection(
      gc::Options{}.set<gc::GrpcCompletionQueueOption>(cq)));
  Transcribe transcribe(std::move(mic), cq, client);

  auto done = transcribe.Run();

  done.get();

  cq.Shutdown();
  background.join();

  return 0;
} catch (RtAudioError& e) {
  std::cerr << "RtAudioError thrown ";
  e.printMessage();
  return 1;
} catch (std::exception const& ex) {
  std::cerr << "Standard exception raised: " << ex.what() << "\n";
  return 1;
}

MicrophoneExample::MicrophoneExample() {
  if (adc_.getDeviceCount() < 1) {
    throw std::runtime_error("No audio devices found");
  }

  auto info = adc_.getDeviceInfo(adc_.getDefaultInputDevice());
  std::cout << "Using audio device " << info.name << " supported formats ";
  struct Format {
    std::string name;
    RtAudioFormat format;
  } formats[] = {
      {"RTAUDIO_SINT8", RTAUDIO_SINT8},
      {"RTAUDIO_SINT16", RTAUDIO_SINT16},
      {"RTAUDIO_SINT24", RTAUDIO_SINT24},
      {"RTAUDIO_SINT32", RTAUDIO_SINT32},
      {"RTAUDIO_FLOAT32", RTAUDIO_FLOAT32},
      {"RTAUDIO_FLOAT64", RTAUDIO_FLOAT64},
  };
  for (auto const& f : formats) {
    if ((info.nativeFormats & f.format) == 0) continue;
    std::cout << " " << f.name;
  }
  std::cout << std::endl;
  //  if ((info.nativeFormats & RTAUDIO_SINT16) == 0) {
  //    throw std::runtime_error(fmt::format(
  //        "The device ({}) does not support LINEAR16 sampling", info.name));
  //  }
  auto rates = info.sampleRates;
  std::sort(rates.begin(), rates.end());
  auto loc = std::find_if(rates.begin(), rates.end(),
                          [](auto rate) { return rate >= kMinimumSampleRate; });
  if (loc == rates.end()) {
    throw std::runtime_error(
        fmt::format("The device ({}) supported sampling rates are all below {}",
                    info.name, kMinimumSampleRate));
  }
  sample_rate_ = *loc;
  std::cout << "Sampling at " << sample_rate_ << std::endl;

  RtAudio::StreamParameters parameters;
  parameters.deviceId = adc_.getDefaultInputDevice();
  parameters.nChannels = 1;
  parameters.firstChannel = 0;

  auto buffer_frames = static_cast<unsigned int>(
      std::chrono::seconds(kBufferTime).count() * sample_rate_);
  adc_.openStream(nullptr, &parameters, RTAUDIO_SINT16, sample_rate_,
                  &buffer_frames, &record_callback, this);
}

MicrophoneExample::~MicrophoneExample() try {
  adc_.stopStream();
  if (adc_.isStreamOpen()) adc_.closeStream();
} catch (RtAudioError const& e) {
  e.printMessage();
}

void MicrophoneExample::RunBackground() { adc_.startStream(); }

speech::v1::StreamingRecognitionConfig MicrophoneExample::Config() {
  speech::v1::StreamingRecognitionConfig config;
  auto& cfg = *config.mutable_config();
  cfg.set_language_code("en-US");
  cfg.set_encoding(speech::v1::RecognitionConfig::LINEAR16);
  cfg.set_sample_rate_hertz(static_cast<std::int32_t>(sample_rate_));
  return config;
}

std::string MicrophoneExample::GetAudio() {
  std::lock_guard lk(mu_);
  std::string tmp;
  tmp.swap(buffer_);
  return tmp;
}

std::chrono::milliseconds MicrophoneExample::Backoff() { return kBufferTime; }

int MicrophoneExample::OnRecord(void* input_buffer,
                                unsigned int buffer_frames) {
  auto input = std::span<std::int16_t const>(
      static_cast<std::int16_t const*>(input_buffer), buffer_frames);
  auto output = std::vector<boost::endian::little_int16_buf_t>(input.size());
  std::copy(input.begin(), input.end(), output.begin());

  std::span<char> bytes(reinterpret_cast<char*>(output.data()),
                        sizeof(output[0]) * output.size());

  std::lock_guard lk(mu_);
  buffer_.append(bytes.begin(), bytes.end());

  return 0;
}

int MicrophoneExample::record_callback(
    void* /*output_buffer*/, void* input_buffer, unsigned int buffer_frames,
    double /*stream_time*/, RtAudioStreamStatus /*status*/, void* user_data) {
  return reinterpret_cast<MicrophoneExample*>(user_data)->OnRecord(
      input_buffer, buffer_frames);
}
