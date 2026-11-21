#pragma once

#include <filesystem>

namespace xbox_native {
int recompile_xbe(const std::filesystem::path& image, const std::filesystem::path& maps,
                  const std::filesystem::path& output, bool allow_partial,
                  const std::filesystem::path& disc = {});
}
