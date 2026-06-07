#include "ArmorAddonPatcher.h"

#include <algorithm>
#include <vector>

#include "Config.h"

namespace ArmorPatcher::ArmorAddonPatcher {
    namespace {
        std::vector<RE::TESRace*> g_ubeRaces;
        RE::TESRace* g_armorCheckRace = nullptr;

        [[nodiscard]] bool HasRace(const RE::TESObjectARMA* a_arma, const RE::TESRace* a_race) {
            if (!a_arma || !a_race) {
                return false;
            }

            if (a_arma->race == a_race) {
                return true;
            }

            for (const auto* race : a_arma->additionalRaces) {
                if (race == a_race) {
                    return true;
                }
            }

            return false;
        }

        [[nodiscard]] bool HasAnySlot(const RE::TESObjectARMA* a_arma, const std::vector<RE::BIPED_MODEL::BipedObjectSlot>& a_slots) {
            return std::ranges::any_of(a_slots, [a_arma](const auto a_slot) { return a_arma->HasPartOf(a_slot); });
        }

        [[nodiscard]] bool ShouldPatch(const RE::TESObjectARMA* a_arma) {
            if (!a_arma) {
                return false;
            }

            const auto& config = Config::Get();
            if (!config.allowedSlots.empty() && !HasAnySlot(a_arma, config.allowedSlots)) {
                return false;
            }

            if (!config.blockedSlots.empty() && HasAnySlot(a_arma, config.blockedSlots)) {
                return false;
            }

            // The HORRORS i witnessed by commenting this out during testing. THE HORRORS
            if (config.requireArmorCheckRace && !HasRace(a_arma, g_armorCheckRace)) {
                return false;
            }

            return true;
        }
    }  // namespace

    bool IsUBERace(const RE::TESRace* a_race) { return a_race && std::ranges::find(g_ubeRaces, a_race) != g_ubeRaces.end(); }

    void ResolveRaces() {
        g_ubeRaces.clear();
        g_armorCheckRace = nullptr;

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("TESDataHandler unavailable; cannot resolve races");
            return;
        }

        const auto& config = Config::Get();
        for (const auto& raceForm : config.raceForms) {
            if (raceForm.plugin.empty() || raceForm.formID == 0) {
                continue;
            }

            auto* race = dataHandler->LookupForm<RE::TESRace>(raceForm.formID, raceForm.plugin);
            if (!race) {
                SKSE::log::warn("Failed to resolve UBE race {}|{:06X}", raceForm.plugin, raceForm.formID);
                continue;
            }

            if (!IsUBERace(race)) {
                g_ubeRaces.push_back(race);
                SKSE::log::debug("Resolved UBE race {}|{:06X} -> {:08X}", raceForm.plugin, raceForm.formID, race->GetFormID());
            }
        }

        if (config.requireArmorCheckRace) {
            g_armorCheckRace = dataHandler->LookupForm<RE::TESRace>(config.armorCheckRace.formID, config.armorCheckRace.plugin);
            if (!g_armorCheckRace) {
                SKSE::log::warn("Failed to resolve armor check race {}|{:06X}; ARMA patching will fail closed", config.armorCheckRace.plugin, config.armorCheckRace.formID);
            }
        }

        SKSE::log::info("Resolved {} UBE race(s)", g_ubeRaces.size());
    }

    // Easier to just patch everything on bootup. It's still possible to just inject it lazily like we do slots, but idc
    void PatchArmorAddons() {
        const auto& config = Config::Get();
        if (!config.enableArmorAddonPatching) {
            SKSE::log::info("ARMA patching disabled by config");
            return;
        }

        if (g_ubeRaces.empty()) {
            SKSE::log::warn("No UBE races resolved; skipping ARMA patch");
            return;
        }

        if (config.requireArmorCheckRace && !g_armorCheckRace) {
            SKSE::log::warn("Armor check race is required but unresolved; skipping ARMA patch");
            return;
        }

        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            SKSE::log::error("TESDataHandler unavailable; cannot patch ARMAs");
            return;
        }

        std::size_t checked = 0;
        std::size_t eligible = 0;
        std::size_t patched = 0;
        std::size_t added = 0;

        for (auto* arma : dataHandler->GetFormArray<RE::TESObjectARMA>()) {
            if (!arma) {
                continue;
            }

            ++checked;
            if (!ShouldPatch(arma)) {
                continue;
            }

            ++eligible;
            bool changed = false;

            for (auto* race : g_ubeRaces) {
                if (!HasRace(arma, race)) {
                    arma->additionalRaces.push_back(race);
                    changed = true;
                    ++added;
                }
            }

            if (changed) {
                ++patched;
            }
        }

        SKSE::log::info("ARMA patch complete: checked={}, eligible={}, patched={}, race entries added={}", checked, eligible, patched, added);
    }
}  // namespace ArmorPatcher::ArmorAddonPatcher
