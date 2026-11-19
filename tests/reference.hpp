#pragma once

/** @file reference.hpp
 * @brief Deliberately small, independent dense oracle for the defining formula.
 *
 * This code works in the ambient generator spaces. It explicitly constructs T,
 * multiplies its powers, and computes ranks modulo target relations using
 * rank([D T^i | R]) - rank(R). It shares no reduction or path-parity code with
 * the library. Its role is correctness checking on small examples.
 */
#include <jordan/jordan.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace reference {
using namespace jordan::f2;

struct Dense {
  std::size_t rows;
  std::size_t cols;
  std::vector<std::uint8_t> entries;
  Dense(std::size_t r, std::size_t c) : rows(r), cols(c), entries(r * c, 0) {}
  std::uint8_t& operator()(std::size_t r, std::size_t c) { return entries[r * cols + c]; }
  std::uint8_t operator()(std::size_t r, std::size_t c) const { return entries[r * cols + c]; }
};

inline Dense multiply(const Dense& left, const Dense& right) {
  Dense result(left.rows, right.cols);
  for (std::size_t r = 0; r < left.rows; ++r) {
    for (std::size_t k = 0; k < left.cols; ++k) {
      if (!left(r, k)) continue;
      for (std::size_t c = 0; c < right.cols; ++c) result(r, c) ^= right(k, c);
    }
  }
  return result;
}

inline std::size_t rank(Dense matrix) {
  std::size_t count = 0;
  for (std::size_t c = 0; c < matrix.cols && count < matrix.rows; ++c) {
    std::size_t pivot = count;
    while (pivot < matrix.rows && !matrix(pivot, c)) ++pivot;
    if (pivot == matrix.rows) continue;
    for (std::size_t j = 0; j < matrix.cols; ++j) std::swap(matrix(count, j), matrix(pivot, j));
    for (std::size_t r = count + 1; r < matrix.rows; ++r) {
      if (matrix(r, c)) {
        for (std::size_t j = c; j < matrix.cols; ++j) matrix(r, j) ^= matrix(count, j);
      }
    }
    ++count;
  }
  return count;
}

inline bool leq(const Grade& a, const Grade& b) {
  for (std::size_t k = 0; k < a.size(); ++k) if (a[k] > b[k]) return false;
  return true;
}

struct Sample {
  Grade grade;
  std::vector<std::size_t> active;
  std::vector<std::size_t> positions;
  std::size_t offset;
};

struct Layout {
  std::vector<Sample> samples;
  std::vector<std::size_t> slice_offsets;
  std::size_t dimension = 0;
};

inline Layout layout(const Presentation& presentation, const SlicePattern& pattern, const Grade& shift) {
  Layout result;
  for (const auto& slice : pattern.slices()) {
    result.slice_offsets.push_back(result.samples.size());
    for (const auto& point : slice) {
      Sample sample{point, {}, std::vector<std::size_t>(presentation.generator_grades.size()), result.dimension};
      for (std::size_t k = 0; k < point.size(); ++k) sample.grade[k] += shift[k];
      for (std::size_t g = 0; g < presentation.generator_grades.size(); ++g) {
        if (leq(presentation.generator_grades[g], sample.grade)) {
          sample.positions[g] = sample.active.size();
          sample.active.push_back(g);
        }
      }
      result.dimension += sample.active.size();
      result.samples.push_back(std::move(sample));
    }
  }
  result.slice_offsets.push_back(result.samples.size());
  return result;
}

inline std::size_t filtered_rank(const Presentation& presentation, const SlicePattern& pattern,
                                 std::size_t power, const Grade& a, const Grade& b) {
  const auto source = layout(presentation, pattern, a);
  const auto target = layout(presentation, pattern, b);
  Dense nilpotent(source.dimension, source.dimension);
  for (std::size_t l = 0; l + 1 < pattern.slice_count(); ++l) {
    for (std::size_t s = source.slice_offsets[l]; s < source.slice_offsets[l + 1]; ++s) {
      for (std::size_t t = source.slice_offsets[l + 1]; t < source.slice_offsets[l + 2]; ++t) {
        if (!leq(source.samples[s].grade, source.samples[t].grade)) continue;
        for (auto g : source.samples[s].active) {
          nilpotent(source.samples[t].offset + source.samples[t].positions[g],
                    source.samples[s].offset + source.samples[s].positions[g]) = 1;
        }
      }
    }
  }
  Dense operator_power(source.dimension, source.dimension);
  for (std::size_t i = 0; i < source.dimension; ++i) operator_power(i, i) = 1;
  for (std::size_t i = 0; i < power; ++i) operator_power = multiply(nilpotent, operator_power);

  Dense transport(target.dimension, source.dimension);
  for (std::size_t s = 0; s < source.samples.size(); ++s) {
    for (auto g : source.samples[s].active) {
      transport(target.samples[s].offset + target.samples[s].positions[g],
                source.samples[s].offset + source.samples[s].positions[g]) = 1;
    }
  }
  const auto image = multiply(transport, operator_power);
  std::size_t relation_count = 0;
  for (const auto& sample : target.samples) {
    for (const auto& grade : presentation.relation_grades) if (leq(grade, sample.grade)) ++relation_count;
  }
  Dense relations(target.dimension, relation_count);
  std::size_t column = 0;
  for (const auto& sample : target.samples) {
    for (std::size_t j = 0; j < presentation.relations.size(); ++j) {
      if (!leq(presentation.relation_grades[j], sample.grade)) continue;
      for (Index g : presentation.relations[j]) relations(sample.offset + sample.positions[g], column) ^= 1;
      ++column;
    }
  }
  Dense augmented(target.dimension, image.cols + relations.cols);
  for (std::size_t r = 0; r < target.dimension; ++r) {
    for (std::size_t c = 0; c < image.cols; ++c) augmented(r, c) = image(r, c);
    for (std::size_t c = 0; c < relations.cols; ++c) augmented(r, image.cols + c) = relations(r, c);
  }
  return rank(std::move(augmented)) - rank(std::move(relations));
}

}  // namespace reference
