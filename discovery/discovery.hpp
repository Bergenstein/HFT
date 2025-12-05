#pragma once
#include <string>
#include <vector>
#include <unordered_map>

struct Discovery {
  static std::vector<std::string> tradable_products();
  static std::unordered_map<std::string,double> volume_24h();
  static std::vector<std::string> top_by_volume(const std::vector<std::string>& universe,
                                                const std::unordered_map<std::string,double>& vol,
                                                std::size_t N);
};