#include <iostream>
#include <fstream>
#include <unordered_map>
#include <optional>
#include <nlohmann/json.hpp>
#include "../md/normalizer.hpp"
#include "../core/order_book.hpp"

// OrderBook wrapper that works with L2Snapshot and L2Update
class ReplayOrderBook {
public:
    void on_snapshot(const L2Snapshot& snap) {
        book_.clear();
        for (const auto& [px, qty] : snap.bids) {
            book_.set_level(true, px, qty);
        }
        for (const auto& [px, qty] : snap.asks) {
            book_.set_level(false, px, qty);
        }
        last_seq_ = snap.seq;
    }
    
    void on_update(const L2Update& upd) {
        book_.set_level(upd.is_bid, upd.price, upd.qty);
        last_seq_ = upd.seq;
    }
    
    std::optional<double> best_bid() const {
        auto bb = book_.best_bid();
        return bb ? std::optional<double>(bb->first) : std::nullopt;
    }
    
    std::optional<double> best_ask() const {
        auto ba = book_.best_ask();
        return ba ? std::optional<double>(ba->first) : std::nullopt;
    }
    
    uint64_t last_seq() const { return last_seq_; }
    
private:
    core::OrderBook book_;
    uint64_t last_seq_ = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <ndjson_file>\n";
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "Cannot open " << argv[1] << "\n"; return 1; }
    
    std::unordered_map<std::string, ReplayOrderBook> books;
    struct Tob { double bid=0, ask=0; bool have=false; };
    std::unordered_map<std::string, Tob> last;
    
    std::string line;
    size_t n_lines = 0, n_updates = 0, n_snaps = 0;
    
    while (std::getline(in, line)) {
        ++n_lines;
        nlohmann::json rec = nlohmann::json::parse(line, nullptr, false);
        if (rec.is_discarded() || !rec.contains("raw")) continue;
        
        std::vector<L2Snapshot> snaps;
        std::vector<L2Update> ups;
        Normalizer::parse(rec["raw"].get<std::string>(), snaps, ups);
        
        for (auto& s : snaps) {
            books[s.product].on_snapshot(s);
            ++n_snaps;
        }
        for (auto& u : ups) {
            auto& ob = books[u.product];
            ob.on_update(u);
            ++n_updates;
            auto bb = ob.best_bid();
            auto ba = ob.best_ask();
            if (bb && ba) {
                auto& t = last[u.product];
                if (!t.have || t.bid != *bb || t.ask != *ba) {
                    t.have = true; t.bid = *bb; t.ask = *ba;
                    std::cout << u.product << " bb=" << *bb << " ba=" << *ba << " seq=" << ob.last_seq() << "\n";
                }
            }
        }
    }
    std::cerr << "Done. lines=" << n_lines << " snaps=" << n_snaps << " updates=" << n_updates << "\n";
    return 0;
}