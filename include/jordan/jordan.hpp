#pragma once

/** @file jordan.hpp
 * @brief Exact Jordan types and translated filtered ranks over F_2.
 *
 * Inputs are finite graded presentations, not filtered complexes. Coefficients
 * are in F_2 throughout this namespace; double-valued grades are only parameter
 * coordinates. A coordinate is active when its birth grade is coordinatewise
 * less than or equal to the query grade, including equality.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace jordan::f2 {

/// A generator index. The initial arithmetic backend uses signed 32-bit indices.
using Index = std::int32_t;

/// Finite physical parameter coordinates. There is no implicit grid conversion.
using Grade = std::vector<double>;

/** @brief Row indices with coefficient one in an F_2 column.
 *
 * Presentation columns may be unsorted and may contain duplicates. Engine
 * construction sorts them and cancels repeated indices modulo two. Negative
 * and out-of-range indices are rejected even when repeated.
 */
using Column = std::vector<Index>;

/** @brief A finite presentation, with one sparse column per relation.
 *
 * At p, the presented space is the span of generators born at grades <= p,
 * modulo the columns of relations born at grades <= p. Every nonzero relation
 * entry must refer to a generator born no later than that relation.
 *
 * Minimality is not required. Empty spaces, redundant relations, zero relation
 * columns, and repeated grades are supported. All coordinate arrays must have
 * num_parameters entries, num_parameters must be positive, and coordinates
 * must be finite. Generator and relation counts must fit in Index.
 *
 * These are owning standard containers. Engine takes its own copy or takes
 * ownership when constructed from an rvalue, so later input edits cannot
 * invalidate its prepared data or caches.
 */
struct Presentation {
  std::size_t num_parameters = 0;
  std::vector<Grade> generator_grades;
  std::vector<Grade> relation_grades;
  std::vector<Column> relations;
};

/** @brief An ordered finite slice pattern with reusable path parities.
 *
 * There must be at least two slices. Points are pairwise distinct across all
 * slices; empty slices are permitted. Points need not form antichains, and the
 * slice order is retained exactly as supplied. Only strict comparable pairs
 * between consecutive slices contribute to the operator.
 *
 * Construction prepares the adjacent-slice comparabilities. Higher path powers
 * are prepared only when requested by rank calculations and retained for the
 * lifetime of the shared pattern. Streaming Jordan types need no higher powers.
 * Copies share immutable sample points and synchronized geometry caches; a
 * pattern can safely be used by independent engines in different threads.
 *
 * @throws std::invalid_argument On duplicate points, nonfinite coordinates,
 *         inconsistent dimensions, zero parameters, or fewer than two slices.
 * @throws std::length_error If a slice exceeds the supported index range.
 */
class SlicePattern {
 public:
  SlicePattern(std::size_t num_parameters, std::vector<std::vector<Grade>> slices);
  [[nodiscard]] std::size_t num_parameters() const noexcept;
  [[nodiscard]] std::size_t slice_count() const noexcept;
  [[nodiscard]] const std::vector<std::vector<Grade>>& slices() const noexcept;

 private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Engine;
};

/** @brief Counts derived from a single nilpotent slice operator.
 *
 * power_ranks[j] is rank(T^j), for j = 0,...,n, including rank(T^0) = dim(V)
 * and the terminal zero. block_multiplicities[i-1] counts blocks of size i.
 * Block starting positions are not part of this result.
 */
struct JordanType {
  std::vector<std::size_t> power_ranks;
  std::vector<std::size_t> block_multiplicities;
};

/** @brief Equivalent algorithms for a fixed-translation Jordan type.
 *
 * rank_blocks computes ranks of the slice-to-slice power blocks. streaming
 * propagates a birth-ordered basis through adjacent slices and counts interval
 * lengths. automatic uses rank blocks for at most five slices or when the
 * squared pattern operator vanishes over F_2, and otherwise uses streaming.
 * This is a measured heuristic, not a claim of optimal selection for all inputs.
 * Explicit choices permit reproducible comparisons without changing the
 * mathematical output or the filtered-rank algorithm.
 */
enum class JordanMethod { automatic, rank_blocks, streaming };

/// Lightweight counters for checking reuse; they are not elapsed-time estimates.
struct CacheStatistics {
  /// Distinct coordinates retained, including those sharing a reduction.
  std::size_t cached_grades = 0;
  /// Lookups of coordinates already retained; excludes new-coordinate reuse.
  std::size_t grade_cache_hits = 0;
  /// Normal forms computed across the distinct retained reductions.
  std::size_t generator_normal_forms = 0;
  /// Distinct active generator/relation index sets reduced, including zero spaces.
  std::size_t cached_reductions = 0;
};

/** @brief A prepared presentation and a caller-owned query cache.
 *
 * Grades with identical active generator and relation index sets share a
 * relation reduction and its lazily computed generator normal forms. Repeated
 * coordinates also reuse their classification. Cached data lives until
 * clear_cache() or destruction; callers processing long streams can therefore
 * bound retained memory explicitly. Complete query answers are not cached.
 *
 * An Engine is movable but not copyable and is not thread-safe. Use an Engine
 * per concurrent worker. SlicePattern objects can be shared between workers.
 * No Python, external runtime, global cache, or implicit parallelism is used.
 *
 * @throws std::invalid_argument If the presentation is not well formed.
 * @throws std::length_error If counts exceed the supported index range.
 */
class Engine {
 public:
  explicit Engine(Presentation presentation);
  ~Engine();
  Engine(Engine&&) noexcept;
  Engine& operator=(Engine&&) noexcept;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /** @brief Compute a Jordan type at the supplied translation.
   * @param pattern Fixed finite sampling geometry.
   * @param translation Explicit shift, with one finite coordinate per parameter.
   * @param method Exact algorithm to use; see JordanMethod.
   * @return Power ranks and block counts, with the indexing described above.
   * @throws std::invalid_argument On incompatible coordinate dimensions.
   * @throws std::overflow_error If coordinate addition is nonfinite.
   * @throws std::length_error If a working matrix exceeds the index range.
   */
  [[nodiscard]] JordanType jordan_type(const SlicePattern& pattern, const Grade& translation,
                                       JordanMethod method = JordanMethod::automatic);

  /** @brief Query the degree-power filtered rank between two translations.
   *
   * This is rank(D_{target,source} T_source^power). At equal translations it
   * equals rank(T^power). Power zero is included; powers >= slice_count return
   * zero. Both shifts must be finite and source <= target coordinatewise.
   *
   * The API is restricted to comparable queries. Unlike the paper's extended
   * rank-function notation, it throws for an incomparable pair instead of
   * encoding infinity in an unsigned rank value. Finite samples are exact F_2
   * evaluations, not a claim to enumerate the entire translated invariant.
   *
   * @throws std::invalid_argument On incompatible dimensions or incomparable shifts.
   * @throws std::overflow_error If a sampled coordinate sum is nonfinite.
   * @throws std::length_error If a working matrix exceeds the index range.
   */
  [[nodiscard]] std::size_t filtered_rank(const SlicePattern& pattern, std::size_t power,
                                        const Grade& source, const Grade& target);

  /// Query powers 0,...,n-1 while sharing the prepared sampled spaces.
  [[nodiscard]] std::vector<std::size_t> filtered_ranks(const SlicePattern& pattern,
                                                      const Grade& source, const Grade& target);

  /// Remove grade lookups, shared reductions and normal forms; reset counters.
  void clear_cache();
  [[nodiscard]] CacheStatistics cache_statistics() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace jordan::f2
