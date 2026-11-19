/** Compare Engine cache policies on one workload at a time. Query translations
 * are generated as needed, and answers are discarded after checking or writing
 * them. Input loading/generation, preparation and the two passes are separate.
 */
#include "workloads.hpp"

#include <chrono>
#include <iomanip>

using namespace jordan::f2;
using Clock = std::chrono::steady_clock;

namespace {
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::vector<std::size_t> answer(Engine& engine, const SlicePattern& pattern,
                               const Grade& shift, int method, const Grade& offset) {
  if (method == 2) {
    Grade target = shift;
    for (std::size_t k = 0; k < target.size(); ++k) target[k] += offset[k];
    return engine.filtered_ranks(pattern, shift, target);
  }
  auto value = engine.jordan_type(pattern, shift,
    method == 0 ? JordanMethod::rank_blocks : JordanMethod::streaming);
  value.power_ranks.insert(value.power_ranks.end(), value.block_multiplicities.begin(),
                          value.block_multiplicities.end());
  return value.power_ranks;
}

constexpr std::uint64_t initial_checksum = 14695981039346656037ULL;
void add_checksum(std::uint64_t& checksum, const std::vector<std::size_t>& values) {
  for (auto value : values) checksum = (checksum ^ value) * 1099511628211ULL;
}

struct Batch {
  double time_ms = 0;
  double first_ms = 0;
  std::uint64_t checksum = initial_checksum;
  std::uint64_t checked_prefix = initial_checksum;
  std::size_t rank0 = 0, rank1 = 0;
};

Batch timed_batch(Engine& engine, const SlicePattern& pattern, const benchmark::Workload& workload,
                  const benchmark::Options& settings, int method, std::size_t checked) {
  Batch result;
  const auto start = Clock::now();
  const auto count = benchmark::query_count(workload, settings);
  const auto offset = benchmark::rank_offset(workload);
  for (std::size_t i = 0; i < count; ++i) {
    benchmark::before_query(engine, settings.cache);
    const auto values = answer(engine, pattern, benchmark::shift(workload, i), method, offset);
    add_checksum(result.checksum, values);
    if (i == 0) { result.first_ms = milliseconds(start); result.rank0 = values[0]; result.rank1 = values[1]; }
    if (i + 1 == checked) result.checked_prefix = result.checksum;
  }
  result.time_ms = milliseconds(start);
  return result;
}

void measure(const benchmark::Workload& workload, const benchmark::Options& settings,
             double input_ms, int method, std::ofstream& dump) {
  const char* names[]{"rank_blocks", "streaming", "filtered_ranks"};
  const auto prepare = Clock::now();
  Engine engine(workload.presentation);
  SlicePattern pattern(workload.presentation.num_parameters, workload.slices);
  const double prepare_ms = milliseconds(prepare);
  const auto count = benchmark::query_count(workload, settings);
  const auto checked = dump.is_open() ? count : std::min(count, workload.query_period);
  const auto cold = timed_batch(engine, pattern, workload, settings, method, checked);
  const auto repeated = timed_batch(engine, pattern, workload, settings, method, checked);
  if (cold.checksum != repeated.checksum) throw std::runtime_error("The measured passes disagree");
  const auto stats = engine.cache_statistics();

  // Verify one schedule (or every requested answer when exporting) in a
  // separate pass. Compare full results before and after an immediate repeat;
  // a checksum also connects this pass to the measured cold-prefix results.
  engine.clear_cache();
  std::uint64_t verified = initial_checksum;
  std::size_t positive_queries = 0, max_rank0 = 0;
  const auto offset = benchmark::rank_offset(workload);
  for (std::size_t i = 0; i < checked; ++i) {
    benchmark::before_query(engine, settings.cache);
    const auto shift = benchmark::shift(workload, i);
    const auto value = answer(engine, pattern, shift, method, offset);
    if (answer(engine, pattern, shift, method, offset) != value) {
      throw std::runtime_error("Cached results disagree on query " + std::to_string(i));
    }
    add_checksum(verified, value);
    positive_queries += value[0] != 0;
    max_rank0 = std::max(max_rank0, value[0]);
    if (dump.is_open()) {
      dump << workload.name << ',' << names[method] << ',' << i;
      for (auto entry : value) dump << ',' << entry;
      dump << '\n';
    }
  }
  if (verified != cold.checked_prefix) throw std::runtime_error("Verification differs from the timed result");

  benchmark::write_input_fields(workload, settings);
  std::cout << ',' << names[method] << ',' << input_ms << ',' << prepare_ms << ',' << cold.first_ms << ','
            << cold.time_ms << ',' << repeated.time_ms << ',' << stats.cached_grades << ','
            << stats.cached_reductions << ',' << stats.generator_normal_forms << ',' << cold.rank0 << ',' << cold.rank1
            << ',' << checked << ',' << positive_queries << ',' << max_rank0 << ',' << cold.checksum << '\n';
  std::cout.flush();
  if (!std::cout) throw std::runtime_error("Could not write benchmark results");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = benchmark::options(argc, argv);
    if (settings.help) { benchmark::help(argv[0], benchmark::query_names); return 0; }
    if (settings.list) { benchmark::list_cases(benchmark::query_names); return 0; }
    if (settings.workload != "all" && settings.workload != "file" &&
        std::find(benchmark::query_names.begin(), benchmark::query_names.end(), settings.workload) == benchmark::query_names.end()) {
      throw std::invalid_argument("Unknown cache workload: " + settings.workload);
    }
    auto dump = benchmark::answer_file(settings);
    std::cout << benchmark::input_columns << ",method,input_ms,prepare_ms,first_ms,cold_batch_ms,repeat_batch_ms,"
                 "cached_grades,cached_reductions,normal_forms,first_rank0,first_rank1,verified_queries,positive_queries,max_rank0,checksum\n" << std::fixed << std::setprecision(6);
    const auto run = [&](std::string_view name) {
      const auto start = Clock::now();
      const auto workload = name == "file" ? benchmark::file_workload(settings) : benchmark::query_workload(name, settings.seed);
      const double input_ms = milliseconds(start);
      for (int method = 0; method < 3; ++method) measure(workload, settings, input_ms, method, dump);
    }; // Selection happens before building; only one workload is retained.
    if (settings.workload == "file") run("file");
    else for (auto name : benchmark::query_names) if (settings.workload == "all" || settings.workload == name) run(name);
    if (dump.is_open()) {
      dump.flush();
      if (!dump) throw std::runtime_error("Could not write answer file");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
