#include <jordan/io.hpp>

#include <charconv>
#include <cmath>
#include <iostream>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace jordan::f2;

constexpr const char* help = R"(Usage:
  jordan_type type PRESENTATION SLICES [--shift "x y ..."]
                   [--method automatic|rank-blocks|streaming]
  jordan_type ranks PRESENTATION SLICES --source "x y ..." --target "x y ..."
                    [--power N]

PRESENTATION is an SCC2020 presentation over F_2.
SLICES is a jordan-slices-v1 file. Use - for exactly one input to read stdin.
Coordinates are finite decimals, with one value per parameter inside quotes.
The default shift for type is zero. ranks requires source <= target.
Without --power, ranks returns powers 0 through the slice count minus one.
Results go to stdout; errors go to stderr and return a nonzero exit status.
See README.md for input formats, methods, and runnable examples.
)";

Grade coordinates(const std::string& text, std::size_t parameters) {
  std::istringstream input(text);
  input.imbue(std::locale::classic());
  Grade result;
  std::string token;
  while (input >> token) {
    std::string_view view(token);
    if (view.front() == '+') {
      view.remove_prefix(1);
      if (view.empty() || view.front() == '-' || view.front() == '+') {
        throw std::invalid_argument("Invalid coordinate: " + token);
      }
    }
    double value = 0;
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (error != std::errc{} || end != view.data() + view.size() || !std::isfinite(value)) {
      throw std::invalid_argument("Expected a finite decimal coordinate: " + token);
    }
    result.push_back(value);
  }
  if (result.size() != parameters) throw std::invalid_argument("Wrong number of query coordinates");
  return result;
}

std::size_t power(const std::string& text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("--power requires a nonnegative integer in the supported size range");
  }
  return value;
}

JordanMethod method(const std::string& text) {
  if (text == "automatic") return JordanMethod::automatic;
  if (text == "rank-blocks") return JordanMethod::rank_blocks;
  if (text == "streaming") return JordanMethod::streaming;
  throw std::invalid_argument("--method must be automatic, rank-blocks, or streaming");
}

void print(const char* label, const std::vector<std::size_t>& values) {
  std::cout << label;
  for (const auto value : values) std::cout << ' ' << value;
  std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
    std::cout << help;
    return 0;
  }
  try {
    if (argc < 4) throw std::invalid_argument("Expected a query command and two input paths; use --help");
    const std::string_view command(argv[1]);
    if (command != "type" && command != "ranks") throw std::invalid_argument("Command must be type or ranks");

    std::map<std::string, std::string> options;
    for (int i = 4; i < argc; i += 2) {
      const std::string option(argv[i]);
      const bool allowed = command == "type" ? (option == "--shift" || option == "--method")
                                             : (option == "--source" || option == "--target" || option == "--power");
      if (!allowed) throw std::invalid_argument("Unknown option for this command: " + option);
      if (i + 1 == argc) throw std::invalid_argument("Missing value for " + option);
      if (!options.emplace(option, argv[i + 1]).second) throw std::invalid_argument("Repeated option: " + option);
    }
    if (command == "ranks" && (!options.contains("--source") || !options.contains("--target"))) {
      throw std::invalid_argument("ranks requires both --source and --target");
    }

    const std::string presentation_path(argv[2]);
    const std::string slices_path(argv[3]);
    if (presentation_path == "-" && slices_path == "-") {
      throw std::invalid_argument("Only one input can use stdin");
    }
    auto presentation = presentation_path == "-" ? read_presentation(std::cin)
                                                 : read_presentation_file(presentation_path);
    auto pattern = slices_path == "-" ? read_slice_pattern(std::cin) : read_slice_pattern_file(slices_path);
    if (presentation.num_parameters != pattern.num_parameters()) {
      throw std::invalid_argument("Presentation and slices have different parameter counts");
    }
    const auto parameters = presentation.num_parameters;
    Engine engine(std::move(presentation));

    // Compute the complete requested answer before writing anything to stdout,
    // so a rejected query cannot be mistaken for a partial successful result.
    if (command == "type") {
      const auto shift = options.contains("--shift") ? coordinates(options.at("--shift"), parameters)
                                                      : Grade(parameters, 0);
      const auto selected = options.contains("--method") ? method(options.at("--method"))
                                                          : JordanMethod::automatic;
      const auto result = engine.jordan_type(pattern, shift, selected);
      print("power ranks:", result.power_ranks);
      print("Jordan block counts:", result.block_multiplicities);
    } else {
      const auto source = coordinates(options.at("--source"), parameters);
      const auto target = coordinates(options.at("--target"), parameters);
      if (options.contains("--power")) {
        const auto exponent = power(options.at("--power"));
        const auto result = engine.filtered_rank(pattern, exponent, source, target);
        std::cout << "filtered rank[" << exponent << "]: " << result << '\n';
      } else {
        print("filtered ranks:", engine.filtered_ranks(pattern, source, target));
      }
    }
    std::cout.flush();
    if (!std::cout) throw std::ios_base::failure("Could not write the result to stdout");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "jordan_type: " << error.what() << '\n';
    return 1;
  }
}
