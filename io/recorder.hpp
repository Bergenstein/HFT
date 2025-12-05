#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

class Recorder {
public:
  Recorder(const std::string& out_dir, int ws_idx)
  : ws_idx_(ws_idx) {
    namespace fs = std::filesystem;
    if (!fs::exists(out_dir)) fs::create_directories(out_dir);
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::gmtime(&t));
    file_path_ = out_dir + "/raw_" + std::string(buf) + "_ws" + std::to_string(ws_idx_) + ".ndjson";
    out_.open(file_path_, std::ios::out | std::ios::binary);
  }
  ~Recorder() { if (out_.is_open()) out_.flush(), out_.close(); }
  void append(const std::string& raw_msg, long long ts_ns) {
    nlohmann::json line = {
      {"ts_recv_ns", ts_ns},
      {"ws_idx", ws_idx_},
      {"raw", raw_msg}
    };
    std::lock_guard<std::mutex> lk(mu_);
    out_ << line.dump() << '\n';
  }
  const std::string& path() const { return file_path_; }

private:
  int ws_idx_;
  std::string file_path_;
  std::ofstream out_;
  std::mutex mu_;
};