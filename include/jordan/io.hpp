#pragma once

/** @file io.hpp
 * @brief Readers for finite presentations and slice patterns over F_2.
 *
 * These readers only supply the existing algebraic inputs. They do not compute
 * homology, minimize presentations, change coordinate directions, or infer a
 * sampling pattern. See the README's input-files section for format examples.
 */

#include <jordan/jordan.hpp>

#include <filesystem>
#include <iosfwd>

namespace jordan::f2 {

/** @brief Read one SCC2020 presentation.
 *
 * The first nonblank, uncommented line must be exactly "scc2020"; filename
 * extensions are ignored. The parameter count must be positive. Matrix counts
 * are relations, generators, and optionally a terminal zero.
 * Every relation and generator must have one grade and a semicolon. Generator
 * boundaries must be empty. Only zero-based row indices with coefficient one
 * are accepted; repeated indices cancel in F_2. Minimality is not required.
 *
 * Coordinates are finite decimal/scientific-notation doubles with a dot as the
 * decimal separator. Blank lines and # comments are ignored. General chain
 * complexes, extra records, reversed axes, and other format extensions are
 * rejected. The stream is consumed through EOF and need not support seeking.
 * The returned presentation owns its data, preserves generator/relation order,
 * and has sorted, normalized columns and validated grades.
 *
 * @throws std::invalid_argument On malformed or unsupported input, invalid
 *         indices, nonfinite coordinates, or incompatible relation grades.
 *         Syntax errors identify their input line.
 * @throws std::length_error If a declared count exceeds the signed 32-bit limit.
 * @throws std::ios_base::failure On a stream read failure.
 */
[[nodiscard]] Presentation read_presentation(std::istream& input);

/// Open a file and call read_presentation; file errors include the path.
[[nodiscard]] Presentation read_presentation_file(const std::filesystem::path& path);

/** @brief Read one versioned slice-pattern text file.
 *
 * The first record is "jordan-slices-v1", followed by "parameters slices".
 * Each slice starts with its point count, then one coordinate vector per line.
 * There must be at least two slices; empty slices are permitted. Coordinates,
 * whitespace, comments, stream consumption, and errors follow read_presentation.
 * SlicePattern validates distinct points and prepares adjacent comparisons.
 * Translations are supplied separately when making a query.
 */
[[nodiscard]] SlicePattern read_slice_pattern(std::istream& input);

/// Open a file and call read_slice_pattern; file errors include the path.
[[nodiscard]] SlicePattern read_slice_pattern_file(const std::filesystem::path& path);

}  // namespace jordan::f2
