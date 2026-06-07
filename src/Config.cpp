#include "Config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string_view>
#include <toml.hpp>
#include <utility>

namespace ArmorPatcher {
    namespace {
        using Slot = RE::BIPED_MODEL::BipedObjectSlot;
        using SlotEntry = std::pair<std::string_view, Slot>;

        Settings g_settings;

        [[nodiscard]] Settings DefaultSettings() {
            Settings settings;

            settings.raceForms = {};
            settings.meshFolders = {"!UBE"};
            settings.pathStyles = {PathStyle::kPrefix, PathStyle::kSibling};
            settings.allowedSlots = {};
            settings.blockedSlots = {};

            return settings;
        }

        [[nodiscard]] std::string ToLower(std::string_view a_value) {
            std::string result(a_value);
            std::ranges::transform(result, result.begin(), [](unsigned char a_ch) { return static_cast<char>(std::tolower(a_ch)); });
            return result;
        }

        [[nodiscard]] std::string Trim(std::string_view a_value) {
            const auto first = a_value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos) {
                return {};
            }

            const auto last = a_value.find_last_not_of(" \t\r\n");
            return std::string(a_value.substr(first, last - first + 1));
        }

        [[nodiscard]] std::optional<RE::FormID> ParseFormID(std::string_view a_value) {
            auto text = Trim(a_value);
            if (text.empty()) {
                return std::nullopt;
            }

            if (text.starts_with("0x") || text.starts_with("0X")) {
                text.erase(0, 2);
            }

            RE::FormID formID = 0;
            const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), formID, 16);
            if (ec != std::errc{} || ptr != text.data() + text.size() || formID == 0) {
                return std::nullopt;
            }

            return formID;
        }

        [[nodiscard]] std::optional<RaceForm> ParseRaceForm(std::string_view a_value) {
            const auto separator = a_value.find('|');
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }

            auto plugin = Trim(a_value.substr(0, separator));
            auto formID = ParseFormID(a_value.substr(separator + 1));
            if (plugin.empty() || !formID) {
                return std::nullopt;
            }

            return RaceForm{std::move(plugin), *formID};
        }

        [[nodiscard]] std::optional<PathStyle> ParsePathStyle(std::string_view a_value) {
            const auto value = ToLower(Trim(a_value));
            if (value == "prefix") {
                return PathStyle::kPrefix;
            }
            if (value == "sibling") {
                return PathStyle::kSibling;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<Slot> ParseSlot(std::string_view a_value) {
            static constexpr SlotEntry slots[] = {
                {"head", Slot::kHead},
                {"hair", Slot::kHair},
                {"body", Slot::kBody},
                {"hands", Slot::kHands},
                {"forearms", Slot::kForearms},
                {"amulet", Slot::kAmulet},
                {"ring", Slot::kRing},
                {"feet", Slot::kFeet},
                {"calves", Slot::kCalves},
                {"shield", Slot::kShield},
                {"tail", Slot::kTail},
                {"longhair", Slot::kLongHair},
                {"circlet", Slot::kCirclet},
                {"ears", Slot::kEars},
                {"decapitatehead", Slot::kDecapitateHead},
                {"decapitate", Slot::kDecapitate},
                {"fx01", Slot::kFX01},
            };

            if (const auto it = std::ranges::find(slots, a_value, &SlotEntry::first); it != std::ranges::end(slots)) {
                return it->second;
            }
            return std::nullopt;
        }

        void ReadStringArray(const toml::value& a_table, std::string_view a_key, std::vector<std::string>& a_out) {
            if (!a_table.contains(std::string(a_key))) {
                return;
            }

            const auto& value = a_table.at(std::string(a_key));
            if (!value.is_array()) {
                SKSE::log::warn("{} must be an array of strings", a_key);
                return;
            }

            std::vector<std::string> parsed;
            for (const auto& item : value.as_array()) {
                if (!item.is_string()) {
                    SKSE::log::warn("Ignoring non-string {} entry", a_key);
                    continue;
                }

                auto text = Trim(item.as_string());
                if (!text.empty()) {
                    parsed.push_back(std::move(text));
                }
            }

            if (!parsed.empty()) {
                a_out = std::move(parsed);
            }
        }

        void ReadPathStyles(const toml::value& a_table, Settings& a_settings) {
            if (!a_table.contains("Styles")) {
                return;
            }

            const auto& value = a_table.at("Styles");
            if (!value.is_array()) {
                SKSE::log::warn("Paths.Styles must be an array");
                return;
            }

            std::vector<PathStyle> parsed;
            for (const auto& item : value.as_array()) {
                if (!item.is_string()) {
                    SKSE::log::warn("Ignoring non-string path style");
                    continue;
                }

                if (auto style = ParsePathStyle(item.as_string())) {
                    parsed.push_back(*style);
                } else {
                    SKSE::log::warn("Ignoring unknown path style {}", item.as_string());
                }
            }

            if (!parsed.empty()) {
                a_settings.pathStyles = std::move(parsed);
            }
        }

        void ReadSlots(const toml::value& a_table, std::string_view a_key, std::vector<Slot>& a_out) {
            if (!a_table.contains(std::string(a_key))) {
                return;
            }

            const auto& value = a_table.at(std::string(a_key));
            if (!value.is_array()) {
                SKSE::log::warn("{} must be an array of slot names", a_key);
                return;
            }

            std::vector<Slot> parsed;
            for (const auto& item : value.as_array()) {
                if (!item.is_string()) {
                    SKSE::log::warn("Ignoring non-string slot entry");
                    continue;
                }

                if (auto slot = ParseSlot(item.as_string())) {
                    parsed.push_back(*slot);
                } else {
                    SKSE::log::warn("Ignoring unknown slot {}", item.as_string());
                }
            }

            a_out = std::move(parsed);
        }

        void ReadRaces(const toml::value& a_table, Settings& a_settings) {
            std::vector<RaceForm> parsed;

            if (a_table.contains("Forms")) {
                const auto& value = a_table.at("Forms");
                if (value.is_array()) {
                    for (const auto& item : value.as_array()) {
                        if (!item.is_string()) {
                            SKSE::log::warn("Ignoring non-string race form entry");
                            continue;
                        }

                        if (auto form = ParseRaceForm(item.as_string())) {
                            parsed.push_back(std::move(*form));
                        } else {
                            SKSE::log::warn("Ignoring invalid race form {}", item.as_string());
                        }
                    }
                } else {
                    SKSE::log::warn("Races.Forms must be an array");
                }
            }

            if (!parsed.empty()) {
                a_settings.raceForms = std::move(parsed);
            }
        }

        void ReadArmorCheckRace(const toml::value& a_table, Settings& a_settings) {
            const auto value = toml::find_or(a_table, "ArmorCheckRace", std::string{});
            if (value.empty()) {
                return;
            }

            if (auto form = ParseRaceForm(value)) {
                a_settings.armorCheckRace = std::move(*form);
            } else {
                SKSE::log::warn("Ignoring invalid ArmorAddonFilter.ArmorCheckRace {}", value);
            }
        }
    }  // namespace

    void Config::Load() {
        g_settings = DefaultSettings();

        const auto path = std::filesystem::current_path() / "Data" / "SKSE" / "Plugins" / "ArmorRacePatcher.toml";
        SKSE::log::info("Loading config: {}", path.string());

        if (!std::filesystem::exists(path)) {
            SKSE::log::info("Config file not found; using built-in defaults");
            spdlog::set_level(g_settings.debugLogging ? spdlog::level::debug : spdlog::level::info);
            return;
        }

        try {
            const auto data = toml::parse(path.string());

            if (data.contains("General") && data.at("General").is_table()) {
                const auto& table = data.at("General");
                g_settings.enableArmorAddonPatching = toml::find_or(table, "EnableArmorAddonPatching", g_settings.enableArmorAddonPatching);
                g_settings.enableMeshRedirection = toml::find_or(table, "EnableMeshRedirection", g_settings.enableMeshRedirection);
            }

            if (data.contains("Logging") && data.at("Logging").is_table()) {
                const auto& table = data.at("Logging");
                g_settings.debugLogging = toml::find_or(table, "Debug", g_settings.debugLogging);
            }

            if (data.contains("Races") && data.at("Races").is_table()) {
                ReadRaces(data.at("Races"), g_settings);
            }

            if (data.contains("Paths") && data.at("Paths").is_table()) {
                const auto& table = data.at("Paths");
                ReadStringArray(table, "Folders", g_settings.meshFolders);
                ReadPathStyles(table, g_settings);
            }

            if (data.contains("ArmorAddonFilter") && data.at("ArmorAddonFilter").is_table()) {
                const auto& table = data.at("ArmorAddonFilter");
                g_settings.requireArmorCheckRace = toml::find_or(table, "RequireArmorCheckRace", g_settings.requireArmorCheckRace);
                ReadArmorCheckRace(table, g_settings);
                ReadSlots(table, "AllowSlots", g_settings.allowedSlots);
                ReadSlots(table, "BlockSlots", g_settings.blockedSlots);
            }

            spdlog::set_level(g_settings.debugLogging ? spdlog::level::debug : spdlog::level::info);
        } catch (const toml::syntax_error& e) {
            SKSE::log::error("TOML syntax error: {}", e.what());
        } catch (const std::exception& e) {
            SKSE::log::error("Config load error: {}", e.what());
        }

        SKSE::log::info("Config: races={}, mesh folders={}, path styles={}, ARMA patching={}, mesh redirection={}, require check race={}", g_settings.raceForms.size(), g_settings.meshFolders.size(),
            g_settings.pathStyles.size(), g_settings.enableArmorAddonPatching, g_settings.enableMeshRedirection, g_settings.requireArmorCheckRace);
    }

    const Settings& Config::Get() { return g_settings; }
}  // namespace ArmorPatcher
