// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <boost/program_options.hpp>
#include <google/cloud/storage/client.h>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {
namespace po = boost::program_options;
namespace gcs = google::cloud::storage;
std::tuple<po::variables_map, po::options_description> parse_command_line(
    int argc, char* argv[]);
}  // namespace

int main(int argc, char* argv[]) try {
  auto [vm, desc] = parse_command_line(argc, argv);
  auto help = [d = std::move(desc)](po::variables_map const&) {
    std::cout << "Usage: " << d << "\n";
    return 0;
  };
  if (vm.count("help")) return help(vm);

  for (std::string opt : {"bucket", "object", "destination"}) {
    if (vm.count(opt) != 0) continue;
    if (not vm[opt].as<std::string>().empty()) continue;
    throw std::runtime_error("the --" + opt + " option cannot be empty");
  }
  auto const bucket = vm["bucket"].as<std::string>();
  auto const object = vm["object"].as<std::string>();
  auto const destination = vm["destination"].as<std::string>();
  auto const desired_thread_count = vm["thread-count"].as<int>();
  if (desired_thread_count == 0) {
    throw std::runtime_error("the --thread-count option cannot be zero");
  }
  auto const minimum_slice_size = vm["minimum-slice-size"].as<std::int64_t>();

  auto client = gcs::Client::CreateDefaultClient().value();
  auto metadata = client.GetObjectMetadata(bucket, object).value();

  auto [slice_size, thread_count] = [&]() -> std::pair<std::int64_t, int> {
    auto const thread_slice = metadata.size() / desired_thread_count;
    if (thread_slice >= minimum_slice_size) {
      return {thread_slice, desired_thread_count};
    }
    auto const threads = metadata.size() / minimum_slice_size;
    if (threads == 0) {
      return {metadata.size(), 1};
    }
    return {minimum_slice_size, static_cast<int>(threads)};
  }();

  std::cout << "Downloading " << object << " from bucket " << bucket
            << " to file " << destination << "\nThis object has "
            << metadata.size() << " bytes, it will be downloaded in "
            << thread_count << " slices, each approximately " << slice_size
            << " bytes long" << std::endl;

  auto task = [](std::int64_t offset, std::int64_t length,
                 std::string const& bucket, std::string const& object,
                 std::string const& destination) {
    auto client = gcs::Client::CreateDefaultClient().value();
    auto is = client.ReadObject(bucket, object,
                                gcs::ReadRange(offset, offset + length));

    std::ofstream os(destination, std::ios::binary | std::ios::app);
    os.seekp(offset, std::ios::beg);
    std::vector<char> buffer(1024 * 1024L);
    do {
      is.read(buffer.data(), buffer.size());
      if (is.bad()) break;
      os.write(buffer.data(), is.gcount());
    } while (not is.eof());
  };

  auto const start = std::chrono::steady_clock::now();
  std::vector<std::future<void>> tasks;
  for (std::int64_t offset = 0; offset < metadata.size();
       offset += slice_size) {
    auto const current_slice_size =
        std::min<std::int64_t>(slice_size, metadata.size() - offset);
    tasks.push_back(std::async(std::launch::async, task, offset,
                               current_slice_size, bucket, object,
                               destination));
  }

  for (auto& t : tasks) t.get();

  std::cout << "Download completed\n";

  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard C++ exception thrown: " << ex.what() << std::endl;
  return 1;
} catch (...) {
  std::cerr << "Unknown C++ exception thrown" << std::endl;
  return 1;
}

namespace {
std::tuple<po::variables_map, po::options_description> parse_command_line(
    int argc, char* argv[]) {
  auto const default_minimum_slice_size = 64 * 1024 * 1024L;
  auto const default_thread_count = [] {
    auto constexpr kFallbackThreadCount = 2;
    auto constexpr kThreadsPerCore = 2;
    auto const count = std::thread::hardware_concurrency();
    if (count == 0) return kFallbackThreadCount;
    return static_cast<int>(count * kThreadsPerCore);
  }();

  po::positional_options_description positional;
  positional.add("bucket", 1);
  positional.add("object", 1);
  positional.add("destination", 1);
  po::options_description desc(
      "Download a single GCS object using multiple slices");
  desc.add_options()("help", "produce help message")
      //
      ("bucket", po::value<std::string>(),
       "set the GCS bucket to download from")
      //
      ("object", po::value<std::string>(),
       "set the GCS object to download from")
      //
      ("destination", po::value<std::string>(),
       "set the destination file to download into")
      //
      ("thread-count", po::value<int>()->default_value(default_thread_count),
       "number of parallel handlers to handle work items")
      //
      ("minimum-slice-size",
       po::value<std::int64_t>()->default_value(default_minimum_slice_size),
       "minimum slice size");

  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
                .options(desc)
                .positional(positional)
                .run(),
            vm);
  po::notify(vm);
  return {vm, desc};
}

}  // namespace