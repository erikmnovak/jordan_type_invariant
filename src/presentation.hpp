#pragma once

#include <jordan/jordan.hpp>

namespace jordan::f2::detail {

// Share validation and F_2 normalization between file input and Engine input.
// This prepares owning arrays only; it does not reduce any sampled module space.
Presentation prepare_presentation(Presentation input);

}  // namespace jordan::f2::detail
