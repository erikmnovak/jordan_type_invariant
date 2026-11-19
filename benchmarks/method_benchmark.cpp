/** Compare rank blocks, streaming and automatic selection on one workload at
 * a time. Input generation/loading is separate from preparation and queries.
 * Each method keeps its own Engine so cache state is comparable. Full answers
 * are checked on the query schedule; repeated timing passes use checksums.
 */
#include "workloads.hpp"

#include <array>
#include <chrono>
#include <iomanip>

using namespace jordan::f2;
using Clock = std::chrono::steady_clock;

namespace {
double elapsed_ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::uint64_t checksum(const JordanType& type) {
  std::uint64_t sum = 0;
  for (std::size_t i = 0; i < type.power_ranks.size(); ++i) sum += (i + 1) * type.power_ranks[i];
  for (std::size_t i = 0; i < type.block_multiplicities.size(); ++i) sum += (i + 1) * type.block_multiplicities[i];
  return sum;
}

bool same(const JordanType& left, const JordanType& right) {
  return left.power_ranks == right.power_ranks && left.block_multiplicities == right.block_multiplicities;
}

struct Prepared {
  Engine engine;
  SlicePattern pattern;
  explicit Prepared(const benchmark::Workload& workload)
      : engine(workload.presentation), pattern(workload.presentation.num_parameters, workload.slices) {}
};

JordanType query(Prepared& prepared, JordanMethod method, const benchmark::Workload& workload,
                 const benchmark::Options& settings, std::size_t index) {
  benchmark::before_query(prepared.engine, settings.cache);
  return prepared.engine.jordan_type(prepared.pattern, benchmark::shift(workload, index), method);
}

double timed_batch(Prepared& prepared, JordanMethod method, const benchmark::Workload& workload,
                    const benchmark::Options& settings, std::uint64_t expected) {
  const auto start = Clock::now();
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < benchmark::query_count(workload, settings); ++i) {
    result += checksum(query(prepared, method, workload, settings, i));
  }
  const double time = elapsed_ms(start);
  if (result != expected) throw std::runtime_error("A timed result differs from the verified result");
  return time;
}

void compare(const benchmark::Workload& workload, const benchmark::Options& settings,
             double input_ms, std::ofstream& dump) {
  auto start = Clock::now();
  Prepared ranked(workload);
  const double rank_setup = elapsed_ms(start);
  start = Clock::now();
  Prepared streamed(workload);
  const double stream_setup = elapsed_ms(start);
  Prepared automatic(workload);

  start = Clock::now();
  const auto rank_first = query(ranked, JordanMethod::rank_blocks, workload, settings, 0);
  const double rank_cold = elapsed_ms(start);
  start = Clock::now();
  const auto stream_first = query(streamed, JordanMethod::streaming, workload, settings, 0);
  const double stream_cold = elapsed_ms(start);
  start = Clock::now();
  const auto auto_first = query(automatic, JordanMethod::automatic, workload, settings, 0);
  const double auto_cold = elapsed_ms(start);
  if (!same(rank_first, stream_first) || !same(rank_first, auto_first)) {
    throw std::runtime_error("The methods disagree on the first query");
  }

  // Check the complete finite schedule one query at a time. For a longer run,
  // derive the expected sum from whole cycles and the final partial cycle.
  const auto count = benchmark::query_count(workload, settings);
  const auto checked = std::min(count, workload.query_period);
  std::uint64_t cycle_sum = 0, prefix_sum = 0;
  std::size_t positive_queries = 0, max_dimension = 0;
  for (std::size_t i = 0; i < checked; ++i) {
    const auto rank = query(ranked, JordanMethod::rank_blocks, workload, settings, i);
    const auto stream = query(streamed, JordanMethod::streaming, workload, settings, i);
    const auto selected = query(automatic, JordanMethod::automatic, workload, settings, i);
    if (!same(rank, stream) || !same(rank, selected)) {
      throw std::runtime_error("The methods disagree on query " + std::to_string(i));
    }
    cycle_sum += checksum(rank);
    positive_queries += rank.power_ranks[0] != 0;
    max_dimension = std::max(max_dimension, rank.power_ranks[0]);
    if (i < count % workload.query_period) prefix_sum += checksum(rank);
  }
  const auto expected = (count / workload.query_period) * cycle_sum + prefix_sum;

  std::array<std::vector<double>, 3> times;
  std::array<Prepared*, 3> prepared{&ranked, &streamed, &automatic};
  const std::array<JordanMethod, 3> methods{JordanMethod::rank_blocks, JordanMethod::streaming,
                                          JordanMethod::automatic};
  for (int repetition = 0; repetition < 5; ++repetition) {
    for (int offset = 0; offset < 3; ++offset) {
      const auto method = static_cast<std::size_t>((repetition + offset) % 3);
      times[method].push_back(timed_batch(*prepared[method], methods[method], workload, settings, expected));
    }
  }
  for (auto& values : times) std::sort(values.begin(), values.end());

  // Optional inspection output is streamed after timing, never accumulated.
  if (dump.is_open()) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto value = query(ranked, JordanMethod::rank_blocks, workload, settings, i);
      dump << workload.name << ",jordan_type," << i;
      for (auto rank : value.power_ranks) dump << ',' << rank;
      for (auto blocks : value.block_multiplicities) dump << ',' << blocks;
      dump << '\n';
    }
  }
  std::size_t largest_block = 0;
  for (std::size_t i = 0; i < rank_first.block_multiplicities.size(); ++i) {
    if (rank_first.block_multiplicities[i]) largest_block = i + 1;
  }
  benchmark::write_input_fields(workload, settings);
  std::cout << ',' << input_ms << ',' << rank_setup << ',' << stream_setup << ',' << rank_cold << ','
            << stream_cold << ',' << auto_cold << ',' << times[0][2] << ',' << times[1][2] << ','
            << times[2][2] << ',' << times[0][2] / times[1][2] << ',' << rank_first.power_ranks[0]
            << ',' << rank_first.power_ranks[1] << ',' << largest_block << ',' << checked << ','
            << positive_queries << ',' << max_dimension << ',' << expected << '\n';
  std::cout.flush();
  if (!std::cout) throw std::runtime_error("Could not write benchmark results");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = benchmark::options(argc, argv);
    if (settings.help) { benchmark::help(argv[0], benchmark::method_names); return 0; }
    if (settings.list) { benchmark::list_cases(benchmark::method_names); return 0; }
    if (settings.workload != "all" && settings.workload != "file" &&
        std::find(benchmark::method_names.begin(), benchmark::method_names.end(), settings.workload) == benchmark::method_names.end()) {
      throw std::invalid_argument("Unknown method workload: " + settings.workload);
    }
    auto dump = benchmark::answer_file(settings);
    std::cout << benchmark::input_columns << ",input_ms,rank_setup_ms,stream_setup_ms,"
                 "rank_first_ms,stream_first_ms,auto_first_ms,rank_repeat_ms,stream_repeat_ms,auto_repeat_ms,"
                 "repeat_ratio,first_dimension,first_rank1,largest_block,verified_queries,positive_queries,max_dimension,checksum\n" << std::fixed << std::setprecision(6);
    const auto run = [&](std::string_view name) {
      const auto start = Clock::now();
      const auto workload = name == "file" ? benchmark::file_workload(settings) : benchmark::method_workload(name, settings.seed);
      const double input_ms = elapsed_ms(start);
      compare(workload, settings, input_ms, dump);
    }; // The workload and its engines are released before the next recipe.
    if (settings.workload == "file") run("file");
    else for (auto name : benchmark::method_names) if (settings.workload == "all" || settings.workload == name) run(name);
    if (dump.is_open()) {
      dump.flush();
      if (!dump) throw std::runtime_error("Could not write answer file");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
