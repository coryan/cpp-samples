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

#include <boost/algorithm/string.hpp>
#include <boost/endian/buffers.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/program_options.hpp>
#include <cppcodec/base64_rfc4648.hpp>
#include <crc32c/crc32c.h>
#include <fmt/format.h>
#include <google/cloud/storage/client.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <numeric>
#include <regex>
#include <string>
#include <thread>
#include <utility>
// Posix headers last.
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

namespace {
namespace po = boost::program_options;
namespace gcs = google::cloud::storage;
po::variables_map parse_command_line(int argc, char* argv[]);
std::string format_size(std::int64_t size);

auto constexpr kKiB = std::int64_t(1024);
auto constexpr kMiB = 1024 * kKiB;
auto constexpr kGiB = 1024 * kMiB;
auto constexpr kTiB = 1024 * kGiB;
auto constexpr kPiB = 1024 * kTiB;
}  // namespace

int main(int argc, char* argv[]) try {
  auto vm = parse_command_line(argc, argv);

  auto const urls = vm["urls"].as<std::string>();
  auto const destination_directory = vm["destination"].as<std::string>();
  auto const desired_thread_count = vm["thread-count"].as<int>();
  auto const minimum_slice_size = vm["minimum-slice-size"].as<std::int64_t>();

  auto client = gcs::Client::CreateDefaultClient().value();

  // Split the input urls into individual URLs and iterate
  std::vector<std::string> split_urls;
  boost::split(split_urls, urls, boost::is_any_of(",\n"));

  std::regex bkt_obj_rgx("gs://([^/]*)/(.*)");

  for (std::string url : split_urls) {
    // parse the bucket and object from the URL
    auto [bucket, object] = [&]() -> std::pair<std::string, std::string> {
      std::smatch match;
      std::regex_search(url, match, bkt_obj_rgx);
      return {match[1], match[2]};
    }();

    // compute full destination path based on object name
    // TODO: Better portability with Boost::filesystem
    std::string destination = (std::string)destination_directory +=
        std::string("/") += boost::replace_all_copy(object, "/", "_");

    auto metadata = client.GetObjectMetadata(bucket, object).value();

    // compute the slice size and thread count, taking limits into account
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
              << " to file " << destination << "\n";
    std::cout << "This object size is approximately "
              << format_size(metadata.size()) << ". It will be downloaded in "
              << thread_count << " slices, each approximately "
              << format_size(slice_size) << " in size." << std::endl;

    auto check_system_call = [](std::string const& op, int r) {
      if (r >= 0) return r;
      auto err = errno;
      throw std::runtime_error(
          fmt::format("Error in {}() - return value={}, error=[{}] {}", op, r,
                      err, strerror(err)));
    };
    auto task = [&](std::int64_t offset, std::int64_t length,
                    std::string const& bucket, std::string const& object,
                    int fd) {
      auto client = gcs::Client::CreateDefaultClient().value();
      auto is = client.ReadObject(bucket, object,
                                  gcs::ReadRange(offset, offset + length));

      std::vector<char> buffer(1024 * 1024L);
      std::int64_t count = 0;
      std::int64_t write_offset = offset;
      do {
        is.read(buffer.data(), buffer.size());
        if (is.bad()) break;
        count += is.gcount();
        check_system_call(
            "pwrite()", ::pwrite(fd, buffer.data(), is.gcount(), write_offset));
        write_offset += is.gcount();
      } while (not is.eof());
      return fmt::format("Download range [{}, {}] got {}/{} bytes", offset,
                         offset + length, count, length);
    };

    auto const start = std::chrono::steady_clock::now();
    std::vector<std::future<std::string>> tasks;
    auto constexpr kOpenFlags = O_CREAT | O_TRUNC | O_WRONLY;
    auto constexpr kOpenMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    auto const fd = check_system_call(
        "open()", ::open(destination.c_str(), kOpenFlags, kOpenMode));
    for (std::int64_t offset = 0; offset < metadata.size();
         offset += slice_size) {
      auto const current_slice_size =
          std::min<std::int64_t>(slice_size, metadata.size() - offset);
      tasks.push_back(std::async(std::launch::async, task, offset,
                                 current_slice_size, bucket, object, fd));
    }

    for (auto& t : tasks) {
      std::cout << t.get() << "\n";
    }
    check_system_call("close(fd)", ::close(fd));

    auto const end = std::chrono::steady_clock::now();
    auto const elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    auto const elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    auto const effective_bandwidth_MiBs =
        (static_cast<double>(metadata.size()) / kMiB) /
        (elapsed_us.count() / 1'000'000.0);
    std::cout << "Download completed in " << elapsed_ms.count() << "ms\n"
              << "Effective bandwidth " << effective_bandwidth_MiBs
              << " MiB/s\n";

    auto downloaded_file_info = [](std::string const& filename) {
      std::ifstream is(filename);
      std::vector<char> buffer(1024 * 1024L);
      std::uint32_t crc32c = 0;
      std::int64_t size = 0;
      do {
        is.read(buffer.data(), buffer.size());
        if (is.bad()) break;
        crc32c = crc32c::Extend(crc32c,
                                reinterpret_cast<std::uint8_t*>(buffer.data()),
                                is.gcount());
        size += is.gcount();
      } while (!is.eof());

      static_assert(std::numeric_limits<unsigned char>::digits == 8,
                    "This program assumes an 8-bit char");
      boost::endian::big_uint32_buf_at buf(crc32c);
      return std::make_pair(size, cppcodec::base64_rfc4648::encode(std::string(
                                      buf.data(), buf.data() + sizeof(buf))));
    };

    auto [size, crc32c] = downloaded_file_info(destination);
    if (size != metadata.size()) {
      std::cerr << "Downloaded file size mismatch, expected=" << metadata.size()
                << ", got=" << size << std::endl;
      return 1;
    }

    if (crc32c != metadata.crc32c()) {
      std::cerr << "Download file CRC32C mismatch, expected="
                << metadata.crc32c() << ", got=" << crc32c << std::endl;
      return 1;
    }

    std::cout << "File size and CRC32C match expected values" << std::endl;
  }
  return 0;
} catch (std::exception const& ex) {
  std::cerr << "Standard C++ exception thrown: " << ex.what() << std::endl;
  return 1;
} catch (...) {
  std::cerr << "Unknown C++ exception thrown" << std::endl;
  return 1;
}

namespace {
char const* kPositional[] = {"urls", "destination"};

[[noreturn]] void usage(std::string const& argv0,
                        po::options_description const& desc,
                        std::string const& message = {}) {
  auto exit_status = EXIT_SUCCESS;
  if (!message.empty()) {
    exit_status = EXIT_FAILURE;
    std::cout << "Error: " << message << "\n";
  }

  // format positional args
  auto const positional_names =
      std::accumulate(std::begin(kPositional), std::end(kPositional),
                      std::string{" [options]"}, [](auto a, auto const& b) {
                        a += ' ';
                        a += b;
                        return a;
                      });

  // print usage + options help, and exit normally
  std::cout << "usage: " << argv0 << positional_names << "\n\n" << desc << "\n";
  std::exit(exit_status);
}

po::variables_map parse_command_line(int argc, char* argv[]) {
  auto const default_minimum_slice_size = 64 * 1024 * 1024L;
  auto const default_thread_count = [] {
    auto constexpr kFallbackThreadCount = 2;
    auto constexpr kThreadsPerCore = 2;
    auto const count = std::thread::hardware_concurrency();
    if (count == 0) return kFallbackThreadCount;
    return static_cast<int>(count * kThreadsPerCore);
  }();

  po::positional_options_description positional;
  for (auto const* name : kPositional) positional.add(name, 1);
  po::options_description desc(
      "Download a single GCS object using multiple slices");
  desc.add_options()("help", "produce help message")
      //
      ("urls", po::value<std::string>()->required(),
       "a comma or newline separated list of gs:// object URLs")
      //
      ("destination", po::value<std::string>()->required(),
       "the destination directory to download into")
      //
      ("thread-count", po::value<int>()->default_value(default_thread_count),
       "number of parallel handlers to handle work items")
      //
      ("minimum-slice-size",
       po::value<std::int64_t>()->default_value(default_minimum_slice_size),
       "minimum slice size");

  // parse the arguments
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
                .options(desc)
                .positional(positional)
                .run(),
            vm);
  if (vm.count("help") != 0) usage(argv[0], desc);
  try {
    po::notify(vm);
  } catch (std::exception const& ex) {
    // if no arguments were given at all, print usage
    if (argc == 1) usage(argv[0], desc, ex.what());
  }

  for (std::string opt : kPositional) {
    if (not vm[opt].as<std::string>().empty()) continue;
    usage(argv[0], desc, fmt::format("the {} argument cannot be empty", opt));
  }

  if (vm["thread-count"].as<int>() == 0) {
    usage(argv[0], desc, "the --thread-count option cannot be zero");
  }
  if (vm["minimum-slice-size"].as<std::int64_t>() == 0) {
    usage(argv[0], desc, "the --minimum-slice-size option cannot be zero");
  }

  return vm;
}

std::string format_size(std::int64_t size) {
  struct range_definition {
    std::int64_t max_value;
    std::int64_t scale;
    char const* units;
  } ranges[] = {
      {kKiB, 1, "Bytes"},  {kMiB, kKiB, "KiB"}, {kGiB, kMiB, "MiB"},
      {kTiB, kGiB, "GiB"}, {kPiB, kTiB, "TiB"},
  };
  for (auto d : ranges) {
    if (size < d.max_value) return std::to_string(size / d.scale) + d.units;
  }
  return std::to_string(size / kPiB) + "PiB";
}

}  // namespace
