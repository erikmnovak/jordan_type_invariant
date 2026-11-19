#pragma once

// Named input recipes shared by the two benchmark programs. A recipe creates
// one presentation and pattern. Translations are computed from their index,
// so increasing the query count does not allocate a list of queries.
#include <jordan/io.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace benchmark {
using namespace jordan::f2;

enum class CachePolicy { reuse, reset };

struct Options {
  std::string workload = "all";
  CachePolicy cache = CachePolicy::reuse;
  std::optional<std::size_t> queries;
  std::string presentation_file;
  std::string slices_file;
  std::string step;
  std::string answers_file;
  bool help = false;
  bool list = false;
  std::optional<std::uint32_t> seed;
};

inline Options options(int argc, char** argv) {
  Options result;
  int i = 1;
  if (i < argc && !std::string_view(argv[i]).starts_with("--")) result.workload = argv[i++];
  std::set<std::string> seen;
  for (; i < argc; ++i) {
    const std::string option(argv[i]);
    if (option == "--help") { result.help = true; return result; }
    if (option == "--list") { result.list = true; return result; }
    if (!seen.insert(option).second) throw std::invalid_argument("Repeated option: " + option);
    if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
    const std::string value(argv[++i]);
    if (value.empty()) throw std::invalid_argument("Empty value for " + option);
    if (option == "--cache") {
      if (value == "reuse") result.cache = CachePolicy::reuse;
      else if (value == "reset") result.cache = CachePolicy::reset;
      else throw std::invalid_argument("--cache must be reuse or reset");
    } else if (option == "--queries") {
      std::size_t count = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
      if (error != std::errc{} || end != value.data() + value.size() || count == 0) {
        throw std::invalid_argument("--queries requires a positive integer");
      }
      result.queries = count;
    } else if (option == "--seed") {
      std::uint32_t seed = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seed);
      if (error != std::errc{} || end != value.data() + value.size()) throw std::invalid_argument("--seed requires a 32-bit unsigned integer");
      result.seed = seed;
    } else if (option == "--presentation") result.presentation_file = value;
    else if (option == "--slices") result.slices_file = value;
    else if (option == "--step") result.step = value;
    else if (option == "--answers") result.answers_file = value;
    else throw std::invalid_argument("Unknown option: " + option);
  }
  if (result.workload == "file") {
    if (result.seed) throw std::invalid_argument("File inputs do not use a generation seed");
    if (result.presentation_file.empty() || result.slices_file.empty()) {
      throw std::invalid_argument("The file workload requires --presentation and --slices");
    }
    if (!result.step.empty() && !result.queries) throw std::invalid_argument("--step requires --queries");
  } else if (!result.presentation_file.empty() || !result.slices_file.empty() || !result.step.empty()) {
    throw std::invalid_argument("Input paths and --step require the file workload");
  }
  return result;
}

inline const char* cache_name(CachePolicy policy) {
  return policy == CachePolicy::reuse ? "reuse" : "reset";
}

inline void before_query(Engine& engine, CachePolicy policy) {
  if (policy == CachePolicy::reset) engine.clear_cache();
}

struct Workload {
  std::string name;
  Presentation presentation;
  std::vector<std::vector<Grade>> slices;
  std::size_t query_period;
  // The same index must produce the same translation on every pass.
  std::function<Grade(std::size_t)> translation;
  std::optional<std::uint32_t> seed;
  std::string presentation_file = {};
  std::string slices_file = {};
  Grade rank_offset = {};
};

inline std::size_t query_count(const Workload& workload, const Options& settings) {
  return settings.queries.value_or(workload.query_period);
}

inline Grade shift(const Workload& workload, std::size_t query) {
  return workload.translation(query % workload.query_period);
}

inline Presentation free_module(std::size_t generators, std::size_t parameters) {
  return {parameters, std::vector<Grade>(generators, Grade(parameters, 0)), {}, {}};
}

// Each relation uses a fixed number of draws, then cancels duplicates over F2.
// Its grade is no earlier than the grades of the generators it uses.
inline Presentation mixed_module(std::size_t generators, std::size_t relations,
                                  std::size_t entries, std::uint32_t seed, std::size_t parameters = 2) {
  std::mt19937 random(seed);
  Presentation result;
  result.num_parameters = parameters;
  for (std::size_t g = 0; g < generators; ++g) {
    Grade grade(parameters);
    for (auto& value : grade) value = double(random() % 12);
    result.generator_grades.push_back(std::move(grade));
  }
  for (std::size_t r = 0; r < relations; ++r) {
    Grade grade(parameters, 0);
    Column raw;
    for (std::size_t j = 0; j < entries; ++j) {
      const auto g = static_cast<Index>(random() % generators);
      raw.push_back(g);
      for (std::size_t axis = 0; axis < parameters; ++axis) {
        grade[axis] = std::max(grade[axis], result.generator_grades[g][axis]);
      }
    }
    std::sort(raw.begin(), raw.end());
    Column column;
    for (std::size_t j = 0; j < raw.size();) {
      std::size_t end = j + 1;
      while (end < raw.size() && raw[end] == raw[j]) ++end;
      if ((end - j) % 2) column.push_back(raw[j]);
      j = end;
    }
    for (auto& value : grade) value += double(random() % 4);
    result.relation_grades.push_back(std::move(grade));
    result.relations.push_back(std::move(column));
  }
  return result;
}

inline std::vector<std::vector<Grade>> chain(std::size_t length, std::size_t parameters) {
  std::vector<std::vector<Grade>> slices;
  for (std::size_t i = 0; i < length; ++i) slices.push_back({Grade(parameters, double(i))});
  return slices;
}

inline std::vector<std::vector<Grade>> wide_chain(std::size_t length, std::size_t width) {
  std::vector<std::vector<Grade>> slices(length);
  for (std::size_t l = 0; l < length; ++l) {
    for (std::size_t j = 0; j < width; ++j) {
      const auto spacing = std::max(std::size_t{4}, width + 1);
      slices[l].push_back({double(spacing * l + j), double(spacing * l + width - 1 - j)});
    }
  }
  return slices;
}

inline std::vector<std::vector<Grade>> small_grid() {
  return {{{0, 0}}, {{1, 0}, {0, 1}}, {{2, 0}, {1, 1}, {0, 2}},
          {{2, 1}, {1, 2}}, {{2, 2}}};
}

inline Grade zero_shift(std::size_t) { return {0, 0}; }
inline constexpr std::uint32_t grid_seed = 20261109;

// Fixed presentations are shared across sampling studies; no matrix copies are
// stored for a different slice resolution or translation grid.
struct FileCase {
  const char* name;
  const char* presentation;
  const char* slices;
  std::size_t grid_side = 1;
  double transport_fraction = 1.0 / 16;
};

inline const std::vector<FileCase> file_cases{
  {"circle_h0_small", "benchmarks/data/circle_h0_small.scc", "benchmarks/data/circle_h0_small.slices"},
  {"circle_h0_large", "benchmarks/data/circle_h0_large.scc", "benchmarks/data/circle_h0_large.slices"},
  {"circle_h1", "benchmarks/data/circle_h1.scc", "benchmarks/data/circle_h1.slices"},
  {"torus_h1", "benchmarks/data/torus_h1.scc", "benchmarks/data/torus_h1.slices"},
  {"multicover_small", "benchmarks/data/multicover_small.scc", "benchmarks/data/multicover_small.slices"},
  {"multicover_large", "benchmarks/data/multicover_large.scc", "benchmarks/data/multicover_large.slices"},
  {"dragon", "benchmarks/data/dragon.scc", "benchmarks/data/dragon.slices"},
  {"tumour_foxp3", "benchmarks/data/tumour_foxp3.scc", "benchmarks/data/tumour_foxp3.slices"},
  {"tumour_cd8", "benchmarks/data/tumour_cd8.scc", "benchmarks/data/tumour_cd8.slices"},
  {"multicover_local_2", "benchmarks/data/multicover_large.scc", "benchmarks/data/multicover_large_local_2.slices"},
  {"multicover_local_8", "benchmarks/data/multicover_large.scc", "benchmarks/data/multicover_large_local_8.slices"},
  {"multicover_local_32", "benchmarks/data/multicover_large.scc", "benchmarks/data/multicover_large_local_32.slices"},
  {"dragon_local", "benchmarks/data/dragon.scc", "benchmarks/data/dragon_local_8.slices"},
  {"circle_scan_4", "benchmarks/data/circle_h0_small.scc", "benchmarks/data/circle_h0_small.slices", 4},
  {"circle_scan_8", "benchmarks/data/circle_h0_small.scc", "benchmarks/data/circle_h0_small.slices", 8},
  {"circle_scan_16", "benchmarks/data/circle_h0_small.scc", "benchmarks/data/circle_h0_small.slices", 16},
  {"fork_shared", "tests/cases/fork_shared_kernel.scc", "tests/cases/fork.slices"},
  {"fork_independent", "tests/cases/fork_independent_kernels.scc", "tests/cases/fork.slices"},
  {"fork_shared_single", "tests/cases/fork_shared_kernel.scc", "benchmarks/data/fork_single.slices"},
  {"fork_independent_single", "tests/cases/fork_independent_kernels.scc", "benchmarks/data/fork_single.slices"},
  {"diamond_cancellation", "tests/cases/constant_plane.scc", "tests/cases/diamond.slices"},
  {"death_transport", "tests/cases/death_transport.scc", "tests/cases/line_pair.slices", 1, 1.0}
};

inline const FileCase* file_case(std::string_view name) {
  for (const auto& item : file_cases) if (name == item.name) return &item;
  return nullptr;
}

inline std::filesystem::path source_path(const char* relative) {
  return std::filesystem::path(JORDAN_BENCHMARK_ROOT) / relative;
}

inline std::ofstream answer_file(const Options& settings) {
  if (settings.answers_file.empty()) return {};
  std::vector<std::filesystem::path> inputs{settings.presentation_file, settings.slices_file};
  for (const auto& item : file_cases) {
    if (settings.workload == "all" || settings.workload == item.name) {
      inputs.push_back(source_path(item.presentation));
      inputs.push_back(source_path(item.slices));
    }
  }
  for (const auto& input : inputs) {
    if (!input.empty() && std::filesystem::exists(input) && std::filesystem::exists(settings.answers_file) &&
        std::filesystem::equivalent(input, settings.answers_file)) {
      throw std::invalid_argument("The answer file must differ from the input files");
    }
  }
  std::ofstream output(settings.answers_file);
  if (!output) throw std::runtime_error("Cannot open answer file: " + settings.answers_file);
  return output;
}

inline Workload fixed_workload(const FileCase& item) {
  const auto input_path = source_path(item.presentation);
  const auto pattern_path = source_path(item.slices);
  auto input = read_presentation_file(input_path);
  const auto pattern = read_slice_pattern_file(pattern_path);
  if (input.num_parameters != pattern.num_parameters()) throw std::logic_error("Dataset and pattern dimensions differ");
  Grade low(input.num_parameters), high(input.num_parameters);
  bool first = true;
  for (const auto& layer : pattern.slices()) for (const auto& point : layer) {
    if (first) { low = high = point; first = false; }
    for (std::size_t k = 0; k < low.size(); ++k) { low[k] = std::min(low[k], point[k]); high[k] = std::max(high[k], point[k]); }
  }
  Grade window(low.size()), offset(low.size());
  for (std::size_t k = 0; k < low.size(); ++k) {
    window[k] = (high[k] - low[k]) / 4;
    offset[k] = (high[k] - low[k]) * item.transport_fraction;
  }
  // Translation scans refine one fixed physical window. They do not snap or
  // rescale the presentation, and finer scans do not enlarge the window.
  const auto side = item.grid_side;
  if (side > 1 && window.size() != 2) throw std::logic_error("Registered grid scans require two parameters");
  Workload result{item.name, std::move(input), pattern.slices(), side * side,
    [window, side](std::size_t i) {
      Grade value(window.size(), 0);
      if (side > 1) {
        value[0] = window[0] * double(i / side) / double(side - 1);
        value[1] = window[1] * double(i % side) / double(side - 1);
      }
      return value;
    }, {}};
  result.presentation_file = input_path.string();
  result.slices_file = pattern_path.string();
  result.rank_offset = std::move(offset);
  return result;
}

inline const std::vector<std::string_view> scaling_names{
  "thin_2048", "thin_16384", "thin_131072", "stalk_16", "stalk_256", "stalk_1024",
  "basis_sparse_512", "basis_dense_512", "basis_sparse_1024", "basis_dense_1024",
  "redundant_512", "boxes_1d", "boxes_2d", "boxes_3d", "random_sparse_512", "random_sparse_2048",
  "odd_width_9", "even_width_8"};

inline std::vector<std::string_view> with_suite(std::vector<std::string_view> names) {
  names.insert(names.end(), scaling_names.begin(), scaling_names.end());
  for (const auto& item : file_cases) names.push_back(item.name);
  return names;
}
inline const std::vector<std::string_view> method_names = with_suite({
  "free_chain_8", "free_chain_32", "free_chain_128", "free_chain_256",
  "short_grid", "odd_width_3", "even_width_2", "mixed_chain", "dense_relations"});
inline const std::vector<std::string_view> query_names = with_suite({
  "integer_grid", "fractional_grid", "dense_region", "distinct_regions", "free_chain"});

inline std::optional<std::uint32_t> default_seed(std::string_view name) {
  if (name == "short_grid" || name == "integer_grid" || name == "fractional_grid") return grid_seed;
  if (name == "mixed_chain") return 44001;
  if (name == "dense_relations") return 44002;
  if (name == "dense_region") return 20261116;
  if (name.starts_with("basis_") || name == "redundant_512") return 61801;
  if (name.starts_with("random_sparse_")) return 61802;
  return {};
}

// For a given size these present isomorphic modules, with known interval
// lengths on the sampled diagonal. Relation r is e_r plus persistent-tail
// generators: changing the tail changes the basis and fill-in, not the module.
inline Presentation interval_basis(std::size_t generators, std::size_t entries, std::uint32_t seed) {
  auto input = free_module(generators, 2);
  const auto relations = 3 * generators / 4;
  std::mt19937 random(seed);
  for (std::size_t r = 0; r < relations; ++r) {
    Column column{static_cast<Index>(r)};
    for (std::size_t i = 0; i < entries; ++i) column.push_back(static_cast<Index>(relations + random() % (generators - relations)));
    std::sort(column.begin(), column.end());
    Column normalized;
    for (std::size_t i = 0; i < column.size();) {
      auto end = i + 1;
      while (end < column.size() && column[end] == column[i]) ++end;
      if ((end - i) % 2) normalized.push_back(column[i]);
      i = end;
    }
    input.relations.push_back(std::move(normalized));
    input.relation_grades.push_back(Grade(2, double(1 + r % 15)));
  }
  return input;
}

inline Workload scaling_workload(std::string_view name, std::uint32_t seed) {
  for (std::size_t size : {2048, 16384, 131072}) if (name == "thin_" + std::to_string(size)) {
    Presentation input{1, {}, {}, {}};
    for (std::size_t g = 0; g < size; ++g) {
      input.generator_grades.push_back({double(g)});
      input.relation_grades.push_back({double(g + 2)});
      input.relations.push_back({static_cast<Index>(g)});
    }
    auto slices = chain(8, 1);
    for (auto& layer : slices) layer[0][0] += double(size / 2);
    return {std::string(name), std::move(input), std::move(slices), 1, [](std::size_t) -> Grade { return {0}; }, {}};
  }
  for (std::size_t size : {16, 256, 1024}) if (name == "stalk_" + std::to_string(size)) {
    return {std::string(name), free_module(size, 2), chain(16, 2), 1, zero_shift, {}};
  }
  if (name.starts_with("basis_") || name == "redundant_512") {
    const std::size_t size = name.ends_with("1024") ? 1024 : 512;
    auto input = interval_basis(size, name.starts_with("basis_dense") ? 32 : 1, seed);
    if (name == "redundant_512") {
      const auto columns = input.relations;
      const auto grades = input.relation_grades;
      for (int copy = 0; copy < 3; ++copy) {
        input.relations.insert(input.relations.end(), columns.begin(), columns.end());
        input.relation_grades.insert(input.relation_grades.end(), grades.begin(), grades.end());
      }
    }
    return {std::string(name), std::move(input), chain(16, 2), 4,
      [](std::size_t i) { return Grade(2, i / 4.0); }, seed};
  }
  for (std::size_t dimensions : {1, 2, 3}) if (name == "boxes_" + std::to_string(dimensions) + "d") {
    Presentation input{dimensions, {}, {}, {}};
    for (Index g = 0; g < 256; ++g) {
      Grade birth(dimensions, double(g % 4));
      input.generator_grades.push_back(birth);
      for (std::size_t k = 0; k < dimensions; ++k) {
        auto death = birth; death[k] += double(2 + g % 5);
        input.relation_grades.push_back(std::move(death)); input.relations.push_back({g});
      }
    }
    return {std::string(name), std::move(input), chain(8, dimensions), 4,
      [dimensions](std::size_t i) { return Grade(dimensions, i / 4.0); }, {}};
  }
  for (std::size_t size : {512, 2048}) if (name == "random_sparse_" + std::to_string(size)) {
    auto slices = chain(8, 2);
    for (auto& layer : slices) for (auto& value : layer[0]) value += 4;
    return {std::string(name), mixed_module(size, 3 * size / 4, 4, seed), std::move(slices), 4,
      [](std::size_t i) { return Grade(2, i / 4.0); }, seed};
  }
  if (name == "odd_width_9" || name == "even_width_8") {
    return {std::string(name), free_module(32, 2), wide_chain(32, name == "odd_width_9" ? 9 : 8), 1, zero_shift, {}};
  }
  throw std::invalid_argument("Unknown scaling workload: " + std::string(name));
}

inline Workload method_workload(std::string_view name, std::optional<std::uint32_t> override_seed = {}) {
  if (override_seed && !default_seed(name)) throw std::invalid_argument("This workload does not use a seed");
  const auto chosen_seed = override_seed.value_or(default_seed(name).value_or(0));
  if (const auto* data = file_case(name)) return fixed_workload(*data);
  if (std::find(scaling_names.begin(), scaling_names.end(), name) != scaling_names.end()) return scaling_workload(name, chosen_seed);
  // Constant modules isolate how the query algorithms scale with slice length.
  for (std::size_t length : {8, 32, 128, 256}) {
    if (name == "free_chain_" + std::to_string(length)) {
      return {std::string(name), free_module(64, 2), chain(length, 2), 1, zero_shift, {}};
    }
  }
  if (name == "short_grid") {
    return {std::string(name), mixed_module(256, 320, 4, chosen_seed), small_grid(), 12,
      [](std::size_t i) -> Grade { return {double(5 + i / 4), double(5 + i % 4)}; }, chosen_seed};
  }
  // Odd width retains long paths; even width makes two-step paths cancel.
  if (name == "odd_width_3" || name == "even_width_2") {
    const auto length = name == "odd_width_3" ? 32 : 64;
    const auto width = name == "odd_width_3" ? 3 : 2;
    return {std::string(name), free_module(32, 2), wide_chain(length, width), 1, zero_shift, {}};
  }
  if (name == "mixed_chain") {
    const auto seed = chosen_seed;
    return {std::string(name), mixed_module(128, 96, 4, seed), chain(64, 2), 4,
      [](std::size_t i) -> Grade { return {double(i % 2), double(i / 2)}; }, seed};
  }
  if (name == "dense_relations") {
    const auto seed = chosen_seed;
    return {std::string(name), mixed_module(128, 112, 32, seed), chain(64, 2), 1, zero_shift, seed};
  }
  throw std::invalid_argument("Unknown method workload: " + std::string(name));
}

inline Workload query_workload(std::string_view name, std::optional<std::uint32_t> override_seed = {}) {
  if (override_seed && !default_seed(name)) throw std::invalid_argument("This workload does not use a seed");
  const auto chosen_seed = override_seed.value_or(default_seed(name).value_or(0));
  if (const auto* data = file_case(name)) return fixed_workload(*data);
  if (std::find(scaling_names.begin(), scaling_names.end(), name) != scaling_names.end()) return scaling_workload(name, chosen_seed);
  if (name == "integer_grid" || name == "fractional_grid") {
    const bool fractional = name == "fractional_grid";
    return {std::string(name), mixed_module(256, 320, 4, chosen_seed), small_grid(), 108,
      [fractional](std::size_t i) -> Grade {
        const double offset = fractional ? (i / 36) / 4.0 : 0;
        return {4 + double((i % 36) / 6) + offset, 4 + double(i % 6) + offset};
      }, chosen_seed};
  }
  if (name == "dense_region") {
    // Every sampled point shares one nontrivial relation space.
    auto input = free_module(128, 2);
    const auto seed = chosen_seed;
    std::mt19937 random(seed);
    for (int r = 0; r < 112; ++r) {
      Column column;
      for (Index g = 0; g < 128; ++g) if (random() % 4 == 0) column.push_back(g);
      input.relations.push_back(std::move(column));
      input.relation_grades.push_back({0, 0});
    }
    return {std::string(name), std::move(input), small_grid(), 64,
      [](std::size_t i) -> Grade { return {i / 128.0, i / 128.0}; }, seed};
  }
  if (name == "distinct_regions") {
    // Each of the 64 translations visits four different active-data sets.
    Presentation input{1, {}, {}, {}};
    for (Index g = 0; g < 256; ++g) {
      input.generator_grades.push_back({double(g)});
      input.relation_grades.push_back({double(g + 2)});
      input.relations.push_back({g});
    }
    return {std::string(name), std::move(input), chain(4, 1), 64,
      [](std::size_t i) -> Grade { return {double(4 * i)}; }, {}};
  }
  if (name == "free_chain") {
    return {std::string(name), free_module(64, 1), chain(32, 1), 8,
      [](std::size_t i) -> Grade { return {i / 8.0}; }, {}};
  }
  throw std::invalid_argument("Unknown cache workload: " + std::string(name));
}

inline Workload file_workload(const Options& settings) {
  auto input = read_presentation_file(settings.presentation_file);
  const auto pattern = read_slice_pattern_file(settings.slices_file);
  if (pattern.num_parameters() != input.num_parameters) {
    throw std::invalid_argument("Presentation and slices have different parameter counts");
  }
  const auto dimensions = input.num_parameters;
  if (!settings.step.empty()) {
    Grade step;
    std::istringstream coordinates(settings.step);
    std::string token;
    while (coordinates >> token) {
      std::string_view text(token);
      if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '+' || text.front() == '-') {
          throw std::invalid_argument("Invalid --step coordinate: " + token);
        }
      }
      double value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        throw std::invalid_argument("--step coordinates must be finite decimals");
      }
      if (!std::isfinite(value * double(*settings.queries - 1))) {
        throw std::overflow_error("The requested translation sweep exceeds finite coordinates");
      }
      step.push_back(value);
    }
    if (step.size() != dimensions) throw std::invalid_argument("--step has the wrong number of coordinates");
    return {"file", std::move(input), pattern.slices(), *settings.queries,
      [step](std::size_t i) {
        Grade shift = step;
        for (auto& coordinate : shift) coordinate *= double(i);
        return shift;
      }, {}};
  }
  return {"file", std::move(input), pattern.slices(), 1,
    [dimensions](std::size_t) { return Grade(dimensions, 0); }, {}};
}

inline Grade rank_offset(const Workload& workload) {
  return workload.rank_offset.empty() ? Grade(workload.presentation.num_parameters, 0.25) : workload.rank_offset;
}

// Quote paths as CSV fields, including names containing commas or quotes.
inline std::string csv(const std::string& value) {
  std::string result = "\"";
  for (char c : value) { if (c == '"') result += '"'; result += c; }
  return result + '"';
}

inline void write_input_fields(const Workload& workload, const Options& settings) {
  std::size_t nonzeros = 0, points = 0;
  for (const auto& column : workload.presentation.relations) nonzeros += column.size();
  for (const auto& slice : workload.slices) points += slice.size();
  const auto presentation = workload.presentation_file.empty() ? settings.presentation_file : workload.presentation_file;
  const auto slices = workload.slices_file.empty() ? settings.slices_file : workload.slices_file;
  std::ostringstream offset;
  offset.precision(17);
  const auto displacement = rank_offset(workload);
  for (std::size_t k = 0; k < displacement.size(); ++k) offset << (k ? " " : "") << displacement[k];
  std::cout << workload.name << ',' << csv(presentation.empty() ? "generated" : presentation)
            << ',' << csv(slices) << ',' << csv(settings.step) << ',' << csv(offset.str()) << ',' << cache_name(settings.cache) << ',';
  if (workload.seed) std::cout << *workload.seed;
  std::cout << ',' << workload.presentation.num_parameters << ',' << workload.presentation.generator_grades.size()
            << ',' << workload.presentation.relations.size() << ',' << nonzeros << ',' << workload.slices.size()
            << ',' << points << ',' << workload.query_period << ',' << query_count(workload, settings);
}

inline constexpr const char* input_columns =
  "workload,presentation,slice_file,shift_step,rank_offset,cache,seed,parameters,generators,relations,nonzeros,slices,points,query_period,queries";

inline void list_cases(const std::vector<std::string_view>& names) {
  std::cout << "workload,presentation,slice_file,seed\n";
  for (auto name : names) {
    const auto* data = file_case(name);
    std::cout << name << ',' << csv(data ? source_path(data->presentation).string() : "")
              << ',' << csv(data ? source_path(data->slices).string() : "") << ',';
    if (const auto seed = default_seed(name)) std::cout << *seed;
    std::cout << '\n';
  }
}

inline void help(const char* program, const std::vector<std::string_view>& names) {
  std::cout << "Usage: " << program << " [WORKLOAD] [--cache reuse|reset] [--queries N]\n"
            << "       [--presentation FILE --slices FILE] [--step \"x y ...\"] [--answers FILE] [--seed N]\n\n"
            << "Workloads: all (default), file";
  for (auto name : names) std::cout << ", " << name;
  std::cout << "\nfile requires both input paths and uses zero translation unless --step is supplied.\n"
            << "For file inputs, --step with --queries generates translations i * step, starting at zero.\n"
            << "--queries repeats the workload's finite query schedule to the requested count.\n"
            << "reuse retains Engine caches; reset clears them before every measured query.\n"
            << "SlicePattern geometry stays prepared in either policy.\n"
            << "--seed N changes a generated random instance; deterministic/file cases reject it.\n"
            << "--list prints the case catalogue without constructing inputs.\n"
            << "--answers streams complete answers in an untimed pass; no file is written by default.\n";
}
}  // namespace benchmark
