#include <iostream>
#include <thread>
#include <vector>
#include <algorithm>
#include "../discovery/discovery.hpp"
#include "../md/ws_l2_client.hpp"

static void launch_batched(const std::vector<std::string>& product_ids,
                           std::size_t BATCH,
                           const std::string& data_dir) {
  std::vector<std::thread> threads;
  int idx = 0;
  for (std::size_t i = 0; i < product_ids.size(); i += BATCH) {
    std::size_t j = std::min(i + BATCH, product_ids.size());
    std::vector<std::string> batch(product_ids.begin()+i, product_ids.begin()+j);
    threads.emplace_back([batch=std::move(batch), idx, data_dir]() mutable {
      try {
        WsL2Client c(idx, std::move(batch), data_dir);
        c.run();
      } catch (const std::exception& e) {
        std::cerr << "[WS#" << idx << "] Error: " << e.what() << "\n";
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ++idx;
  }
  for (auto& t : threads) t.join();
}

int main(int argc, char** argv) {
  const std::size_t TOPN  = 50;
  const std::size_t BATCH = 20;
  const std::string DATA_DIR = "data";
  
  std::cerr << "Fetching /products ...\n";
  auto tradable = Discovery::tradable_products();
  std::cerr << "Tradable count = " << tradable.size() << "\n";
  
  std::cerr << "Fetching /products/volume-summary ...\n";
  auto vol = Discovery::volume_24h();
  
  auto top = Discovery::top_by_volume(tradable, vol, TOPN);
  if (top.empty()) {
    top = {
      "BTC-USD","ETH-USD","SOL-USD","XRP-USD","DOGE-USD",
      "ADA-USD","AVAX-USD","LINK-USD","BCH-USD","TRX-USD",
      "NEAR-USD","APT-USD","TON-USD","SUI-USD","ARB-USD",
      "OP-USD","MATIC-USD","LTC-USD","ATOM-USD","ICP-USD",
      "HBAR-USD","ALGO-USD","FIL-USD","ETC-USD","AAVE-USD",
      "UNI-USD","XLM-USD","VET-USD","EOS-USD","FTM-USD",
      "RNDR-USD","IMX-USD","PEPE-USD","JUP-USD","SEI-USD",
      "PYTH-USD","TIA-USD","BONK-USD","ENA-USD","WIF-USD",
      "JASMY-USD","SHIB-USD","GALA-USD","SAND-USD","INJ-USD",
      "APT-USDT","AVAX-USDT","SOL-USDT","BTC-USDT","ETH-USDT"
    };
  }
  
  std::cerr << "Top " << top.size() << " to subscribe:\n";
  for (auto& pid : top) std::cerr << "  " << pid << "\n";
  
  launch_batched(top, BATCH, DATA_DIR);
  return 0;
}