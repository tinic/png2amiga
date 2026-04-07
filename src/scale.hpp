#pragma once

#include "types.hpp"
#include <cstddef>

namespace png2amiga::scale {

enum class Kernel : unsigned char { mitchell_netravali, catmull_rom };

Result<Image> bicubic(const Image& src, std::size_t dst_width,
                      std::size_t dst_height,
                      Kernel kernel = Kernel::mitchell_netravali);

} // namespace png2amiga::scale
