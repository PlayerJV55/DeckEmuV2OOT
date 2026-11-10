// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "common/logging.h"
#include "video_core/host1x/syncpoint_manager.h"

namespace Tegra {

namespace Host1x {

namespace {

// Increment ids come from command streams (fence actions, the 3D syncpoint method), which hold
// any value once a game has overwritten commands before the GPU thread read them. Indexing the
// syncpoint arrays with one ran off them and crashed.
bool IsValidSyncpoint(u32 syncpoint_id, size_t count) {
    if (syncpoint_id < count) {
        return true;
    }
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_WARNING(HW_GPU, "Ignoring an increment of syncpoint {} (there are {})", syncpoint_id,
                    count);
    }
    return false;
}

} // Anonymous namespace

SyncpointManager::ActionHandle SyncpointManager::RegisterAction(
    std::atomic<u32>& syncpoint, std::list<RegisteredAction>& action_storage, u32 expected_value,
    std::function<void()>&& action) {
    if (syncpoint.load(std::memory_order_acquire) >= expected_value) {
        action();
        return {};
    }

    {
        std::scoped_lock lk(guard);
        if (syncpoint.load(std::memory_order_relaxed) < expected_value) {
            auto it = action_storage.begin();
            while (it != action_storage.end()) {
                if (it->expected_value >= expected_value) {
                    break;
                }
                ++it;
            }
            const ActionHandle id = ++last_action_id;
            action_storage.emplace(it, id, expected_value, std::move(action));
            return id;
        }
    }
    action();
    return {};
}

void SyncpointManager::DeregisterAction(std::list<RegisteredAction>& action_storage,
                                        ActionHandle handle) {
    if (handle == 0) {
        return; // It ran inside RegisterAction
    }
    std::unique_lock lk(guard);

    // The deregister isn't called from a locked context, so the action may have run and been
    // erased since. Its iterator was then left dangling (Debug builds abort comparing it), and a
    // new action at the same address could have been erased instead: look the action up by id.
    for (auto it = action_storage.begin(); it != action_storage.end(); it++) {
        if (it->id == handle) {
            action_storage.erase(it);
            return;
        }
    }
    // An Increment may have taken it out and be running it with the guard released. Return only
    // once it ran, so it never runs after its cancel (nvhost_ctrl stores the event state after
    // it), unless this thread is the one running it (a cancel from inside the action).
    const auto current_thread = std::this_thread::get_id();
    action_done_cv.wait(lk, [&] {
        return std::ranges::none_of(running_actions, [&](const RunningAction& running) {
            return running.id == handle && running.thread != current_thread;
        });
    });
}

void SyncpointManager::DeregisterGuestAction(u32 syncpoint_id, ActionHandle handle) {
    DeregisterAction(guest_action_storage[syncpoint_id], handle);
}

void SyncpointManager::DeregisterHostAction(u32 syncpoint_id, ActionHandle handle) {
    DeregisterAction(host_action_storage[syncpoint_id], handle);
}

void SyncpointManager::IncrementGuest(u32 syncpoint_id) {
    if (!IsValidSyncpoint(syncpoint_id, NUM_MAX_SYNCPOINTS)) {
        return;
    }
    Increment(syncpoints_guest[syncpoint_id], wait_guest_cv, guest_action_storage[syncpoint_id]);
}

void SyncpointManager::IncrementHost(u32 syncpoint_id) {
    if (!IsValidSyncpoint(syncpoint_id, NUM_MAX_SYNCPOINTS)) {
        return;
    }
    Increment(syncpoints_host[syncpoint_id], wait_host_cv, host_action_storage[syncpoint_id]);
}

void SyncpointManager::WaitGuest(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_guest[syncpoint_id], wait_guest_cv, expected_value);
}

void SyncpointManager::WaitHost(u32 syncpoint_id, u32 expected_value) {
    Wait(syncpoints_host[syncpoint_id], wait_host_cv, expected_value);
}

void SyncpointManager::Increment(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                                 std::list<RegisteredAction>& action_storage) {
    auto new_value{syncpoint.fetch_add(1, std::memory_order_acq_rel) + 1};

    std::unique_lock lk(guard);
    wait_cv.notify_all();
    // Due actions run in value order with the guard released, one at a time. The composite action
    // can block in AcquireNextImage, and under the guard every other syncpoint user (the fence
    // thread's Increment, the guest's RegisterAction) waited behind it: Donkey Kong Bananza froze
    // with presents stopped. A cancel still removes a due action that has not been taken out.
    const auto current_thread = std::this_thread::get_id();
    while (!action_storage.empty() && action_storage.front().expected_value <= new_value) {
        const ActionHandle id = action_storage.front().id;
        std::function<void()> action = std::move(action_storage.front().action);
        action_storage.pop_front();
        running_actions.push_back({id, current_thread});
        lk.unlock();
        action();
        action = nullptr;
        lk.lock();
        std::erase_if(running_actions,
                      [id](const RunningAction& running) { return running.id == id; });
        action_done_cv.notify_all();
    }
}

void SyncpointManager::Wait(std::atomic<u32>& syncpoint, std::condition_variable& wait_cv,
                            u32 expected_value) {
    const auto pred = [&]() { return syncpoint.load(std::memory_order_acquire) >= expected_value; };
    if (pred()) {
        return;
    }

    std::unique_lock lk(guard);
    wait_cv.wait(lk, pred);
}

} // namespace Host1x

} // namespace Tegra
