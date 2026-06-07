#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace ArmorPatcher::Slot53EntryGC {
    struct Entry {
        std::uint32_t actorFormID{0};
        RE::TESForm* owner{nullptr};
        RE::TESObjectARMA* arma{nullptr};
        RE::TESModelTextureSwap* model{nullptr};
        void* skinTexture{nullptr};
    };

    [[nodiscard]] bool IsArmorStillWorn(RE::Actor* a_actor, const Entry& a_entry);
    void Start(std::recursive_mutex& a_lock, std::vector<Entry>& a_entries);
}  // namespace ArmorPatcher::Slot53EntryGC
