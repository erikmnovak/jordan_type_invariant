#pragma once

#include <jordan/io.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// Test data stays in the source tree. CMake supplies its absolute location so
// the tests work from the build directory or any other working directory.
namespace cases {
inline std::filesystem::path path(const std::string& name) {
  return std::filesystem::path(JORDAN_TEST_CASES_DIR) / name;
}

inline std::filesystem::path example(const std::string& name) {
  return std::filesystem::path(JORDAN_TEST_CASES_DIR).parent_path().parent_path() / "examples" / name;
}

inline jordan::f2::Presentation presentation(const std::string& name) {
  return jordan::f2::read_presentation_file(path(name + ".scc"));
}

inline jordan::f2::SlicePattern slices(const std::string& name) {
  return jordan::f2::read_slice_pattern_file(path(name + ".slices"));
}

inline std::string text(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Cannot open test data: " + file.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::vector<std::string> names(const std::string& directory, const std::string& extension = ".scc") {
  std::vector<std::string> result;
  for (const auto& entry : std::filesystem::directory_iterator(path(directory))) {
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      result.push_back(entry.path().stem().string());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}
}  // namespace cases
