#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ArmorPatcher::Paths {
    [[nodiscard]] std::string ToBackslashes(std::string_view a_path);
    [[nodiscard]] std::vector<std::string> BuildCandidates(std::string_view a_originalPath);
    [[nodiscard]] std::optional<std::string> FindRedirectPath(std::string_view a_originalPath);
}  // namespace ArmorPatcher::Paths
