#include <jordan/jordan.hpp>

#include "presentation.hpp"

#include <grlina/matrices/sparse.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace jordan::f2 {
namespace {

using Matrix = graded_linalg::SparseMatrix<Index>;
using Columns = graded_linalg::Column_traits<Column, Index>;

Index checked_index(std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<Index>::max())) {
    throw std::length_error("A matrix dimension exceeds the signed 32-bit index range");
  }
  return static_cast<Index>(size);
}

std::size_t checked_sum(std::size_t a, std::size_t b) {
  if (b > std::numeric_limits<std::size_t>::max() - a) {
    throw std::length_error("The total dimension exceeds the supported size range");
  }
  return a + b;
}

void validate_grade(const Grade& grade, std::size_t parameters) {
  if (grade.size() != parameters) {
    throw std::invalid_argument("A grade has the wrong number of parameters");
  }
  if (!std::all_of(grade.begin(), grade.end(), [](double x) { return std::isfinite(x); })) {
    throw std::invalid_argument("Grade coordinates must be finite");
  }
}

bool less_equal(const Grade& a, const Grade& b) {
  for (std::size_t k = 0; k < a.size(); ++k) {
    if (a[k] > b[k]) return false;
  }
  return true;
}

Grade translated(const Grade& point, const Grade& shift) {
  Grade result(point.size());
  for (std::size_t k = 0; k < point.size(); ++k) {
    result[k] = point[k] + shift[k];
    if (!std::isfinite(result[k])) {
      throw std::overflow_error("Translation produced a nonfinite coordinate");
    }
  }
  return result;
}

}  // namespace

namespace detail {

Presentation prepare_presentation(Presentation input) {
  if (input.num_parameters == 0) throw std::invalid_argument("The parameter count must be positive");
  checked_index(input.generator_grades.size());
  checked_index(input.relations.size());
  if (input.relation_grades.size() != input.relations.size()) {
    throw std::invalid_argument("Each relation must have exactly one grade");
  }
  for (const auto& grade : input.generator_grades) validate_grade(grade, input.num_parameters);
  for (const auto& grade : input.relation_grades) validate_grade(grade, input.num_parameters);
  for (std::size_t j = 0; j < input.relations.size(); ++j) {
    auto& column = input.relations[j];
    for (Index row : column) {
      if (row < 0 || static_cast<std::size_t>(row) >= input.generator_grades.size()) {
        throw std::invalid_argument("A relation contains an invalid generator index");
      }
    }
    std::sort(column.begin(), column.end());
    graded_linalg::convert_mod_2(column);
    for (Index row : column) {
      if (!less_equal(input.generator_grades[row], input.relation_grades[j])) {
        throw std::invalid_argument("Relation " + std::to_string(j) +
                                    " uses a generator before that generator is born");
      }
    }
  }
  return input;
}

}  // namespace detail

namespace {

/** Each column lists the target points reached with odd path multiplicity.
 * A block describes paths from one slice to a later slice. It is a scalar
 * matrix depending only on the pattern, not on the dimensions of the module.
 */
struct PathBlock {
  std::vector<Column> targets;
  bool is_zero() const {
    return std::all_of(targets.begin(), targets.end(), [](const Column& c) { return c.empty(); });
  }
};

/** The presentation data active at a grade, in original index order.
 *
 * Equal index sets give exactly the same relation reduction and quotient
 * coordinates. Counts or quotient dimensions alone would not identify them.
 * Store membership in 64-bit words to keep keys small even when most generators
 * and relations are active. Classify only requested grades; no global
 * subdivision is constructed. Unused bits in the final words stay zero.
 */
struct ActiveData {
  std::vector<std::uint64_t> generators;
  std::vector<std::uint64_t> relations;

  ActiveData(const Presentation& input, const Grade& grade)
      : generators((input.generator_grades.size() + 63) / 64, 0),
        relations((input.relation_grades.size() + 63) / 64, 0) {
    for (std::size_t g = 0; g < input.generator_grades.size(); ++g) {
      if (less_equal(input.generator_grades[g], grade)) generators[g / 64] |= std::uint64_t{1} << (g % 64);
    }
    for (std::size_t r = 0; r < input.relation_grades.size(); ++r) {
      if (less_equal(input.relation_grades[r], grade)) relations[r / 64] |= std::uint64_t{1} << (r % 64);
    }
  }

  static bool contains(const std::vector<std::uint64_t>& words, std::size_t index) {
    return (words[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
  }

  bool operator==(const ActiveData& other) const {
    return generators == other.generators && relations == other.relations;
  }
};

struct ActiveDataHash {
  std::size_t operator()(const ActiveData& active) const noexcept {
    // Hash both membership sets and their sizes. Full equality above resolves
    // collisions; a fingerprint alone must never decide algebraic reuse.
    std::uint64_t value = 14695981039346656037ULL;
    const auto append = [&value](std::uint64_t entry) {
      value = (value ^ entry) * 1099511628211ULL;
    };
    append(active.generators.size());
    for (auto word : active.generators) append(word);
    append(active.relations.size());
    for (auto word : active.relations) append(word);
    return static_cast<std::size_t>(value);
  }
};

/** A quotient shared by grades with identical active presentation data.
 *
 * The relation basis is triangular with its largest nonzero row as pivot.
 * Nonpivot active generators give a basis of the quotient. Keeping the original
 * generator indices makes an inclusion map a reduction of the same generator
 * at its target grade, without a change-of-basis matrix between ambient spaces.
 */
struct Stalk {
  Matrix relations;
  std::vector<Index> free_generators;
  std::unordered_map<Index, Column> normal_forms;

  Stalk(const Presentation& input, const ActiveData& active)
      : relations(0, checked_index(input.generator_grades.size())) {
    for (std::size_t j = 0; j < input.relations.size(); ++j) {
      if (ActiveData::contains(active.relations, j) && !input.relations[j].empty()) {
        relations.append_col(input.relations[j]);
      }
    }
    relations.set_num_cols(checked_index(relations.data.size()));
    relations.column_reduction_triangular(true);
    for (std::size_t g = 0; g < input.generator_grades.size(); ++g) {
      if (ActiveData::contains(active.generators, g) &&
          relations.pivots.find(static_cast<Index>(g)) == relations.pivots.end()) {
        free_generators.push_back(static_cast<Index>(g));
      }
    }
  }

  const Column& normal_form(Index generator, std::size_t& computed) {
    if (auto found = normal_forms.find(generator); found != normal_forms.end()) return found->second;

    Column work{generator};
    Column result;
    // Continue after encountering a free row: lower pivot rows still need to
    // be eliminated. Stopping at the first nonpivot would not give a quotient
    // coordinate vector and can produce incorrect ranks of combined maps.
    while (!work.empty()) {
      const Index row = work.back();
      const auto pivot = relations.pivots.find(row);
      if (pivot != relations.pivots.end()) {
        Columns::add_to(relations.column(pivot->second), work);
      } else {
        const auto free = std::lower_bound(free_generators.begin(), free_generators.end(), row);
        if (free == free_generators.end() || *free != row) {
          throw std::logic_error("An inclusion requested an inactive target generator");
        }
        result.push_back(checked_index(static_cast<std::size_t>(free - free_generators.begin())));
        work.pop_back();
      }
    }
    // Elimination visits decreasing global indices; quotient positions use the
    // same order. Reversing is enough to restore the sparse column convention.
    std::reverse(result.begin(), result.end());
    ++computed;
    return normal_forms.emplace(generator, std::move(result)).first->second;
  }
};

using SampledSpaces = std::vector<std::vector<Stalk*>>;

std::size_t dimension(const std::vector<Stalk*>& spaces) {
  std::size_t result = 0;
  for (const auto* space : spaces) result = checked_sum(result, space->free_generators.size());
  return result;
}

/** Construct one slice-to-slice map in quotient coordinates.
 *
 * For a source basis generator g, its image consists of the target normal
 * forms of g at precisely the points with odd path parity. Target summands
 * occupy disjoint row ranges, so concatenating their sorted columns preserves
 * the sparse format without sorting or multiplying module matrices.
 *
 * keep_zero_columns preserves the source coordinate indices for composition
 * with a changing basis. Rank-only calls may discard zero columns instead.
 */
Matrix block_matrix(const PathBlock& paths, const std::vector<Stalk*>& source,
                    const std::vector<Stalk*>& target, std::size_t& normal_forms_computed,
                    bool keep_zero_columns) {
  std::vector<std::size_t> offsets(target.size() + 1, 0);
  for (std::size_t t = 0; t < target.size(); ++t) {
    offsets[t + 1] = checked_sum(offsets[t], target[t]->free_generators.size());
  }
  Matrix image(0, checked_index(offsets.back()));
  for (std::size_t s = 0; s < source.size(); ++s) {
    if (!keep_zero_columns && paths.targets[s].empty()) continue;
    for (Index generator : source[s]->free_generators) {
      Column column;
      for (Index t : paths.targets[s]) {
        const auto& normal = target[t]->normal_form(generator, normal_forms_computed);
        for (Index row : normal) column.push_back(checked_index(offsets[t] + static_cast<std::size_t>(row)));
      }
      if (keep_zero_columns || !column.empty()) image.append_col(std::move(column));
    }
  }
  image.set_num_cols(checked_index(image.data.size()));
  return image;
}

std::size_t block_rank(const PathBlock& paths, const std::vector<Stalk*>& source,
                       const std::vector<Stalk*>& target, std::size_t& normal_forms_computed) {
  if (paths.is_zero() || dimension(source) == 0 || dimension(target) == 0) return 0;
  auto image = block_matrix(paths, source, target, normal_forms_computed, false);
  image.column_reduction_triangular(true);
  return image.pivots.size();
}

/** Count interval lengths in the equioriented representation of the slices.
 *
 * The current basis is ordered by birth slice. Its prefixes span the images of
 * earlier slices. Reducing columns from oldest birth to youngest preserves
 * this filtration: a dependent image ends the interval of that column's birth,
 * and an independent image continues it. Standard target vectors are appended
 * last, so the surviving complement consists exactly of new births.
 *
 * We reduce [A_l * basis | I_target] in that order. The reduction must retain
 * zero columns and their positions; deleting or swapping them would lose the
 * birth labels and produce incorrect interval lengths.
 */
JordanType streamed_type(const std::vector<PathBlock>& arrows, const SampledSpaces& spaces,
                         std::size_t& normal_forms_computed) {
  const std::size_t n = spaces.size();
  JordanType result{std::vector<std::size_t>(n + 1, 0), std::vector<std::size_t>(n, 0)};
  for (const auto& slice : spaces) {
    result.power_ranks[0] = checked_sum(result.power_ranks[0], dimension(slice));
  }

  std::vector<Column> basis;
  std::vector<std::size_t> births;
  const std::size_t initial_dimension = dimension(spaces.front());
  checked_index(initial_dimension);
  basis.reserve(initial_dimension);
  births.assign(initial_dimension, 0);
  for (std::size_t row = 0; row < initial_dimension; ++row) basis.push_back({checked_index(row)});

  for (std::size_t l = 0; l + 1 < n; ++l) {
    auto arrow = block_matrix(arrows[l], spaces[l], spaces[l + 1], normal_forms_computed, true);
    const auto target_dimension = static_cast<std::size_t>(arrow.get_num_rows());
    const std::size_t old_columns = basis.size();
    checked_index(checked_sum(old_columns, target_dimension));
    Matrix propagated(0, arrow.get_num_rows());

    for (const auto& vector : basis) {
      Column image;
      for (Index coordinate : vector) Columns::add_to(arrow.column(coordinate), image);
      propagated.append_col(std::move(image));
    }
    for (std::size_t row = 0; row < target_dimension; ++row) {
      propagated.append_col(Column{checked_index(row)});
    }
    propagated.set_num_cols(checked_index(propagated.data.size()));
    propagated.column_reduction_triangular(false);

    std::vector<Column> next_basis;
    std::vector<std::size_t> next_births;
    next_basis.reserve(target_dimension);
    next_births.reserve(target_dimension);
    for (std::size_t j = 0; j < propagated.data.size(); ++j) {
      if (propagated.data[j].empty()) {
        if (j < old_columns) ++result.block_multiplicities[l - births[j]];
      } else {
        next_basis.push_back(std::move(propagated.data[j]));
        next_births.push_back(j < old_columns ? births[j] : l + 1);
      }
    }
    if (next_basis.size() != target_dimension) {
      throw std::logic_error("The streamed interval basis does not span its target slice");
    }
    basis = std::move(next_basis);
    births = std::move(next_births);
  }
  for (const auto birth : births) ++result.block_multiplicities[n - birth - 1];

  // A size-k interval contributes max(k-power, 0) to a power rank. The number
  // of intervals still contributing is the drop between successive ranks.
  std::size_t remaining = 0;
  for (const auto count : result.block_multiplicities) remaining = checked_sum(remaining, count);
  for (std::size_t power = 0; power < n; ++power) {
    if (result.power_ranks[power] < remaining) {
      throw std::logic_error("Streamed interval lengths exceed the sampled dimension");
    }
    result.power_ranks[power + 1] = result.power_ranks[power] - remaining;
    remaining -= result.block_multiplicities[power];
  }
  if (result.power_ranks.back() != 0) {
    throw std::logic_error("Streamed interval lengths do not account for the sampled dimension");
  }
  return result;
}

}  // namespace

struct SlicePattern::Impl {
  std::size_t parameters;
  std::vector<std::vector<Grade>> points;
  // paths[power][start_slice] has columns indexed by points of start_slice.
  mutable std::vector<std::vector<PathBlock>> paths;
  mutable std::size_t prepared_power = 1;
  mutable std::mutex preparation_mutex;

  Impl(std::size_t dimensions, std::vector<std::vector<Grade>> slices)
      : parameters(dimensions), points(std::move(slices)), paths(points.size()) {
    if (parameters == 0 || points.size() < 2) {
      throw std::invalid_argument("A pattern needs positive parameters and at least two slices");
    }
    std::set<Grade> seen;
    for (const auto& slice : points) {
      checked_index(slice.size());
      for (const auto& point : slice) {
        validate_grade(point, parameters);
        if (!seen.insert(point).second) throw std::invalid_argument("Slice points must be pairwise distinct");
      }
    }
    const std::size_t n = points.size();
    paths[0].resize(n);
    for (std::size_t l = 0; l < n; ++l) {
      paths[0][l].targets.resize(points[l].size());
      for (std::size_t s = 0; s < points[l].size(); ++s) {
        paths[0][l].targets[s] = {checked_index(s)};
      }
    }
    paths[1].resize(n - 1);
    for (std::size_t l = 0; l + 1 < n; ++l) {
      paths[1][l].targets.resize(points[l].size());
      for (std::size_t s = 0; s < points[l].size(); ++s) {
        for (std::size_t t = 0; t < points[l + 1].size(); ++t) {
          if (less_equal(points[l][s], points[l + 1][t])) {
            paths[1][l].targets[s].push_back(checked_index(t));
          }
        }
      }
    }
  }

  /** Higher powers are only needed by rank queries. Streaming uses the adjacent
   * arrows directly, so it should not pay quadratic pattern-preparation costs.
   *
   * Published power blocks never change. A mutex serializes their first
   * preparation when independent engines share the same pattern. The outer
   * vector has its final size from construction, so later preparation cannot
   * invalidate references to previously published blocks.
   */
  const std::vector<PathBlock>& path_blocks(std::size_t power) const {
    if (power <= 1) return paths[power];
    std::lock_guard<std::mutex> guard(preparation_mutex);
    const auto n = points.size();
    while (prepared_power < power) {
      const auto next_power = prepared_power + 1;
      std::vector<PathBlock> next(n - next_power);
      for (std::size_t l = 0; l + next_power < n; ++l) {
        auto& block = next[l];
        block.targets.resize(points[l].size());
        for (std::size_t s = 0; s < points[l].size(); ++s) {
          for (Index intermediate : paths[next_power - 1][l].targets[s]) {
            // XOR retains path parity, including cancellations in F_2.
            Columns::add_to(paths[1][l + next_power - 1].targets[intermediate], block.targets[s]);
          }
        }
      }
      // Publish a complete level only. An allocation failure leaves the
      // already prepared prefix valid and a later request can retry safely.
      paths[next_power] = std::move(next);
      prepared_power = next_power;
    }
    return paths[power];
  }
};

SlicePattern::SlicePattern(std::size_t parameters, std::vector<std::vector<Grade>> slices)
    : impl_(std::make_shared<Impl>(parameters, std::move(slices))) {}
std::size_t SlicePattern::num_parameters() const noexcept { return impl_->parameters; }
std::size_t SlicePattern::slice_count() const noexcept { return impl_->points.size(); }
const std::vector<std::vector<Grade>>& SlicePattern::slices() const noexcept { return impl_->points; }

struct Engine::Impl {
  Presentation presentation;
  std::unordered_map<ActiveData, std::unique_ptr<Stalk>, ActiveDataHash> reductions;
  // Keep exact-grade lookups cheap: repeated coordinates need no classification.
  // The owning reduction cache keeps these pointers valid until clear_cache().
  std::map<Grade, Stalk*> cache;
  std::size_t hits = 0;
  std::size_t computed_normal_forms = 0;

  explicit Impl(Presentation input) : presentation(detail::prepare_presentation(std::move(input))) {}

  void validate_query(const SlicePattern& pattern, const Grade& source, const Grade& target) const {
    if (pattern.num_parameters() != presentation.num_parameters) {
      throw std::invalid_argument("The pattern and presentation parameter counts differ");
    }
    validate_grade(source, presentation.num_parameters);
    validate_grade(target, presentation.num_parameters);
    if (!less_equal(source, target)) throw std::invalid_argument("Filtered rank queries require source <= target");
  }

  SampledSpaces spaces(const SlicePattern& pattern, const Grade& shift) {
    SampledSpaces result(pattern.slice_count());
    for (std::size_t l = 0; l < pattern.slice_count(); ++l) {
      for (const auto& point : pattern.slices()[l]) {
        Grade grade = translated(point, shift);
        auto found = cache.find(grade);
        if (found == cache.end()) {
          ActiveData active(presentation, grade);
          auto reduced = reductions.find(active);
          if (reduced == reductions.end()) {
            auto stalk = std::make_unique<Stalk>(presentation, active);
            reduced = reductions.emplace(std::move(active), std::move(stalk)).first;
          }
          found = cache.emplace(std::move(grade), reduced->second.get()).first;
        } else {
          ++hits;
        }
        // Shared algebra does not merge point summands or change slice edges.
        result[l].push_back(found->second);
      }
    }
    return result;
  }

  std::size_t rank(const SlicePattern::Impl& pattern, std::size_t power,
                   const SampledSpaces& source, const SampledSpaces& target) {
    std::size_t total = 0;
    const auto& paths = pattern.path_blocks(power);
    for (std::size_t l = 0; l + power < source.size(); ++l) {
      total = checked_sum(total, block_rank(paths[l], source[l], target[l + power],
                                          computed_normal_forms));
    }
    return total;
  }
};

Engine::Engine(Presentation input) : impl_(std::make_unique<Impl>(std::move(input))) {}
Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

JordanType Engine::jordan_type(const SlicePattern& pattern, const Grade& translation, JordanMethod method) {
  impl_->validate_query(pattern, translation, translation);
  if (method != JordanMethod::automatic && method != JordanMethod::rank_blocks &&
      method != JordanMethod::streaming) {
    throw std::invalid_argument("Unknown Jordan type algorithm");
  }
  if (method == JordanMethod::automatic) {
    method = JordanMethod::rank_blocks;
    if (pattern.slice_count() > 5) {
      // Full basis propagation costs extra on short sequences. It also cannot
      // save many rank problems when every two-step path coefficient cancels:
      // in that case T^2 is zero for every module and every translation.
      // The measured comparison and the limits of this heuristic are recorded
      // in benchmarks/METHODS.md. Explicit methods remain available to callers.
      const auto& squares = pattern.impl_->path_blocks(2);
      const bool square_zero = std::all_of(squares.begin(), squares.end(),
                                           [](const PathBlock& block) { return block.is_zero(); });
      if (!square_zero) method = JordanMethod::streaming;
    }
  }
  const auto spaces = impl_->spaces(pattern, translation);
  if (method == JordanMethod::streaming) {
    return streamed_type(pattern.impl_->paths[1], spaces, impl_->computed_normal_forms);
  }
  const std::size_t n = pattern.slice_count();
  JordanType result{std::vector<std::size_t>(n + 1, 0), std::vector<std::size_t>(n, 0)};
  for (const auto& slice : spaces) result.power_ranks[0] = checked_sum(result.power_ranks[0], dimension(slice));
  for (std::size_t power = 1; power < n; ++power) {
    result.power_ranks[power] = impl_->rank(*pattern.impl_, power, spaces, spaces);
    // Once a power is zero, every subsequent power is zero as well.
    if (result.power_ranks[power] == 0) break;
  }
  for (std::size_t i = 1; i <= n; ++i) {
    const std::size_t next = (i == n) ? 0 : result.power_ranks[i + 1];
    const auto previous = result.power_ranks[i - 1];
    const auto current = result.power_ranks[i];
    if (previous < current || current < next || previous - current < current - next) {
      throw std::logic_error("Power ranks violate the nilpotent rank identities");
    }
    // Differences avoid both unsigned underflow and the overflow of 2*r_i.
    result.block_multiplicities[i - 1] = (previous - current) - (current - next);
  }
  return result;
}

std::size_t Engine::filtered_rank(const SlicePattern& pattern, std::size_t power,
                                  const Grade& source, const Grade& target) {
  impl_->validate_query(pattern, source, target);
  if (power >= pattern.slice_count()) return 0;
  const auto source_spaces = impl_->spaces(pattern, source);
  if (source == target) return impl_->rank(*pattern.impl_, power, source_spaces, source_spaces);
  const auto target_spaces = impl_->spaces(pattern, target);
  return impl_->rank(*pattern.impl_, power, source_spaces, target_spaces);
}

std::vector<std::size_t> Engine::filtered_ranks(const SlicePattern& pattern,
                                              const Grade& source, const Grade& target) {
  impl_->validate_query(pattern, source, target);
  const auto source_spaces = impl_->spaces(pattern, source);
  const auto target_spaces = (source == target) ? source_spaces : impl_->spaces(pattern, target);
  std::vector<std::size_t> ranks(pattern.slice_count());
  for (std::size_t power = 0; power < pattern.slice_count(); ++power) {
    ranks[power] = impl_->rank(*pattern.impl_, power, source_spaces, target_spaces);
    if (ranks[power] == 0) break;
  }
  return ranks;
}

void Engine::clear_cache() {
  impl_->cache.clear();
  impl_->reductions.clear();
  impl_->hits = 0;
  impl_->computed_normal_forms = 0;
}
CacheStatistics Engine::cache_statistics() const noexcept {
  return {impl_->cache.size(), impl_->hits, impl_->computed_normal_forms, impl_->reductions.size()};
}

}  // namespace jordan::f2
