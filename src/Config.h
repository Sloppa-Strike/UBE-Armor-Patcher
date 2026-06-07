#pragma once

#include <string>
#include <vector>

#include "RE/B/BGSBipedObjectForm.h"
#include "RE/B/BSCoreTypes.h"

namespace ArmorPatcher {
    enum class PathStyle { kPrefix, kSibling };

    struct RaceForm {
        std::string plugin;
        RE::FormID formID{0};
    };

    struct Settings {
        bool enableArmorAddonPatching{true};
        bool enableMeshRedirection{true};
        bool debugLogging{false};
        bool requireArmorCheckRace{true};

        RaceForm armorCheckRace{"Skyrim.esm", 0x013746};

        std::vector<RaceForm> raceForms;
        std::vector<std::string> meshFolders;
        std::vector<PathStyle> pathStyles;
        std::vector<RE::BIPED_MODEL::BipedObjectSlot> allowedSlots;
        std::vector<RE::BIPED_MODEL::BipedObjectSlot> blockedSlots;
    };

    class Config {
       public:
        static void Load();
        [[nodiscard]] static const Settings& Get();
    };
}  // namespace ArmorPatcher
