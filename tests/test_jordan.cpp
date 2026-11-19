#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <jordan/jordan.hpp>

#include "cases.hpp"
#include "reference.hpp"

#include <fstream>
#include <cmath>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace jordan::f2;

TEST_CASE("The manuscript example and documented output") {
  Engine engine(read_presentation_file(cases::example("paper.scc")));
  const auto pattern = read_slice_pattern_file(cases::example("paper.slices"));
  const auto result = engine.jordan_type(pattern, {0, 0});
  CHECK(result.power_ranks == std::vector<std::size_t>{6, 3, 1, 0});
  CHECK(result.block_multiplicities == std::vector<std::size_t>{1, 1, 1});
  CHECK(engine.filtered_ranks(pattern, {0, 0}, {0, 0}) == std::vector<std::size_t>{6, 3, 1});
}

TEST_CASE("Combined maps distinguish the two fork modules") {
  Engine first(cases::presentation("fork_shared_kernel"));
  Engine second(cases::presentation("fork_independent_kernels"));
  const auto fork = cases::slices("fork");
  CHECK(first.jordan_type(fork, {0, 0}).block_multiplicities == std::vector<std::size_t>{2, 1});
  CHECK(second.jordan_type(fork, {0, 0}).block_multiplicities == std::vector<std::size_t>{0, 2});
}

TEST_CASE("Two paths cancel in characteristic two") {
  Engine engine(cases::presentation("constant_plane"));
  const auto diamond = cases::slices("diamond");
  const auto result = engine.jordan_type(diamond, {0, 0});
  CHECK(result.power_ranks == std::vector<std::size_t>{4, 2, 0, 0});
  CHECK(result.block_multiplicities == std::vector<std::size_t>{0, 2, 0});
  CHECK(engine.jordan_type(diamond, {0, 0}, JordanMethod::streaming).block_multiplicities ==
        result.block_multiplicities);
  CHECK(engine.filtered_rank(diamond, 2, {0, 0}, {5, 5}) == 0);
}

TEST_CASE("One parameter Jordan blocks record interval lengths") {
  Engine engine(cases::presentation("interval_lengths"));
  const auto pattern = cases::slices("interval_lengths");
  const auto result = engine.jordan_type(pattern, {0});
  CHECK(result.power_ranks == std::vector<std::size_t>{6, 4, 2, 0, 0, 0});
  CHECK(result.block_multiplicities == std::vector<std::size_t>{0, 0, 2, 0, 0});
}

TEST_CASE("A translated filtered rank includes the transport map") {
  Engine engine(cases::presentation("death_transport"));
  const auto pattern = cases::slices("line_pair");
  CHECK(engine.jordan_type(pattern, {0}).power_ranks == std::vector<std::size_t>{2, 1, 0});
  CHECK(engine.filtered_rank(pattern, 0, {0}, {1}) == 1);
  CHECK(engine.filtered_rank(pattern, 1, {0}, {1}) == 0);
  CHECK(engine.filtered_rank(pattern, 10, {0}, {1}) == 0);
  CHECK_THROWS_AS((void)engine.filtered_rank(pattern, 0, {1}, {0}), std::invalid_argument);
}

TEST_CASE("Zero spaces, empty slices, and reversed sample order are valid") {
  const auto empty = cases::slices("empty_plane");
  Engine zero(cases::presentation("zero_plane"));
  CHECK(zero.jordan_type(empty, {0, 0}).power_ranks == std::vector<std::size_t>{0, 0, 0, 0});
  Engine free(cases::presentation("free_line"));
  const auto gap = cases::slices("empty_middle_slice");
  CHECK(free.jordan_type(gap, {0}).block_multiplicities == std::vector<std::size_t>{2, 0, 0});
  const auto reversed = cases::slices("reversed_line");
  CHECK(free.jordan_type(reversed, {0}).block_multiplicities == std::vector<std::size_t>{3, 0, 0});
  CHECK(free.jordan_type(gap, {-10}).power_ranks == std::vector<std::size_t>{0, 0, 0, 0});
}

TEST_CASE("Redundant and duplicate relation coefficients are handled over F2") {
  const auto pattern = cases::slices("line_pair");
  Engine zero_column(cases::presentation("duplicate_coefficients"));
  CHECK(zero_column.jordan_type(pattern, {0}).block_multiplicities == std::vector<std::size_t>{0, 1});
  Engine repeated(cases::presentation("redundant_relations"));
  CHECK(repeated.jordan_type(pattern, {0}).power_ranks == std::vector<std::size_t>{0, 0, 0});
}

TEST_CASE("Invalid input and coordinate arithmetic have explicit errors") {
  // Start from valid files, then deliberately violate individual in-memory
  // contracts. This checks Engine/SlicePattern validation independently of I/O.
  const auto input = cases::presentation("death_transport");
  auto invalid = input;
  invalid.num_parameters = 0;
  CHECK_THROWS_AS(Engine{invalid}, std::invalid_argument);
  invalid = input;
  invalid.relation_grades.clear();
  CHECK_THROWS_AS(Engine{invalid}, std::invalid_argument);
  invalid = input;
  invalid.relations[0][0] = -1;
  CHECK_THROWS_AS(Engine{invalid}, std::invalid_argument);
  invalid = input;
  invalid.relations[0].assign(2, static_cast<Index>(input.generator_grades.size()));
  CHECK_THROWS_AS(Engine{invalid}, std::invalid_argument);
  invalid = input;
  invalid.generator_grades[0][0] = invalid.relation_grades[0][0] + 1;
  CHECK_THROWS_AS(Engine{invalid}, std::invalid_argument);

  const auto normal = cases::slices("line_pair");
  auto points = normal.slices();
  points.resize(1);
  CHECK_THROWS_AS(SlicePattern(normal.num_parameters(), points), std::invalid_argument);
  points = normal.slices();
  points[1] = points[0];
  CHECK_THROWS_AS(SlicePattern(normal.num_parameters(), points), std::invalid_argument);
  CHECK_THROWS_AS(SlicePattern(normal.num_parameters() + 1, normal.slices()), std::invalid_argument);
  points = normal.slices();
  points.back().front()[0] = std::numeric_limits<double>::infinity();
  CHECK_THROWS_AS(SlicePattern(normal.num_parameters(), points), std::invalid_argument);

  Engine engine(cases::presentation("free_line"));
  CHECK_THROWS_AS((void)engine.jordan_type(normal, {std::numeric_limits<double>::quiet_NaN()}), std::invalid_argument);
  CHECK_THROWS_AS((void)engine.jordan_type(normal, {}), std::invalid_argument);
  const auto two_dimensions = cases::slices("plane_pair");
  CHECK_THROWS_AS((void)engine.jordan_type(two_dimensions, {0, 0}), std::invalid_argument);
  const auto huge = cases::slices("coordinate_overflow");
  CHECK_THROWS_AS((void)engine.jordan_type(huge, {std::numeric_limits<double>::max()}), std::overflow_error);
  Engine plane(cases::presentation("constant_plane"));
  CHECK_THROWS_AS((void)plane.filtered_rank(two_dimensions, 0, {1, 0}, {0, 1}), std::invalid_argument);
}

TEST_CASE("Grade and generator reductions are reused and explicitly clearable") {
  auto input = cases::presentation("shared_quotients");
  Engine engine(input);
  const auto pattern = cases::slices("shared_quotients");
  const auto before = engine.jordan_type(pattern, {0, 0});
  const auto initial = engine.cache_statistics();
  input.relations[0].clear(); // The engine owns its prepared presentation.
  const auto after = engine.jordan_type(pattern, {0, 0});
  const auto repeated = engine.cache_statistics();
  CHECK(before.power_ranks == after.power_ranks);
  CHECK(initial.cached_grades == 3);
  CHECK(initial.cached_reductions == 2);
  CHECK(repeated.cached_grades == initial.cached_grades);
  CHECK(repeated.generator_normal_forms == initial.generator_normal_forms);
  CHECK(repeated.grade_cache_hits >= 3);
  engine.clear_cache();
  CHECK(engine.cache_statistics().cached_grades == 0);
  CHECK(engine.cache_statistics().cached_reductions == 0);
  CHECK(engine.cache_statistics().grade_cache_hits == 0);
  CHECK(engine.cache_statistics().generator_normal_forms == 0);
  Engine moved(std::move(engine));
  CHECK(moved.jordan_type(pattern, {0, 0}).power_ranks == before.power_ranks);
}

TEST_CASE("Shared reductions retain point multiplicity and path cancellation") {
  const auto input = cases::presentation("shared_diamond");
  Engine engine(input);
  const auto diamond = cases::slices("diamond");
  const auto expected = std::vector<std::size_t>{8, 4, 0, 0};
  CHECK(engine.jordan_type(diamond, {0, 0}).power_ranks == expected);
  const auto first = engine.cache_statistics();
  CHECK(first.cached_grades == 4);
  CHECK(first.cached_reductions == 1);
  CHECK(engine.jordan_type(diamond, {0.25, 0.25}, JordanMethod::streaming).power_ranks == expected);
  CHECK(engine.cache_statistics().cached_grades == 8);
  CHECK(engine.cache_statistics().cached_reductions == 1);
  CHECK(engine.cache_statistics().generator_normal_forms == first.generator_normal_forms);
  const auto transported = engine.filtered_ranks(diamond, {0, 0}, {10, 10});
  for (std::size_t k = 0; k < transported.size(); ++k) {
    CHECK(transported[k] == reference::filtered_rank(input, diamond, k, {0, 0}, {10, 10}));
  }
  CHECK(engine.cache_statistics().cached_reductions == 2);
}

TEST_CASE("Equal active counts do not identify generators or relations") {
  // The two one-dimensional sources are different lines in the target.
  Engine births(cases::presentation("distinct_generator_sets"));
  const auto merging = cases::slices("distinct_generator_sets");
  CHECK(births.jordan_type(merging, {0, 0}).power_ranks == std::vector<std::size_t>{4, 2, 0});
  CHECK(births.cache_statistics().cached_reductions == 3);

  // The one-dimensional targets have different kernels, so the joint map
  // has rank two. Sharing by relation count or stalk dimension loses this.
  Engine deaths(cases::presentation("distinct_relation_sets"));
  const auto splitting = cases::slices("fork");
  CHECK(deaths.jordan_type(splitting, {0, 0}).power_ranks == std::vector<std::size_t>{4, 2, 0});
  CHECK(deaths.cache_statistics().cached_reductions == 3);
}

TEST_CASE("Active-data reuse respects exact birth and death boundaries") {
  Engine engine(cases::presentation("activation_boundaries"));
  const auto point = cases::slices("activation_boundaries");
  const std::vector<double> shifts{std::nextafter(1.0, 0.0), 1.0, std::nextafter(1.0, 2.0),
                                   std::nextafter(2.0, 1.0), 2.0, std::nextafter(2.0, 3.0)};
  const std::vector<std::size_t> dimensions{0, 1, 1, 1, 0, 0};
  for (std::size_t i = 0; i < shifts.size(); ++i) {
    CAPTURE(shifts[i]);
    CHECK(engine.jordan_type(point, {shifts[i]}).power_ranks ==
          std::vector<std::size_t>{dimensions[i], 0, 0});
  }
  CHECK(engine.cache_statistics().cached_grades == 6);
  CHECK(engine.cache_statistics().cached_reductions == 3);
}

TEST_CASE("Active membership spans multiple storage words") {
  const auto input = cases::presentation("membership_word_boundary");
  Engine engine(input);
  const auto pattern = cases::slices("membership_word_boundary");
  const auto first = engine.jordan_type(pattern, {0});
  CHECK(first.power_ranks == std::vector<std::size_t>{6, 3, 1, 0});
  for (std::size_t k = 0; k < pattern.slice_count(); ++k) {
    CHECK(first.power_ranks[k] == reference::filtered_rank(input, pattern, k, {0}, {0}));
  }
  CHECK(engine.jordan_type(pattern, {0.25}, JordanMethod::streaming).power_ranks == first.power_ranks);
  CHECK(engine.cache_statistics().cached_reductions == 3);
  CHECK(engine.cache_statistics().cached_grades == 6);
}

TEST_CASE("File presentations agree with independent ambient matrix powers") {
  // queries.txt lists a case name, its source coordinates, then its target
  // coordinates. The presentation supplies the number of coordinates to read.
  std::ifstream queries(cases::path("oracle/queries.txt"));
  REQUIRE(queries.is_open());
  std::string name;
  std::size_t count = 0;
  while (queries >> name) {
    CAPTURE(name);
    const auto input = cases::presentation("oracle/" + name);
    const auto pattern = cases::slices("oracle/" + name);
    const auto d = input.num_parameters;
    Grade source(d), target(d);
    for (auto& coordinate : source) REQUIRE(bool(queries >> coordinate));
    for (auto& coordinate : target) REQUIRE(bool(queries >> coordinate));
    Engine engine(input);
    const auto jordan = engine.jordan_type(pattern, source);
    const auto ranked = engine.jordan_type(pattern, source, JordanMethod::rank_blocks);
    const auto streaming = engine.jordan_type(pattern, source, JordanMethod::streaming);
    CHECK(ranked.power_ranks == jordan.power_ranks);
    CHECK(streaming.power_ranks == jordan.power_ranks);
    CHECK(streaming.block_multiplicities == jordan.block_multiplicities);
    const auto filtered = engine.filtered_ranks(pattern, source, target);
    std::size_t represented_dimension = 0;
    for (std::size_t i = 0; i < pattern.slice_count(); ++i) {
      CAPTURE(name);
      CAPTURE(d);
      CAPTURE(i);
      CHECK(jordan.power_ranks[i] == reference::filtered_rank(input, pattern, i, source, source));
      CHECK(filtered[i] == reference::filtered_rank(input, pattern, i, source, target));
      CHECK(engine.filtered_rank(pattern, i, source, target) == filtered[i]);
      represented_dimension += (i + 1) * jordan.block_multiplicities[i];
      if (i + 1 < filtered.size()) CHECK(filtered[i] >= filtered[i + 1]);
    }
    CHECK(jordan.power_ranks.back() == 0);
    CHECK(represented_dimension == jordan.power_ranks[0]);

    // All input grades and sample points are integral. Moving both shifts
    // inside the same unit cells changes coordinates but no active index set.
    // Reuse must preserve the oracle-checked answers without new reductions.
    const auto prepared = engine.cache_statistics();
    for (auto& coordinate : source) coordinate += 0.25;
    for (auto& coordinate : target) coordinate += 0.25;
    CHECK(engine.jordan_type(pattern, source, JordanMethod::rank_blocks).power_ranks == jordan.power_ranks);
    CHECK(engine.jordan_type(pattern, source, JordanMethod::streaming).block_multiplicities ==
          jordan.block_multiplicities);
    CHECK(engine.filtered_ranks(pattern, source, target) == filtered);
    CHECK(engine.cache_statistics().cached_reductions == prepared.cached_reductions);
    CHECK(engine.cache_statistics().generator_normal_forms == prepared.generator_normal_forms);
    ++count;
  }
  CHECK(count == 240);
  CHECK(cases::names("oracle").size() == count);
}

TEST_CASE("Streaming retains the older birth when two images become dependent") {
  Engine engine(cases::presentation("older_birth_survives"));
  const auto pattern = cases::slices("older_birth_survives");
  const auto result = engine.jordan_type(pattern, {0}, JordanMethod::streaming);
  CHECK(result.block_multiplicities == std::vector<std::size_t>{1, 0, 0, 1, 0, 0});
  CHECK(result.power_ranks == std::vector<std::size_t>{5, 3, 2, 1, 0, 0, 0});
}

TEST_CASE("Long slice sequences agree with analytically known interval lengths") {
  Engine engine(cases::presentation("long_intervals"));
  const auto pattern = cases::slices("long_intervals");
  const auto result = engine.jordan_type(pattern, {0}, JordanMethod::streaming);
  std::vector<std::size_t> expected(pattern.slice_count(), 0);
  for (std::size_t length : {4, 48, 65, 70}) ++expected[length - 1];
  CHECK(result.block_multiplicities == expected);
  CHECK(result.power_ranks == engine.jordan_type(pattern, {0}, JordanMethod::rank_blocks).power_ranks);
  CHECK(result.power_ranks == engine.jordan_type(pattern, {0}).power_ranks);
  CHECK_THROWS_AS((void)engine.jordan_type(pattern, {0}, static_cast<JordanMethod>(100)),
                  std::invalid_argument);
}

TEST_CASE("Long even-width patterns retain the square-zero rank shortcut") {
  Engine engine(cases::presentation("constant_plane"));
  const auto pattern = cases::slices("even_width_cancellation");
  const auto result = engine.jordan_type(pattern, {0, 0});
  CHECK(result.block_multiplicities[0] == 2);
  CHECK(result.block_multiplicities[1] == 31);
  CHECK(result.power_ranks[2] == 0);
  CHECK(result.power_ranks == engine.jordan_type(pattern, {0, 0}, JordanMethod::streaming).power_ranks);
}

TEST_CASE("Independent engines can prepare powers on one shared pattern concurrently") {
  const auto pattern = cases::slices("shared_pattern");
  std::vector<std::future<std::size_t>> tasks;
  for (std::size_t power : {std::size_t{2}, std::size_t{15}, std::size_t{30}, std::size_t{8}}) {
    tasks.push_back(std::async(std::launch::async, [pattern, power] {
      Engine engine(cases::presentation("free_line"));
      return engine.filtered_rank(pattern, power, {0}, {0});
    }));
  }
  CHECK(tasks[0].get() == 30);
  CHECK(tasks[1].get() == 17);
  CHECK(tasks[2].get() == 2);
  CHECK(tasks[3].get() == 24);
}

TEST_CASE("Direct sums preserve both Jordan types and off-diagonal ranks") {
  const auto pattern = cases::slices("direct_sums/pattern");
  std::ifstream triples(cases::path("direct_sums/triples.txt"));
  REQUIRE(triples.is_open());
  std::string first_name, second_name, sum_name;
  std::size_t count = 0;
  while (triples >> first_name) {
    REQUIRE(bool(triples >> second_name >> sum_name));
    CAPTURE(first_name);
    const auto first = cases::presentation("direct_sums/" + first_name);
    const auto second = cases::presentation("direct_sums/" + second_name);
    const auto direct_sum = cases::presentation("direct_sums/" + sum_name);
    Engine left(first), right(second), sum(direct_sum);
    const auto a = left.jordan_type(pattern, {0, 0});
    const auto b = right.jordan_type(pattern, {0, 0});
    const auto c = sum.jordan_type(pattern, {0, 0});
    const auto streamed_sum = sum.jordan_type(pattern, {0, 0}, JordanMethod::streaming);
    CHECK(streamed_sum.block_multiplicities == c.block_multiplicities);
    for (std::size_t i = 0; i < pattern.slice_count(); ++i) {
      CHECK(c.block_multiplicities[i] == a.block_multiplicities[i] + b.block_multiplicities[i]);
      CHECK(sum.filtered_rank(pattern, i, {0, 0}, {1, 2}) ==
            left.filtered_rank(pattern, i, {0, 0}, {1, 2}) + right.filtered_rank(pattern, i, {0, 0}, {1, 2}));
    }
    ++count;
  }
  CHECK(count == 30);
  CHECK(cases::names("direct_sums").size() == 3 * count);
}

TEST_CASE("Homogeneous changes of generator basis leave the invariant unchanged") {
  const auto pattern = cases::slices("basis_changes/pattern");
  std::ifstream pairs(cases::path("basis_changes/pairs.txt"));
  REQUIRE(pairs.is_open());
  std::string original_name, changed_name;
  std::size_t count = 0;
  while (pairs >> original_name) {
    REQUIRE(bool(pairs >> changed_name));
    CAPTURE(original_name);
    const auto original = cases::presentation("basis_changes/" + original_name);
    // The changed file records its invertible homogeneous row operations.
    const auto changed = cases::presentation("basis_changes/" + changed_name);
    Engine first(original), second(changed);
    CHECK(first.jordan_type(pattern, {0, 0}).power_ranks == second.jordan_type(pattern, {0, 0}).power_ranks);
    CHECK(first.jordan_type(pattern, {0, 0}, JordanMethod::streaming).power_ranks ==
          second.jordan_type(pattern, {0, 0}, JordanMethod::streaming).power_ranks);
    CHECK(first.filtered_ranks(pattern, {0, 0}, {1, 1}) == second.filtered_ranks(pattern, {0, 0}, {1, 1}));
    ++count;
  }
  CHECK(count == 30);
  CHECK(cases::names("basis_changes").size() == 2 * count);
}

TEST_CASE("Streaming across uneven layers agrees with explicit dense powers") {
  const auto names = cases::names("uneven_layers");
  REQUIRE(names.size() == 24);
  for (const auto& name : names) {
    const auto input = cases::presentation("uneven_layers/" + name);
    const auto pattern = cases::slices("uneven_layers/" + name);
    Engine engine(input);
    const auto streaming = engine.jordan_type(pattern, {0, 0}, JordanMethod::streaming);
    const auto ranked = engine.jordan_type(pattern, {0, 0}, JordanMethod::rank_blocks);
    CHECK(streaming.power_ranks == ranked.power_ranks);
    for (std::size_t power : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{7}}) {
      CAPTURE(name);
      CAPTURE(power);
      CHECK(streaming.power_ranks[power] == reference::filtered_rank(input, pattern, power, {0, 0}, {0, 0}));
    }
  }
}
