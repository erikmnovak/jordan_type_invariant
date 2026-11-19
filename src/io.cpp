#include <jordan/io.hpp>

#include "presentation.hpp"

#include <charconv>
#include <cmath>
#include <fstream>
#include <istream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace jordan::f2 {
namespace {

constexpr std::string_view whitespace = " \t\r\n\f\v";

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(whitespace);
  if (first == std::string_view::npos) return {};
  const auto last = text.find_last_not_of(whitespace);
  return text.substr(first, last - first + 1);
}

// Token views only live until the current input line changes. Each line is
// consumed once; neither a second copy of the file nor seeking is necessary.
std::vector<std::string_view> words(std::string_view text) {
  std::vector<std::string_view> result;
  text = trim(text);
  while (!text.empty()) {
    const auto end = text.find_first_of(whitespace);
    result.push_back(text.substr(0, end));
    if (end == std::string_view::npos) break;
    text = trim(text.substr(end));
  }
  return result;
}

/** Track physical line numbers while ignoring blank lines and # comments.
 * Numeric parsing uses from_chars, so callers' locale settings cannot change
 * the meaning of coordinates or cause a partially parsed token to be accepted.
 */
class Lines {
 public:
  explicit Lines(std::istream& input) : input_(input) {}

  bool next() {
    for (;;) {
      text_.clear();
      bool extracted = false;
      try {
        extracted = static_cast<bool>(std::getline(input_, text_));
      } catch (const std::ios_base::failure&) {
        // EOF is normal even if the caller enabled stream exceptions. With
        // eofbit exceptions, a final line can still contain valid characters.
        // Do not alter the caller's exception mask or swallow actual I/O errors.
        if (!input_.eof() || input_.bad()) throw;
        extracted = !text_.empty();
      }
      if (!extracted) {
        if (input_.bad() || !input_.eof()) throw std::ios_base::failure("Input stream read failed");
        return false;
      }
      ++number_;
      if (const auto comment = text_.find('#'); comment != std::string::npos) text_.erase(comment);
      if (!trim(text_).empty()) return true;
    }
  }

  std::string_view require(const char* description) {
    if (!next()) fail(std::string("Unexpected end of input; expected ") + description);
    return trim(text_);
  }

  [[noreturn]] void fail(const std::string& message) const {
    throw std::invalid_argument("Line " + std::to_string(number_) + ": " + message);
  }

  std::size_t count(std::string_view token) const {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error == std::errc::result_out_of_range) {
      throw std::length_error("Line " + std::to_string(number_) + ": Integer exceeds the supported range");
    }
    if (error != std::errc{} || end != token.data() + token.size()) fail("Expected a nonnegative integer");
    if (value > static_cast<std::size_t>(std::numeric_limits<Index>::max())) {
      throw std::length_error("Line " + std::to_string(number_) + ": Count exceeds the signed 32-bit limit");
    }
    return value;
  }

  Grade grade(std::string_view text, std::size_t parameters) const {
    const auto tokens = words(text);
    if (tokens.size() != parameters) fail("Grade has the wrong number of coordinates");
    Grade result;
    result.reserve(parameters);
    for (auto token : tokens) {
      // Floating-point from_chars permits a leading minus, but not a plus.
      // Accept the conventional explicit plus without weakening token checks.
      if (!token.empty() && token.front() == '+') {
        token.remove_prefix(1);
        if (token.empty() || token.front() == '-' || token.front() == '+') fail("Invalid coordinate");
      }
      double value = 0;
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
      if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value)) {
        fail("Expected a finite decimal coordinate");
      }
      result.push_back(value);
    }
    return result;
  }

 private:
  std::istream& input_;
  std::string text_;
  std::size_t number_ = 0;
};

// Counts are small headers; inspect all tokens before using them. In
// particular, accepting a nonzero third count would mistake a chain complex
// for a presentation and silently compute the wrong module.
std::pair<std::size_t, std::size_t> presentation_counts(Lines& lines) {
  const auto tokens = words(lines.require("relation and generator counts"));
  if (tokens.size() != 2 && tokens.size() != 3) lines.fail("Expected relations generators [0]");
  const auto relations = lines.count(tokens[0]);
  const auto generators = lines.count(tokens[1]);
  if (tokens.size() == 3 && lines.count(tokens[2]) != 0) {
    lines.fail("A general chain complex is not a presentation; the third count must be zero");
  }
  return {relations, generators};
}

std::pair<Grade, Column> presentation_row(Lines& lines, std::size_t parameters,
                                         std::size_t generators, bool relation) {
  const auto text = lines.require(relation ? "a relation grade and column" : "a generator grade");
  const auto separator = text.find(';');
  if (separator == std::string_view::npos || text.find(';', separator + 1) != std::string_view::npos) {
    lines.fail("Expected exactly one semicolon separating the grade and column");
  }
  Grade grade = lines.grade(text.substr(0, separator), parameters);
  Column column;
  const auto entries = words(text.substr(separator + 1));
  if (!relation && !entries.empty()) lines.fail("Generator boundaries must be empty in a presentation");
  for (const auto token : entries) {
    const auto row = lines.count(token);
    // Validate even entries which will later cancel in pairs over F_2.
    if (row >= generators) lines.fail("Relation index is outside the generator range");
    column.push_back(static_cast<Index>(row));
  }
  return {std::move(grade), std::move(column)};
}

// Preserve standard exception categories while adding a filename. This helper
// only owns the stream; the same readers also work with pipes and stringstreams.
template <typename Reader>
auto read_file(const std::filesystem::path& path, Reader reader) {
  std::ifstream input(path);
  if (!input) throw std::ios_base::failure("Cannot open " + path.string());
  try {
    return reader(input);
  } catch (const std::invalid_argument& error) {
    throw std::invalid_argument(path.string() + ": " + error.what());
  } catch (const std::length_error& error) {
    throw std::length_error(path.string() + ": " + error.what());
  } catch (const std::ios_base::failure& error) {
    throw std::ios_base::failure(path.string() + ": " + error.what());
  }
}

}  // namespace

Presentation read_presentation(std::istream& input) {
  Lines lines(input);
  Presentation result;
  if (lines.require("scc2020") != "scc2020") lines.fail("Expected scc2020");
  result.num_parameters = lines.count(lines.require("the parameter count"));
  if (result.num_parameters == 0) lines.fail("The parameter count must be positive");

  const auto [relations, generators] = presentation_counts(lines);
  // Grow from actual records instead of trusting a declared size to allocate
  // arbitrarily large buffers before detecting a truncated file.
  for (std::size_t j = 0; j < relations; ++j) {
    auto [grade, column] = presentation_row(lines, result.num_parameters, generators, true);
    result.relation_grades.push_back(std::move(grade));
    result.relations.push_back(std::move(column));
  }
  for (std::size_t g = 0; g < generators; ++g) {
    auto [grade, column] = presentation_row(lines, result.num_parameters, generators, false);
    result.generator_grades.push_back(std::move(grade));
  }
  if (lines.next()) lines.fail("Unexpected data after the presentation");
  return detail::prepare_presentation(std::move(result));
}

Presentation read_presentation_file(const std::filesystem::path& path) {
  return read_file(path, read_presentation);
}

SlicePattern read_slice_pattern(std::istream& input) {
  Lines lines(input);
  if (lines.require("jordan-slices-v1") != "jordan-slices-v1") lines.fail("Expected jordan-slices-v1");
  const auto dimensions = words(lines.require("parameter and slice counts"));
  if (dimensions.size() != 2) lines.fail("Expected parameters slices");
  const auto parameters = lines.count(dimensions[0]);
  const auto count = lines.count(dimensions[1]);
  if (parameters == 0 || count < 2) lines.fail("A pattern needs positive parameters and at least two slices");

  std::vector<std::vector<Grade>> slices;
  for (std::size_t l = 0; l < count; ++l) {
    const auto points = lines.count(lines.require("a slice point count"));
    std::vector<Grade> slice;
    for (std::size_t p = 0; p < points; ++p) {
      slice.push_back(lines.grade(lines.require("a slice point"), parameters));
    }
    slices.push_back(std::move(slice));
  }
  if (lines.next()) lines.fail("Unexpected data after the slice pattern");
  return SlicePattern(parameters, std::move(slices));
}

SlicePattern read_slice_pattern_file(const std::filesystem::path& path) {
  return read_file(path, read_slice_pattern);
}

}  // namespace jordan::f2
