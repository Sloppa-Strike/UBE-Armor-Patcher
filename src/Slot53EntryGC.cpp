#include "Slot53EntryGC.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <stop_token>
#include <thread>

// Timing happens off-thread, Skyrim form access and pruning run through the SKSE task queue.
// Todo: Look into maybe just making this run every time a new entry is added so I don't need to spin a thread..
namespace ArmorPatcher::Slot53EntryGC {
    namespace {
        constexpr std::uint32_t kBodySlotMask = 1u << 2;
        constexpr auto kInterval = std::chrono::seconds(30);

        struct State {
            std::recursive_mutex& lock;
            std::vector<Entry>& entries;
            std::atomic_bool taskQueued{false};
            std::jthread worker;
        };

        // Todo: Verify this doesn't break anything for the player actor..
        [[nodiscard]] const char* GetStaleReason(const Entry& a_entry) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_entry.actorFormID);
            if (!actor) {
                return "actor form no longer resolves";
            }

            if (!actor->Is3DLoaded()) {
                return "actor 3D is unloaded";
            }

            if (!IsArmorStillWorn(actor, a_entry)) {
                return "remembered armor is no longer worn";
            }

            return nullptr;
        }

        void Prune(State& a_state) {
            std::size_t removed = 0;
            for (auto it = a_state.entries.begin(); it != a_state.entries.end();) {
                if (const auto* reason = GetStaleReason(*it)) {
                    SKSE::log::debug("UBE remembered slot 53 GC dropped: actor={:08X}, reason={}", it->actorFormID, reason);
                    it = a_state.entries.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }

            if (removed > 0) {
                SKSE::log::debug("UBE remembered slot 53 GC pruned {} entr{}; remaining={}", removed, removed == 1 ? "y" : "ies", a_state.entries.size());
            }
        }

        void Queue(State& a_state) {
            const auto* task = SKSE::GetTaskInterface();
            if (!task) {
                a_state.taskQueued.store(false, std::memory_order_release);
                return;
            }

            task->AddTask([state = &a_state] {
                try {
                    const std::lock_guard<std::recursive_mutex> lock(state->lock);
                    Prune(*state);
                } catch (const std::exception& e) {
                    SKSE::log::error("UBE remembered slot 53 GC failed: {}", e.what());
                } catch (...) {
                    SKSE::log::error("UBE remembered slot 53 GC failed");
                }

                state->taskQueued.store(false, std::memory_order_release);
            });
        }

        void Worker(State& a_state, std::stop_token a_stopToken) {
            std::mutex timerMutex;
            std::condition_variable_any timer;
            std::unique_lock timerLock(timerMutex);
            while (!a_stopToken.stop_requested()) {

                timer.wait_for(timerLock, a_stopToken, kInterval, [] { return false; });

                if (a_stopToken.stop_requested()) {
                    break;
                }

                if (!a_state.taskQueued.exchange(true, std::memory_order_acq_rel)) {
                    Queue(a_state);
                }
            }
        }
    }  // namespace

    bool IsArmorStillWorn(RE::Actor* a_actor, const Entry& a_entry) {
        if (!a_actor || !a_entry.owner) {
            return false;
        }

        const auto* wornBodyArmor = a_actor->GetWornArmor(static_cast<RE::BIPED_MODEL::BipedObjectSlot>(kBodySlotMask), true);
        return static_cast<const RE::TESForm*>(wornBodyArmor) == a_entry.owner;
    }

    void Start(std::recursive_mutex& a_lock, std::vector<Entry>& a_entries) {
        // The hook-owned lock and entries live for the lifetime of the process.
        static State state{a_lock, a_entries};
        static std::once_flag once;
        std::call_once(once, [] {
            state.worker = std::jthread([state = &state](std::stop_token a_stopToken) { Worker(*state, a_stopToken); });
            SKSE::log::info("UBE remembered slot 53 GC started: interval={}s", kInterval.count());
        });
    }
}  // namespace ArmorPatcher::Slot53EntryGC
