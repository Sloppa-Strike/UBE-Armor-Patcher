#include "ArmorAddonPatcher.h"
#include "Config.h"
#include "Hooks.h"

namespace {
    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
            ArmorPatcher::ArmorAddonPatcher::ResolveRaces();
            ArmorPatcher::ArmorAddonPatcher::PatchArmorAddons();
        }
    }
}  // namespace

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);
    spdlog::set_level(spdlog::level::info);

    SKSE::log::info("{} loading", SKSE::PluginDeclaration::GetSingleton()->GetName());

    ArmorPatcher::Config::Load();

    SKSE::AllocTrampoline(1 << 12);

    if (const auto* messaging = SKSE::GetMessagingInterface(); !messaging->RegisterListener(OnSKSEMessage)) {
        SKSE::log::error("Failed to register SKSE messaging listener");
        return false;
    }

    ArmorPatcher::Hooks::Install();

    SKSE::log::info("{} loaded", SKSE::PluginDeclaration::GetSingleton()->GetName());
    return true;
}
