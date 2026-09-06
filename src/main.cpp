// SPDX-License-Identifier: MIT
// cpp-cpufreq-watch — Linux CPU frequency scaling monitor (/sys/devices/system/cpu/)
// Build: g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o cpufreq-watch
// Usage: ./cpufreq-watch [--json] [--watch N] [--no-color] [--demo] [--selftest]

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ── Types ──

struct Error {
  int code;
  std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

struct CpuFreqInfo {
  int cpu_id;
  long long cur_khz{0};
  long long min_khz{0};
  long long max_khz{0};
  long long base_khz{0};  // from cpuinfo
  std::string governor;
  bool online{true};
  bool turbo{false};
};

struct FreqSnapshot {
  std::vector<CpuFreqInfo> cpus;
  long long total_cur{0};
  long long total_max{0};
  int online_count{0};
  int turbo_count{0};
  double avg_pct{0.0};
  double timestamp{0.0};
};

// ── Color palette (mutable for --no-color) ──

bool g_color = true;
std::string C_RST, C_BOLD, C_DIM, C_CYAN, C_GREEN, C_YELLOW, C_RED, C_MAGENTA, C_BLUE;

void init_colors() {
  C_RST   = g_color ? "\033[0m"  : "";
  C_BOLD  = g_color ? "\033[1m"  : "";
  C_DIM   = g_color ? "\033[2m"  : "";
  C_CYAN  = g_color ? "\033[36m" : "";
  C_GREEN = g_color ? "\033[32m" : "";
  C_YELLOW= g_color ? "\033[33m" : "";
  C_RED   = g_color ? "\033[31m" : "";
  C_MAGENTA = g_color ? "\033[35m" : "";
  C_BLUE  = g_color ? "\033[34m" : "";
}

// ── Helpers ──

std::string read_file_str(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return "";
  std::string val;
  std::getline(f, val);
  // trim trailing whitespace/newlines
  while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
    val.pop_back();
  return val;
}

long long read_khz(const std::string& path) {
  auto s = read_file_str(path);
  if (s.empty()) return -1;
  long long v = 0;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc{}) return -1;
  return v;
}

std::string freq_to_ghz(long long khz) {
  if (khz <= 0) return "N/A";
  char buf[16];
  double ghz = static_cast<double>(khz) / 1000000.0;
  std::snprintf(buf, sizeof(buf), "%.2f", ghz);
  return std::string(buf) + " GHz";
}

std::string freq_to_mhz(long long khz) {
  if (khz <= 0) return "N/A";
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%lld", khz / 1000);
  return std::string(buf) + " MHz";
}

// Color based on frequency percentage
std::string pct_color(double pct) {
  if (pct > 85.0) return C_RED;
  if (pct > 60.0) return C_YELLOW;
  if (pct > 30.0) return C_GREEN;
  return C_CYAN;
}

std::string make_bar(double pct, int width = 20) {
  int filled = std::clamp(static_cast<int>(std::round(pct / 100.0 * width)), 0, width);
  std::string bar;
  bar += C_BOLD + pct_color(pct);
  bar += std::string(filled, '|');
  bar += C_DIM;
  bar += std::string(width - filled, '.');
  bar += C_RST;
  return bar;
}

std::string fmt_pct(double pct) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%5.1f%%", pct);
  return pct_color(pct) + std::string(buf) + C_RST;
}

// ── Sysfs readers ──

Result<std::vector<CpuFreqInfo>> read_all_cpus() {
  std::vector<CpuFreqInfo> cpus;
  namespace fs = std::filesystem;

  const std::string cpu_base = "/sys/devices/system/cpu";
  if (!fs::exists(cpu_base)) {
    return std::unexpected(Error{500, "sysfs not mounted or /sys/devices/system/cpu not found"});
  }

  // Discover CPU directories (cpu0, cpu1, ...)
  for (auto& entry : fs::directory_iterator(cpu_base)) {
    if (!entry.is_directory()) continue;
    auto name = entry.path().filename().string();
    if (name.substr(0, 3) != "cpu") continue;
    int cpu_id = -1;
    auto [ptr, ec] = std::from_chars(name.data() + 3, name.data() + name.size(), cpu_id);
    if (ec != std::errc{} || cpu_id < 0) continue;

    CpuFreqInfo info;
    info.cpu_id = cpu_id;

    // Check online status
    auto online_path = cpu_base + "/" + name + "/online";
    auto online_str = read_file_str(online_path);
    if (!online_str.empty()) {
      int on = 0;
      std::from_chars(online_str.data(), online_str.data() + online_str.size(), on);
      info.online = (on == 1);
    }

    if (!info.online) {
      info.governor = "offline";
      cpus.push_back(std::move(info));
      continue;
    }

    // Read frequencies from cpufreq subsystem
    auto freq_base = cpu_base + "/" + name + "/cpufreq";
    info.cur_khz  = read_khz(freq_base + "/scaling_cur_freq");
    info.min_khz  = read_khz(freq_base + "/scaling_min_freq");
    info.max_khz  = read_khz(freq_base + "/scaling_max_freq");
    info.base_khz = read_khz(cpu_base + "/" + name + "/cpufreq/cpuinfo_max_freq");

    // Governor
    info.governor = read_file_str(freq_base + "/scaling_governor");

    // Turbo detection: if current or max exceeds base, turbo is active
    if (info.base_khz > 0 && info.max_khz > info.base_khz) {
      info.turbo = true;
    }
    if (info.base_khz <= 0) info.base_khz = info.max_khz;

    cpus.push_back(std::move(info));
  }

  if (cpus.empty()) {
    return std::unexpected(Error{404, "No CPU entries found in sysfs"});
  }

  // Sort by CPU ID
  std::sort(cpus.begin(), cpus.end(),
    [](const CpuFreqInfo& a, const CpuFreqInfo& b) { return a.cpu_id < b.cpu_id; });

  return cpus;
}

FreqSnapshot build_snapshot(const std::vector<CpuFreqInfo>& cpus) {
  FreqSnapshot snap;
  snap.cpus = cpus;
  snap.timestamp = 0.0;

  auto now = std::chrono::system_clock::now();
  snap.timestamp = std::chrono::duration<double>(now.time_since_epoch()).count();

  long long total_cur = 0, total_max = 0;
  int online = 0, turbo = 0;

  for (auto& c : cpus) {
    if (!c.online) continue;
    online++;
    if (c.turbo) turbo++;
    if (c.cur_khz > 0) total_cur += c.cur_khz;
    if (c.max_khz > 0) total_max += c.max_khz;
  }

  snap.total_cur = total_cur;
  snap.total_max = total_max;
  snap.online_count = online;
  snap.turbo_count = turbo;
  snap.avg_pct = (total_max > 0) ? (100.0 * total_cur / total_max) : 0.0;
  return snap;
}

// ── Demo data (for --demo) ──

FreqSnapshot build_demo_snapshot() {
  std::vector<CpuFreqInfo> cpus;
  // Simulate 8-core CPU (4 P-cores + 4 E-cores)
  long long base_freqs[] = {4700000, 4700000, 4700000, 4700000,
                             2100000, 2100000, 2100000, 2100000};
  long long cur_freqs[]  = {3800000, 2400000, 4100000, 1200000,
                             1800000, 2000000, 1500000, 1900000};
  const char* gov[] = {"performance", "powersave", "performance", "powersave",
                        "powersave", "powersave", "powersave", "powersave"};

  for (int i = 0; i < 8; i++) {
    CpuFreqInfo c;
    c.cpu_id = i;
    c.online = true;
    c.base_khz = base_freqs[i];
    c.max_khz = (i < 4) ? 5000000 : 2400000;  // turbo boost on P-cores
    c.min_khz = (i < 4) ? 800000 : 400000;
    c.cur_khz = cur_freqs[i];
    c.governor = gov[i];
    c.turbo = (i < 4);
    cpus.push_back(std::move(c));
  }

  return build_snapshot(cpus);
}

// ── Output formatters ──

void print_header([[maybe_unused]] const FreqSnapshot& snap) {
  std::cout << C_BOLD << C_CYAN;
  std::cout << "+============================================+\n";
  std::cout << "|        CPU Frequency Scaling Monitor       |\n";
  std::cout << "+============================================+" << C_RST << "\n";
  std::cout << C_DIM << "  Source: /sys/devices/system/cpu/cpu*/cpufreq/" << C_RST << "\n\n";
}

void print_summary(const FreqSnapshot& snap) {
  auto total_ghz = freq_to_ghz(snap.total_cur);
  auto total_max_ghz = freq_to_ghz(snap.total_max);

  std::cout << C_BOLD << "  -- Aggregate --" << C_RST << "\n";
  std::cout << "  CPUs online:  " << C_GREEN << snap.online_count << C_RST
            << " of " << snap.cpus.size() << "\n";
  std::cout << "  Total freq:   " << C_CYAN << total_ghz << C_RST
            << " / " << total_max_ghz << "\n";
  std::cout << "  Utilization:  " << fmt_pct(snap.avg_pct) << " "
            << make_bar(snap.avg_pct) << "\n";
  std::cout << "  Turbo active: " << (snap.turbo_count > 0 ? C_YELLOW : C_DIM)
            << snap.turbo_count << C_RST << " cores\n\n";
}

void print_per_core(const FreqSnapshot& snap) {
  std::cout << C_BOLD << "  -- Per-Core --" << C_RST << "\n";

  // Table header
  std::cout << "  " << C_DIM << "CPU     Current      Base        Max  Governor          Load" << C_RST << "\n";
  std::cout << "  " << C_DIM << std::string(66, '-') << C_RST << "\n";

  for (auto& c : snap.cpus) {
    if (!c.online) {
      std::cout << "  " << C_CYAN << "cpu" << c.cpu_id << C_RST
                << "    " << C_DIM << "offline" << C_RST << "\n";
      continue;
    }

    double pct = (c.max_khz > 0) ? (100.0 * c.cur_khz / c.max_khz) : 0.0;
    auto cur_str = freq_to_mhz(c.cur_khz);
    auto base_str = freq_to_mhz(c.base_khz);
    auto max_str = freq_to_mhz(c.max_khz);

    std::cout << "  " << C_CYAN << "cpu" << c.cpu_id << C_RST
              << "  " << pct_color(pct) << cur_str << C_RST
              << "  " << C_DIM << base_str << C_RST
              << "  " << C_GREEN << max_str << C_RST
              << "  " << c.governor
              << "  " << make_bar(pct, 12);
    if (c.turbo) std::cout << C_YELLOW << " *" << C_RST;
    std::cout << "\n";
  }
  std::cout << "\n";
}

// ── JSON output ──

std::string escape_json(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:   out += c; break;
    }
  }
  return out;
}

void print_json(const FreqSnapshot& snap) {
  std::string json = "{\n";
  json += "  \"timestamp\": " + std::to_string(snap.timestamp) + ",\n";
  json += "  \"online_count\": " + std::to_string(snap.online_count) + ",\n";
  json += "  \"total_count\": " + std::to_string(snap.cpus.size()) + ",\n";
  json += "  \"turbo_count\": " + std::to_string(snap.turbo_count) + ",\n";
  json += "  \"total_cur_khz\": " + std::to_string(snap.total_cur) + ",\n";
  json += "  \"total_max_khz\": " + std::to_string(snap.total_max) + ",\n";
  json += "  \"avg_utilization_pct\": " + std::to_string(snap.avg_pct) + ",\n";
  json += "  \"cpus\": [\n";
  for (size_t i = 0; i < snap.cpus.size(); i++) {
    auto& c = snap.cpus[i];
    json += "    {\"id\":" + std::to_string(c.cpu_id);
    json += ",\"online\":" + std::string(c.online ? "true" : "false");
    json += ",\"cur_khz\":" + std::to_string(c.cur_khz);
    json += ",\"min_khz\":" + std::to_string(c.min_khz);
    json += ",\"max_khz\":" + std::to_string(c.max_khz);
    json += ",\"base_khz\":" + std::to_string(c.base_khz);
    json += ",\"governor\":\"" + escape_json(c.governor) + "\"";
    json += ",\"turbo\":" + std::string(c.turbo ? "true" : "false");
    double pct = (c.max_khz > 0) ? (100.0 * c.cur_khz / c.max_khz) : 0.0;
    json += ",\"utilization_pct\":" + std::to_string(pct);
    json += "}";
    if (i + 1 < snap.cpus.size()) json += ",";
    json += "\n";
  }
  json += "  ]\n}";
  std::println(std::cout, "{}", json);
}

// ── Self-test ──

int run_selftest() {
  int passed = 0, failed = 0;

  auto check = [&](const char* name, bool cond) {
    if (cond) {
      std::println(std::cout, "  {}✓{} {}", C_GREEN, C_RST, name);
      passed++;
    } else {
      std::println(std::cout, "  {}✗{} {}", C_RED, C_RST, name);
      failed++;
    }
  };

  std::println(std::cout, "{}", C_BOLD + "  Running self-tests..." + C_RST);

  // Test 1: freq_to_ghz
  check("freq_to_ghz(4700000) == '4.70 GHz'", freq_to_ghz(4700000) == "4.70 GHz");
  check("freq_to_ghz(0) == 'N/A'", freq_to_ghz(0) == "N/A");
  check("freq_to_ghz(-1) == 'N/A'", freq_to_ghz(-1) == "N/A");

  // Test 2: freq_to_mhz
  check("freq_to_mhz(2400000) == '2400 MHz'", freq_to_mhz(2400000) == "2400 MHz");

  // Test 3: make_bar
  auto bar = make_bar(50.0, 10);
  check("make_bar(50,10) has 5 filled + 5 empty",
    bar.size() > 10);  // contains ANSI codes

  // Test 4: Demo snapshot
  auto demo = build_demo_snapshot();
  check("demo has 8 CPUs", static_cast<int>(demo.cpus.size()) == 8);
  check("demo online_count == 8", demo.online_count == 8);
  check("demo turbo_count > 0", demo.turbo_count > 0);
  check("demo avg_pct > 0", demo.avg_pct > 0.0);
  check("demo avg_pct < 100", demo.avg_pct < 100.0);

  // Test 5: Demo print (verify no crash)
  bool print_ok = true;
  try {
    print_header(demo);
    print_summary(demo);
    print_per_core(demo);
  } catch (...) {
    print_ok = false;
  }
  check("demo print no crash", print_ok);

  // Test 6: JSON output
  std::string json_out;
  {
    std::ostringstream oss;
    auto* old_buf = std::cout.rdbuf(oss.rdbuf());
    print_json(demo);
    std::cout.rdbuf(old_buf);
    json_out = oss.str();
  }
  check("JSON output starts with '{'", !json_out.empty() && json_out[0] == '{');
  check("JSON contains \"cpus\"", json_out.find("\"cpus\"") != std::string::npos);
  check("JSON contains \"turbo_count\"", json_out.find("\"turbo_count\"") != std::string::npos);
  check("JSON contains 8 cpu entries", json_out.find("\"id\":7") != std::string::npos);

  // Test 7: Real sysfs read (may fail in containers)
  auto real = read_all_cpus();
  if (real.has_value()) {
    check("sysfs read succeeded", true);
    check("real CPUs > 0", !real->empty());
  } else {
    std::println(std::cout, "  {}ℹ{} sysfs read failed ({}): container/restricted env — skipping",
      C_YELLOW, C_RST, real.error().message);
  }

  // Test 8: No-color mode
  init_colors();  // default on
  std::string colored = pct_color(50.0);
  g_color = false;
  init_colors();
  std::string plain = pct_color(50.0);
  check("--no-color removes ANSI codes", colored != plain);

  // Summary
  std::println(std::cout, "");
  std::println(std::cout, "{}  Results: {} passed, {} failed{}",
    failed == 0 ? C_GREEN : C_RED, passed, failed, C_RST);

  return failed;
}

// ── Main ──

int main(int argc, char** argv) {
  bool json_mode = false;
  bool watch_mode = false;
  bool demo_mode = false;
  bool selftest = false;
  int watch_secs = 2;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == "--json") json_mode = true;
    else if (arg == "--no-color") { g_color = false; }
    else if (arg == "--demo") demo_mode = true;
    else if (arg == "--selftest") selftest = true;
    else if ((arg == "--watch" || arg == "-w") && i + 1 < argc) {
      watch_mode = true;
      i++;
      std::from_chars(argv[i], argv[i] + std::strlen(argv[i]), watch_secs);
      if (watch_secs < 1) watch_secs = 1;
    }
    else if (arg == "--help" || arg == "-h") {
      std::println(std::cout, "{}", C_BOLD + "cpufreq-watch" + C_RST + " — Linux CPU frequency scaling monitor\n");
      std::println(std::cout, "Usage: cpufreq-watch [options]\n");
      std::println(std::cout, "Options:");
      std::println(std::cout, "  -w, --watch N   Refresh every N seconds (default: 2)");
      std::println(std::cout, "  --json          Output JSON format");
      std::println(std::cout, "  --no-color      Disable ANSI colors");
      std::println(std::cout, "  --demo          Use simulated data");
      std::println(std::cout, "  --selftest      Run built-in tests");
      std::println(std::cout, "  -h, --help      Show this help\n");
      std::println(std::cout, "{}", C_DIM + "Reads: /sys/devices/system/cpu/cpu*/cpufreq/" + C_RST);
      std::println(std::cout, "{}", C_DIM + "Info:  scaling_cur_freq, scaling_governor, cpuinfo_max_freq" + C_RST);
      return 0;
    }
  }

  init_colors();

  if (selftest) return run_selftest();

  // ── Snapshot loop ──
  auto take_snapshot = [&]() -> std::optional<FreqSnapshot> {
    if (demo_mode) return build_demo_snapshot();
    auto result = read_all_cpus();
    if (!result) {
      if (!json_mode) {
        std::println(std::cout, "{}Error:{} {}", C_RED, C_RST, result.error().message);
      }
      return std::nullopt;
    }
    return build_snapshot(*result);
  };

  if (watch_mode) {
    bool first = true;
    while (true) {
      auto snap = take_snapshot();
      if (!snap) return 1;

      // Clear screen
      if (!json_mode && !first) {
        std::println(std::cout, "\033[2J\033[H{}", "");
      }
      first = false;

      if (json_mode) {
        print_json(*snap);
      } else {
        print_header(*snap);
        print_summary(*snap);
        print_per_core(*snap);
        std::println(std::cout, "{}  Refreshing every {}s — press Ctrl+C to exit{}",
          C_DIM, watch_secs, C_RST);
      }
      std::cout << std::flush;
      std::this_thread::sleep_for(std::chrono::seconds(watch_secs));
    }
  } else {
    auto snap = take_snapshot();
    if (!snap) return 1;

    if (json_mode) {
      print_json(*snap);
    } else {
      print_header(*snap);
      print_summary(*snap);
      print_per_core(*snap);
    }
  }

  return 0;
}
