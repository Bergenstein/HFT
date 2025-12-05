#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
struct Stats { uint64_t updates=0, snapshots=0; };

static void consume_msg_object(const json& msg, std::unordered_map<std::string,Stats>& per, uint64_t& l2msgs) {
    if (msg.contains("channel") && msg["channel"].is_string() && msg["channel"] == "l2_data" && msg.contains("events") && msg["events"].is_array()) {
        ++l2msgs;
        for (const auto& ev : msg["events"]) {
            if (!ev.is_object()) continue;
            const std::string pid = ev.value("product_id", "");
            if (pid.empty()) continue;
            auto& s = per[pid];
            const std::string ty = ev.value("type", "update");
            if (ty == "snapshot") s.snapshots++; else s.updates++;
        }
        return;
    }
    if (msg.contains("type") && msg.contains("product_id") && msg["type"].is_string() && msg["product_id"].is_string()) {
        const std::string ty  = msg["type"];
        const std::string pid = msg["product_id"];
        if (ty == "l2update" || ty == "snapshot") {
            ++l2msgs;
            auto& s = per[pid];
            if (ty == "snapshot") s.snapshots++; else s.updates++;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "Usage: " << argv[0] << " <recording.ndjson or ->\n"; return 1; }
    std::istream* pin = nullptr;
    std::ifstream file;
    if (std::string(argv[1]) == "-" || std::string(argv[1]) == "/dev/stdin") {
        pin = &std::cin;
    } else {
        file.open(argv[1], std::ios::in);
        if (!file) { std::cerr << "Error: cannot open " << argv[1] << "\n"; return 1; }
        pin = &file;
    }
    std::istream& in = *pin;
    std::unordered_map<std::string, Stats> per;
    uint64_t total=0, parsed=0, l2msgs=0;
    std::string line;
    while (std::getline(in, line)) {
        ++total;
        auto brace = line.find('{');
        if (brace == std::string::npos) continue;
        try {
            json outer = json::parse(line.substr(brace), nullptr, true);
            ++parsed;
            if (outer.contains("raw") && outer["raw"].is_string()) {
                const std::string raw = outer["raw"].get<std::string>();
                try {
                    json inner = json::parse(raw, nullptr, true);
                    if (inner.is_object()) consume_msg_object(inner, per, l2msgs);
                } catch (...) { }
                continue;
            }
            if (outer.is_object()) consume_msg_object(outer, per, l2msgs);
        } catch (...) {
        }
    }
    std::vector<std::pair<std::string, Stats>> v(per.begin(), per.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b){
        return (a.second.updates != b.second.updates) ? a.second.updates > b.second.updates
                                                      : a.first < b.first;
    });
    std::cout << "lines="<<total<<" parsed="<<parsed<<" l2_msgs="<<l2msgs<<" products="<<v.size() << "\n";
    std::cout << "Top products by updates:\n";
    for (size_t i=0; i<std::min<size_t>(50, v.size()); ++i) {
        std::cout << "  " << v[i].first
                  << "\tupdates=" << v[i].second.updates
                  << "\tsnapshots=" << v[i].second.snapshots << "\n";
    }
    return 0;
}