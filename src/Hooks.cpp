#include "Hooks.h"

#include <Windows.h>
#include <detours/detours.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "ArmorAddonPatcher.h"
#include "Config.h"
#include "Paths.h"
#include "Slot53EntryGC.h"

namespace ArmorPatcher::Hooks {
    namespace {
        constexpr REL::VariantID kBipedModelBuilder{15501, 15678, 0x1d7450};
        constexpr REL::VariantID kInitWornArmor{17392, 17792, 0x239E00};
        constexpr REL::VariantID kRequestModel2{74039, 75781, 0xD773A0};
        constexpr std::array<REL::VariantOffset, 3> kRequestModel2CallsiteOffsets{
            REL::VariantOffset{0x56F, 0x6D1, 0x5ED},  // SE 1401C74FF, AE 140213C51, VR 1401D7A3D
            REL::VariantOffset{0x81D, 0x977, 0x923},  // SE 1401C77AD, AE 140213EF7, VR 1401D7D73
            REL::VariantOffset{0x8C4, 0xA7F, 0x9C4},  // SE 1401C7854, AE 140213FFF, VR 1401D7E14
        };

        // xEdit slot numbers are mask bit + 30: 32 Body -> bit 2, 53 Unnamed -> bit 23.
        constexpr std::uint32_t kBodySlotIndex = 2;
        constexpr std::uint32_t kSlot53Index = 23;
        constexpr std::uint32_t kBodySlotMask = 1u << kBodySlotIndex;
        constexpr std::uintptr_t kBipedSlotStride = 0x78;
        constexpr std::uintptr_t kBipedSlotOwnerOffset = 0x10;
        constexpr std::uintptr_t kBipedSlotArmorAddonOffset = 0x18;
        constexpr std::uintptr_t kBipedSlotModelOffset = 0x20;
        constexpr std::uintptr_t kBipedSlotSkinTextureOffset = 0x28;
        constexpr std::uint8_t kCallOpcode = 0xE8;

        struct BipedContext {
            RE::Actor* actor{nullptr};
            RE::BipedAnim* biped{nullptr};
            RE::TESRace* race{nullptr};
        };

        struct BipedSlotEntry {
            RE::TESForm* owner{nullptr};
            RE::TESObjectARMA* arma{nullptr};
            RE::TESModelTextureSwap* model{nullptr};
            void* skinTexture{nullptr};
        };

        using RememberedSlot53Entry = Slot53EntryGC::Entry;

        thread_local BipedContext* g_currentContext = nullptr;
        std::vector<RememberedSlot53Entry> g_rememberedSlot53Entries;
        std::recursive_mutex g_slot53Lock;

        using BipedModelBuilder_t = void (*)(RE::BipedAnim* a_biped, float a_weightOverride, bool a_arg3);
        using InitWornArmor_t = void (*)(RE::TESObjectARMO* a_armor, RE::TESRace* a_race, RE::BSTSmartPointer<RE::BipedAnim>* a_biped, RE::SEX a_sex);
        using RequestModel2_t = int (*)(char* a_path, void* a_outModel, void* a_params);

        BipedModelBuilder_t g_bipedModelBuilder = nullptr;
        InitWornArmor_t g_initWornArmor = nullptr;
        RequestModel2_t g_requestModel2 = nullptr;

        [[nodiscard]] RE::Actor* ResolveActor(RE::BipedAnim* a_biped) {
            if (!a_biped) {
                return nullptr;
            }

            const auto refr = a_biped->actorRef.get();
            if (!refr) {
                return nullptr;
            }

            return refr->As<RE::Actor>();
        }

        [[nodiscard]] bool IsCallInstruction(std::uintptr_t a_address) { return *reinterpret_cast<const std::uint8_t*>(a_address) == kCallOpcode; }

        [[nodiscard]] std::uint32_t GetFormID(const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; }

        [[nodiscard]] std::uint32_t GetFormTypeValue(const RE::TESForm* a_form) { return a_form ? static_cast<std::uint32_t>(a_form->GetFormType()) : 0; }

        [[nodiscard]] std::uintptr_t GetBipedSlotBase(RE::BipedAnim* a_biped, std::uint32_t a_slotIndex) { return reinterpret_cast<std::uintptr_t>(a_biped) + (a_slotIndex * kBipedSlotStride); }

        template <class T>
        [[nodiscard]] T* ReadBipedSlotPointer(std::uintptr_t a_slotBase, std::uintptr_t a_offset) {
            return *reinterpret_cast<T* const*>(a_slotBase + a_offset);
        }

        template <class T>
        void WriteBipedSlotPointer(std::uintptr_t a_slotBase, std::uintptr_t a_offset, T* a_value) {
            *reinterpret_cast<T**>(a_slotBase + a_offset) = a_value;
        }

        [[nodiscard]] BipedSlotEntry ReadBipedSlotEntry(RE::BipedAnim* a_biped, std::uint32_t a_slotIndex) {
            const auto slotBase = GetBipedSlotBase(a_biped, a_slotIndex);
            return {
                ReadBipedSlotPointer<RE::TESForm>(slotBase, kBipedSlotOwnerOffset),
                ReadBipedSlotPointer<RE::TESObjectARMA>(slotBase, kBipedSlotArmorAddonOffset),
                ReadBipedSlotPointer<RE::TESModelTextureSwap>(slotBase, kBipedSlotModelOffset),
                ReadBipedSlotPointer<void>(slotBase, kBipedSlotSkinTextureOffset),
            };
        }

        void WriteBipedSlotEntry(RE::BipedAnim* a_biped, std::uint32_t a_slotIndex, const BipedSlotEntry& a_entry) {
            const auto slotBase = GetBipedSlotBase(a_biped, a_slotIndex);
            WriteBipedSlotPointer(slotBase, kBipedSlotOwnerOffset, a_entry.owner);
            WriteBipedSlotPointer(slotBase, kBipedSlotArmorAddonOffset, a_entry.arma);
            WriteBipedSlotPointer(slotBase, kBipedSlotModelOffset, a_entry.model);
            WriteBipedSlotPointer(slotBase, kBipedSlotSkinTextureOffset, a_entry.skinTexture);
        }

        [[nodiscard]] bool HasSameArmorModel(const BipedSlotEntry& a_lhs, const BipedSlotEntry& a_rhs) { return a_lhs.owner == a_rhs.owner && a_lhs.arma == a_rhs.arma && a_lhs.model == a_rhs.model; }

        RememberedSlot53Entry* FindRememberedSlot53Entry(std::uint32_t a_actorFormID) {
            const auto entry = std::ranges::find(g_rememberedSlot53Entries, a_actorFormID, &RememberedSlot53Entry::actorFormID);
            return entry != g_rememberedSlot53Entries.end() ? std::addressof(*entry) : nullptr;
        }

        void ForgetRememberedSlot53Entry(std::uint32_t a_actorFormID) {
            const auto entry = std::ranges::find(g_rememberedSlot53Entries, a_actorFormID, &RememberedSlot53Entry::actorFormID);
            if (entry == g_rememberedSlot53Entries.end()) {
                return;
            }

            SKSE::log::debug("UBE remembered slot 53 entry dropped: actor={:08X}, owner={:08X}, arma={:08X}", entry->actorFormID, GetFormID(entry->owner), GetFormID(entry->arma));
            g_rememberedSlot53Entries.erase(entry);
        }

        void RememberSlot53Entry(RE::Actor* a_actor, const BipedSlotEntry& a_slot) {
            if (!a_actor || !a_slot.owner || !a_slot.arma || !a_slot.model || !a_slot.owner->Is(RE::FormType::Armor)) {
                return;
            }

            const auto actorFormID = a_actor->GetFormID();
            auto* entry = FindRememberedSlot53Entry(actorFormID);
            if (entry) {
                if (entry->owner == a_slot.owner && entry->arma == a_slot.arma && entry->model == a_slot.model && entry->skinTexture == a_slot.skinTexture) {
                    return;
                }

                entry->owner = a_slot.owner;
                entry->arma = a_slot.arma;
                entry->model = a_slot.model;
                entry->skinTexture = a_slot.skinTexture;
            } else {
                g_rememberedSlot53Entries.push_back({actorFormID, a_slot.owner, a_slot.arma, a_slot.model, a_slot.skinTexture});
            }

            const auto* modelPath = a_slot.model->GetModel();
            SKSE::log::debug("UBE slot 53 entry remembered: actor={:08X}, owner={:08X}, arma={:08X}, modelPath={}", actorFormID, GetFormID(a_slot.owner), GetFormID(a_slot.arma),
                modelPath && *modelPath ? modelPath : "<none>");
        }

        void ReapplyRememberedSlot53Entry(RE::BipedAnim* a_biped, RE::Actor* a_actor, const char* a_trigger) {
            if (!a_biped || !a_actor) {
                return;
            }

            const auto actorFormID = a_actor->GetFormID();
            auto* entry = FindRememberedSlot53Entry(actorFormID);
            if (!entry) {
                return;
            }

            if (!Slot53EntryGC::IsArmorStillWorn(a_actor, *entry)) {
                ForgetRememberedSlot53Entry(actorFormID);
                return;
            }

            const BipedSlotEntry remembered{entry->owner, entry->arma, entry->model, entry->skinTexture};
            const auto previous = ReadBipedSlotEntry(a_biped, kSlot53Index);
            if (HasSameArmorModel(previous, remembered)) {
                return;
            }

            const auto* previousModelPath = previous.model ? previous.model->GetModel() : nullptr;
            const auto* modelPath = entry->model->GetModel();
            WriteBipedSlotEntry(a_biped, kSlot53Index, remembered);

            SKSE::log::debug(
                "UBE remembered slot 53 entry reapplied: actor={:08X}, trigger={}, owner={:08X}, arma={:08X}, modelPath={}, previousOwner={:08X}, previousOwnerType={}, previousArma={:08X}, previousModelPath={}",
                actorFormID, a_trigger, GetFormID(entry->owner), GetFormID(entry->arma), modelPath && *modelPath ? modelPath : "<none>", GetFormID(previous.owner), GetFormTypeValue(previous.owner),
                GetFormID(previous.arma), previousModelPath && *previousModelPath ? previousModelPath : "<none>");
        }

        void MirrorRequestedBodySlotToSlot53(RE::BipedAnim* a_biped, RE::Actor* a_actor, const char* a_path) {
            if (!a_biped || !a_path || !*a_path) {
                return;
            }

            const auto body = ReadBipedSlotEntry(a_biped, kBodySlotIndex);
            const auto* bodyModelPath = body.model ? body.model->GetModel() : nullptr;
            if (!body.owner || !body.arma || !bodyModelPath || _stricmp(bodyModelPath, a_path) != 0) {
                return;
            }

            const auto bodyArmaSlots = body.arma->bipedModelData.bipedObjectSlots.underlying();
            if ((bodyArmaSlots & kBodySlotMask) == 0) {
                return;
            }

            if (!body.owner->Is(RE::FormType::Armor)) {
                SKSE::log::debug("UBE slot 53 mirror skipped: Body owner is not ARMO path={}, ownerForm={:08X}, ownerType={}, armaForm={:08X}, armaSlots=0x{:08X}", a_path, GetFormID(body.owner),
                    GetFormTypeValue(body.owner), GetFormID(body.arma), bodyArmaSlots);
                return;
            }

            const auto previous = ReadBipedSlotEntry(a_biped, kSlot53Index);
            if (HasSameArmorModel(previous, body)) {
                RememberSlot53Entry(a_actor, body);
                return;
            }

            const auto* previousModelPath = previous.model ? previous.model->GetModel() : nullptr;
            WriteBipedSlotEntry(a_biped, kSlot53Index, body);
            RememberSlot53Entry(a_actor, body);

            SKSE::log::debug("UBE Body slot mirrored to slot 53: path={}, ownerForm={:08X}, armaForm={:08X}, armaSlots=0x{:08X}, previousOwner={:08X}, previousOwnerType={}, previousArma={:08X}, previousModelPath={}",
                a_path, GetFormID(body.owner), GetFormID(body.arma), bodyArmaSlots, GetFormID(previous.owner), GetFormTypeValue(previous.owner), GetFormID(previous.arma),
                previousModelPath && *previousModelPath ? previousModelPath : "<none>");
        }

        class ScopedBipedContext {
           public:
            explicit ScopedBipedContext(RE::BipedAnim* a_biped) : _previous(g_currentContext) {
                _context.biped = a_biped;
                _context.actor = ResolveActor(a_biped);
                _context.race = _context.actor ? _context.actor->GetRace() : nullptr;
                g_currentContext = &_context;
            }

            ~ScopedBipedContext() { g_currentContext = _previous; }

            ScopedBipedContext(const ScopedBipedContext&) = delete;
            ScopedBipedContext& operator=(const ScopedBipedContext&) = delete;

           private:
            BipedContext _context;
            BipedContext* _previous;
        };

        void BipedModelBuilder_Hook(RE::BipedAnim* a_biped, float a_weightOverride, bool a_arg3) {
            const std::lock_guard lock(g_slot53Lock);
            const ScopedBipedContext context(a_biped);
            g_bipedModelBuilder(a_biped, a_weightOverride, a_arg3);
        }

        void InitWornArmor_Hook(RE::TESObjectARMO* a_armor, RE::TESRace* a_race, RE::BSTSmartPointer<RE::BipedAnim>* a_biped, RE::SEX a_sex) {
            const std::lock_guard lock(g_slot53Lock);
            auto* biped = a_biped ? a_biped->get() : nullptr;
            g_initWornArmor(a_armor, a_race, a_biped, a_sex);
            auto* actor = g_currentContext && g_currentContext->actor ? g_currentContext->actor : ResolveActor(biped);
            ReapplyRememberedSlot53Entry(biped, actor, "InitWornArmor");
        }

        int RequestModel2_Hook(char* a_path, void* a_outModel, void* a_params) {
            const auto* context = g_currentContext;
            if (!a_path || !*a_path || !context || !ArmorAddonPatcher::IsUBERace(context->race)) {
                return g_requestModel2(a_path, a_outModel, a_params);
            }

            auto redirect = Paths::FindRedirectPath(a_path);
            if (!redirect) {
                return g_requestModel2(a_path, a_outModel, a_params);
            }

            ReapplyRememberedSlot53Entry(context->biped, context->actor, a_path);
            MirrorRequestedBodySlotToSlot53(context->biped, context->actor, a_path);
            SKSE::log::debug("UBE mesh redirect: {} -> {}", a_path, *redirect);
            return g_requestModel2(redirect->data(), a_outModel, a_params);
        }

        template <class T>
        [[nodiscard]] bool AttachDetour(T& a_original, std::uintptr_t a_address, T a_hook, const char* a_name) {
            a_original = reinterpret_cast<T>(a_address);

            if (const auto result = DetourTransactionBegin(); result != NO_ERROR) {
                SKSE::log::error("{} DetourTransactionBegin failed: {}", a_name, result);
                return false;
            }

            if (const auto result = DetourUpdateThread(::GetCurrentThread()); result != NO_ERROR) {
                SKSE::log::error("{} DetourUpdateThread failed: {}", a_name, result);
                DetourTransactionAbort();
                return false;
            }

            auto target = reinterpret_cast<PVOID*>(&a_original);
            if (const auto result = DetourAttach(target, reinterpret_cast<PVOID>(a_hook)); result != NO_ERROR) {
                SKSE::log::error("{} DetourAttach failed: {}", a_name, result);
                DetourTransactionAbort();
                return false;
            }

            if (const auto result = DetourTransactionCommit(); result != NO_ERROR) {
                SKSE::log::error("{} DetourTransactionCommit failed: {}", a_name, result);
                a_original = nullptr;
                return false;
            }

            return true;
        }

        [[nodiscard]] std::size_t InstallRequestModel2Callsites(std::uintptr_t a_bipedModelBuilder) {
            const auto requestModel2 = kRequestModel2.address();
            if (!requestModel2) {
                SKSE::log::warn("Mesh redirection callsites skipped: RequestModel2 Address Library ID is not known for this runtime. Wtf are you using?");
                return 0;
            }

            g_requestModel2 = reinterpret_cast<RequestModel2_t>(requestModel2);
            auto& trampoline = SKSE::GetTrampoline();
            std::size_t installed = 0;
            for (const auto& callsiteOffset : kRequestModel2CallsiteOffsets) {
                const auto offset = callsiteOffset.offset();
                if (!offset) {
                    continue;
                }

                const auto callsite = a_bipedModelBuilder + offset;
                // This function is MASSIVE, so it's possible some version between SE, AE, and VR are slightly offset (Hopefully not)
                if (!IsCallInstruction(callsite)) {
                    SKSE::log::error("Mesh redirection callsite skipped: expected CALL at 0x{:X}", callsite);
                    continue;
                }

                trampoline.write_call<5>(callsite, RequestModel2_Hook);
                ++installed;
            }

            return installed;
        }
    }  // namespace

    void Install() {
        bool installedInitWornArmor = false;
        if (const auto initWornArmor = kInitWornArmor.address()) {
            installedInitWornArmor = AttachDetour(g_initWornArmor, initWornArmor, InitWornArmor_Hook, "InitWornArmor");
        } else {
            SKSE::log::warn("Remembered slot 53 reapply skipped: InitWornArmor Address Library ID is not known for this runtime. Wtf are you using?");
        }

        bool installedBipedBuilder = false;
        std::size_t installedCalls = 0;
        if (Config::Get().enableMeshRedirection) {
            if (const auto bipedModelBuilder = kBipedModelBuilder.address()) {
                installedBipedBuilder = AttachDetour(g_bipedModelBuilder, bipedModelBuilder, BipedModelBuilder_Hook, "Biped model builder");
                if (installedBipedBuilder) {
                    installedCalls = InstallRequestModel2Callsites(bipedModelBuilder);
                }
            } else {
                SKSE::log::warn("Biped model builder hook skipped: Address Library ID is not known for this runtime. Wtf are you using?");
            }
        }

        if (!installedInitWornArmor && !installedBipedBuilder) {
            SKSE::log::warn("Biped hooks skipped: no hook was installed for this runtime :(");
            return;
        }

        if (installedBipedBuilder) {
            Slot53EntryGC::Start(g_slot53Lock, g_rememberedSlot53Entries);
        }

        SKSE::log::info("Biped hooks installed: InitWornArmor={}, biped builder={}, mesh callsites={}", installedInitWornArmor, installedBipedBuilder, installedCalls);
    }
}  // namespace ArmorPatcher::Hooks
