#include <doctest/doctest.h>

#include <jordan/io.hpp>
#include <grlina/matrices/coord.hpp>

#include "cases.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

using namespace jordan::f2;

namespace {
Presentation presentation(const std::string& text) {
  std::istringstream input(text);
  return read_presentation(input);
}
}  // namespace

TEST_CASE("SCC2020 input reproduces the paper example") {
  const auto slices = read_slice_pattern_file(cases::example("paper.slices"));
  const auto input = read_presentation_file(cases::example("paper.scc"));
  CHECK(input.num_parameters == 2);
  CHECK(input.generator_grades == std::vector<Grade>{{0, 1}, {1, 0}});
  CHECK(input.relation_grades == std::vector<Grade>{{2, 1}});
  CHECK(input.relations == std::vector<Column>{{0, 1}});
  Engine engine(input);
  CHECK(engine.jordan_type(slices, {0, 0}).power_ranks == std::vector<std::size_t>{6, 3, 1, 0});
  CHECK(engine.jordan_type(slices, {0, 0}, JordanMethod::streaming).block_multiplicities ==
        std::vector<std::size_t>{1, 1, 1});
  CHECK(engine.filtered_ranks(slices, {0, 0}, {0, 0}) == std::vector<std::size_t>{6, 3, 1});
}

TEST_CASE("Reader accepts a presentation exported by the pinned arithmetic library") {
  // Start with file data, then exercise the dependency's independent writer.
  // This retains a live compatibility check without constructing a test matrix
  // in the test source.
  const auto expected = cases::presentation("readers/writer_three_parameters");
  REQUIRE(expected.num_parameters == 3);
  graded_linalg::CoordGradedSparseMatrix<double, 3, Index> matrix(
    static_cast<Index>(expected.relations.size()), static_cast<Index>(expected.generator_grades.size()));
  matrix.row_degrees.resize(expected.generator_grades.size());
  matrix.col_degrees.resize(expected.relation_grades.size());
  for (std::size_t g = 0; g < expected.generator_grades.size(); ++g) {
    for (std::size_t k = 0; k < 3; ++k) matrix.row_degrees[g][k] = expected.generator_grades[g][k];
  }
  for (std::size_t r = 0; r < expected.relation_grades.size(); ++r) {
    for (std::size_t k = 0; k < 3; ++k) matrix.col_degrees[r][k] = expected.relation_grades[r][k];
    matrix.set_col(static_cast<Index>(r), expected.relations[r]);
  }
  std::ostringstream output;
  matrix.to_stream(output);
  const auto input = presentation(output.str());
  CHECK(input.num_parameters == expected.num_parameters);
  CHECK(input.generator_grades == expected.generator_grades);
  CHECK(input.relation_grades == expected.relation_grades);
  CHECK(input.relations == expected.relations);
}

TEST_CASE("Reader handles comments, CRLF, optional zero count, and final unterminated lines") {
  const auto input = presentation(cases::text(cases::path("readers/whitespace_and_duplicates.scc")));
  CHECK(input.generator_grades == std::vector<Grade>{{-1.25}, {0.5}});
  CHECK(input.relation_grades == std::vector<Grade>{{2}, {3}});
  CHECK(input.relations == std::vector<Column>{{0}, {}});
}

TEST_CASE("Presentation input supports arbitrary parameter counts and empty spaces") {
  CHECK(cases::presentation("readers/zero_seven_parameters").num_parameters == 7);
  const auto free = cases::presentation("readers/free_five_parameters");
  CHECK(free.generator_grades == std::vector<Grade>{{0, 1, 2, 3, 4}});
  CHECK(free.relations.empty());
  CHECK(cases::presentation("readers/zero_relation").relations == std::vector<Column>{{}});
  // Indices must be in range before cancellation; only the surviving nonzero
  // coefficients must respect generator birth grades.
  CHECK(cases::presentation("readers/cancelled_early_relation").relations == std::vector<Column>{{}});
}

TEST_CASE("Malformed and unsupported presentation syntax is rejected completely") {
  const auto names = cases::names("readers/invalid");
  REQUIRE_FALSE(names.empty());
  for (const auto& name : names) {
    CAPTURE(name);
    CHECK_THROWS_AS(cases::presentation("readers/invalid/" + name), std::invalid_argument);
  }
  CHECK_THROWS_WITH(presentation(cases::text(cases::path("readers/invalid/unknown_header.scc"))),
                    "Line 1: Expected scc2020");
}

TEST_CASE("Reader reports truncated inputs, range violations, and incompatible grades") {
  const auto paper_scc = cases::text(cases::example("paper.scc"));
  // Systematically truncate the real fixture, retaining the reader's EOF
  // coverage without committing a separate file for every byte prefix.
  for (std::size_t end = 0; end < paper_scc.size() - 1; ++end) {
    CAPTURE(end);
    CHECK_THROWS_AS(presentation(paper_scc.substr(0, end)), std::invalid_argument);
  }
  CHECK_THROWS_WITH(presentation(cases::text(cases::path("readers/invalid/index_with_line_number.scc"))),
                    "Line 5: Relation index is outside the generator range");
  CHECK_THROWS_AS(cases::presentation("readers/invalid/relation_before_generator"), std::invalid_argument);
  const auto oversized = cases::names("readers/oversized");
  REQUIRE_FALSE(oversized.empty());
  for (const auto& name : oversized) {
    CAPTURE(name);
    CHECK_THROWS_AS(cases::presentation("readers/oversized/" + name), std::length_error);
  }
  // Large but representable declarations fail at EOF without reserving
  // billions of columns or coordinates.
  CHECK_THROWS_AS(cases::presentation("readers/invalid/truncated_large_count"), std::invalid_argument);
  CHECK_THROWS_AS(cases::presentation("readers/invalid/truncated_large_dimension"), std::invalid_argument);
}

TEST_CASE("Input streams need not seek and read errors are distinguished from EOF") {
  class NonSeekable : public std::stringbuf {
   public:
    explicit NonSeekable(const std::string& value) : std::stringbuf(value) {}
    bool sought = false;
    pos_type seekoff(off_type, std::ios_base::seekdir, std::ios_base::openmode) override {
      sought = true;
      return pos_type(off_type(-1));
    }
    pos_type seekpos(pos_type, std::ios_base::openmode) override {
      sought = true;
      return pos_type(off_type(-1));
    }
  };
  const auto paper_scc = cases::text(cases::example("paper.scc"));
  NonSeekable buffer(paper_scc);
  std::istream input(&buffer);
  CHECK(read_presentation(input).relations == std::vector<Column>{{0, 1}});
  CHECK_FALSE(buffer.sought);
  std::istringstream broken(paper_scc);
  broken.setstate(std::ios::badbit);
  CHECK_THROWS_AS(read_presentation(broken), std::ios_base::failure);
  std::istringstream failed(paper_scc);
  failed.setstate(std::ios::failbit);
  CHECK_THROWS_AS(read_presentation(failed), std::ios_base::failure);
  for (const auto mask : {std::ios::badbit | std::ios::failbit,
                         std::ios::badbit | std::ios::failbit | std::ios::eofbit}) {
    for (const auto& text : {paper_scc, paper_scc.substr(0, paper_scc.size() - 1)}) {
      std::istringstream throwing(text);
      throwing.exceptions(mask);
      CHECK(read_presentation(throwing).relations == std::vector<Column>{{0, 1}});
      CHECK(throwing.exceptions() == mask);
    }
  }
}

TEST_CASE("Slice files preserve empty slices and enforce the sampling contract") {
  const auto slices = cases::slices("readers/empty_slices");
  CHECK(slices.num_parameters() == 1);
  CHECK(slices.slices() == std::vector<std::vector<Grade>>{{{-1}}, {}, {{2}}});
  const auto invalid = cases::names("readers/invalid_slices", ".slices");
  REQUIRE_FALSE(invalid.empty());
  for (const auto& name : invalid) {
    CAPTURE(name);
    CHECK_THROWS_AS(cases::slices("readers/invalid_slices/" + name), std::invalid_argument);
  }
}
