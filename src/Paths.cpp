#include "Paths.h"

#include <algorithm>
#include <cctype>

#include "Config.h"

namespace ArmorPatcher::Paths {
    namespace {
        [[nodiscard]] bool EqualsIgnoreCase(std::string_view a_lhs, std::string_view a_rhs) {
            return a_lhs.size() == a_rhs.size() && std::ranges::equal(a_lhs, a_rhs, [](unsigned char a_left, unsigned char a_right) { return std::tolower(a_left) == std::tolower(a_right); });
        }

        [[nodiscard]] bool StartsWithIgnoreCase(std::string_view a_text, std::string_view a_prefix) { return a_text.size() >= a_prefix.size() && EqualsIgnoreCase(a_text.substr(0, a_prefix.size()), a_prefix); }

        [[nodiscard]] bool EndsWithIgnoreCase(std::string_view a_text, std::string_view a_suffix) { return a_text.size() >= a_suffix.size() && EqualsIgnoreCase(a_text.substr(a_text.size() - a_suffix.size()), a_suffix); }

        [[nodiscard]] std::string ToLower(std::string_view a_value) {
            std::string result(a_value);
            std::ranges::transform(result, result.begin(), [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
            return result;
        }

        [[nodiscard]] std::string StripSlashes(std::string_view a_path) {
            auto path = ToBackslashes(a_path);
            while (!path.empty() && path.front() == '\\') {
                path.erase(path.begin());
            }
            while (!path.empty() && path.back() == '\\') {
                path.pop_back();
            }
            return path;
        }

        [[nodiscard]] std::string StripMeshesPrefix(std::string_view a_path) {
            auto path = ToBackslashes(a_path);
            if (StartsWithIgnoreCase(path, "meshes\\")) {
                path.erase(0, 7);
            }
            return path;
        }

        [[nodiscard]] std::string ResourceProbePath(std::string_view a_path) {
            auto path = ToBackslashes(a_path);
            if (StartsWithIgnoreCase(path, "meshes\\")) {
                return path;
            }
            return "meshes\\" + path;
        }

        [[nodiscard]] bool ContainsConfiguredFolder(std::string_view a_path) {
            const auto path = StripSlashes(a_path);

            for (const auto& folder : Config::Get().meshFolders) {
                const auto cleanedFolder = StripSlashes(folder);
                if (cleanedFolder.empty()) {
                    continue;
                }

                const auto lowerPath = ToLower(path);
                const auto lowerFolder = ToLower(cleanedFolder);
                if (EqualsIgnoreCase(path, cleanedFolder) || StartsWithIgnoreCase(path, cleanedFolder + "\\") || EndsWithIgnoreCase(path, "\\" + cleanedFolder) ||
                    lowerPath.find("\\" + lowerFolder + "\\") != std::string::npos) {
                    return true;
                }
            }

            return false;
        }

        void PushUnique(std::vector<std::string>& a_candidates, std::string a_candidate) {
            if (a_candidate.empty()) {
                return;
            }

            const auto found = std::ranges::find_if(a_candidates, [&](const auto& existing) { return EqualsIgnoreCase(existing, a_candidate); });

            if (found == a_candidates.end()) {
                a_candidates.push_back(std::move(a_candidate));
            }
        }

        [[nodiscard]] bool ResourceExists(std::string_view a_modelPath) {
            try {
                const auto fullPath = ResourceProbePath(a_modelPath);
                RE::BSResourceNiBinaryStream stream(fullPath.c_str());
                return stream.good();
            } catch (const std::exception& e) {
                SKSE::log::debug("Resource probe failed for {}: {}", a_modelPath, e.what());
                return false;
            } catch (...) {
                SKSE::log::debug("Resource probe failed for {}", a_modelPath);
                return false;
            }
        }
    }  // namespace

    std::string ToBackslashes(std::string_view a_path) {
        std::string result(a_path);
        std::ranges::replace(result, '/', '\\');
        return result;
    }

    std::vector<std::string> BuildCandidates(std::string_view a_originalPath) {
        std::vector<std::string> candidates;
        if (a_originalPath.empty() || ContainsConfiguredFolder(a_originalPath)) {
            return candidates;
        }

        const auto original = ToBackslashes(a_originalPath);
        const auto relative = StripMeshesPrefix(original);
        const auto hadMeshesPrefix = StartsWithIgnoreCase(original, "meshes\\");
        const auto lastSlash = relative.find_last_of('\\');

        for (const auto& folder : Config::Get().meshFolders) {
            const auto cleanedFolder = StripSlashes(folder);
            if (cleanedFolder.empty()) {
                continue;
            }

            for (const auto style : Config::Get().pathStyles) {
                std::string candidate;
                if (hadMeshesPrefix) {
                    candidate = "meshes\\";
                }

                switch (style) {
                    case PathStyle::kPrefix:
                        candidate += cleanedFolder;
                        candidate += "\\";
                        candidate += relative;
                        PushUnique(candidates, std::move(candidate));
                        break;
                    case PathStyle::kSibling:
                        if (lastSlash != std::string::npos) {
                            candidate.append(relative.substr(0, lastSlash + 1));
                            candidate += cleanedFolder;
                            candidate += "\\";
                            candidate.append(relative.substr(lastSlash + 1));
                            PushUnique(candidates, std::move(candidate));
                        }
                        break;
                }
            }
        }

        return candidates;
    }

    std::optional<std::string> FindRedirectPath(std::string_view a_originalPath) {
        for (auto& candidate : BuildCandidates(a_originalPath)) {
            if (ResourceExists(candidate)) {
                return candidate;
            }
        }

        return std::nullopt;
    }
}  // namespace ArmorPatcher::Paths
