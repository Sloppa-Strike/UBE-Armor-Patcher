#pragma once

namespace ArmorPatcher::ArmorAddonPatcher {
    void ResolveRaces();
    void PatchArmorAddons();

    [[nodiscard]] bool IsUBERace(const RE::TESRace* a_race);
}  // namespace ArmorPatcher::ArmorAddonPatcher
