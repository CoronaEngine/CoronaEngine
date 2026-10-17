// NetworkIdentityRegistry is reached from three thread classes:
//   - the CEF UI thread / engine main thread, which register actors and change
//     ownership while the editor and the collaborative session mutate the scene;
//   - the network system thread, which resolves guids from (storage_id, storage_seq)
//     every tick and clears the registry when a session stops.
// Before the registry was synchronized, an insert during that full-table scan
// rehashed the map under a live iterator, and clear() during a scan was immediate
// undefined behaviour ("add or delete an actor while a peer applies changes").
#include <corona/systems/network/network_identity.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", message);
    std::fflush(stderr);
    std::exit(1);
}

using Corona::Network::NetworkIdentityRegistry;
using Corona::Network::StorageID;

/// Registers one actor device plus one profile device so build_identity() succeeds.
std::uintptr_t make_actor(Corona::SharedDataHub& hub) {
    const auto actor = hub.actor_storage().allocate();
    const auto profile = hub.profile_storage().allocate();
    if (auto device = hub.actor_storage().try_acquire_write(actor)) {
        device->profile_handles.push_back(profile);
    }
    return actor;
}

void release_actor(Corona::SharedDataHub& hub, std::uintptr_t actor) {
    hub.actor_storage().deallocate(actor);
}

}  // namespace

int main() {
    auto& hub = Corona::SharedDataHub::instance();
    NetworkIdentityRegistry registry{hub};

    // --- single-threaded contract ------------------------------------------
    const auto actor_a = make_actor(hub);
    const auto actor_b = make_actor(hub);
    require(actor_a != 0 && actor_b != 0, "actors must be allocated");

    require(registry.register_actor("guid-a", actor_a), "registration must succeed");
    require(registry.register_actor("guid-b", actor_b, /*locally_owned=*/false),
            "registration must succeed");
    require(!registry.register_actor("", actor_a), "an empty guid must be rejected");
    require(!registry.register_actor("guid-c", 0), "a null handle must be rejected");

    const auto resolved = registry.resolve_actor("guid-a");
    require(resolved.has_value(), "a registered actor must resolve");
    require(resolved->actor_handle == actor_a, "the resolved handle must match");
    require(resolved->locally_owned, "local ownership must default to true");
    require(!registry.resolve_actor("guid-missing").has_value(),
            "an unknown guid must not resolve");

    const auto seq = registry.storage_seq_for_actor_guid(StorageID::ST_ACTOR, "guid-a");
    require(seq.has_value(), "a registered actor must expose its actor seq");
    require(registry.actor_guid_for_storage_seq(StorageID::ST_ACTOR, *seq) == "guid-a",
            "the storage seq must map back to its guid");
    require(registry.actor_guid_for_storage_seq(StorageID::ST_ACTOR, *seq + 1000).empty(),
            "an unknown storage seq must not map to a guid");
    require(registry.storage_seq_for_actor_guid(StorageID::ST_GEOMETRY, "guid-a") ==
                std::nullopt,
            "a component seq that was never recorded must not be reported");

    const auto ownership = registry.local_ownership_for_storage_seq(StorageID::ST_ACTOR, *seq);
    require(ownership.has_value() && *ownership, "ownership must be readable by seq");

    // Later ownership decisions win and are remembered for re-registration.
    registry.set_actor_ownership("guid-a", false);
    const auto flipped = registry.resolve_actor("guid-a");
    require(flipped.has_value() && !flipped->locally_owned,
            "set_actor_ownership must update an existing entry");
    registry.unregister_actor("guid-a");
    require(!registry.resolve_actor("guid-a").has_value(), "unregister must remove the entry");
    require(registry.register_actor("guid-a", actor_a), "re-registration must succeed");
    const auto re_registered = registry.resolve_actor("guid-a");
    require(re_registered.has_value() && !re_registered->locally_owned,
            "a previous ownership override must survive re-registration");

    registry.clear();
    require(!registry.resolve_actor("guid-b").has_value(), "clear must empty the registry");

    release_actor(hub, actor_a);
    release_actor(hub, actor_b);

    // --- concurrent hammer ---------------------------------------------------
    //
    // What this covers: the registry stays callable and consistent when the
    // editor/collaboration threads register and change ownership while other threads
    // scan it and drop the whole table (session stop).
    //
    // What it does NOT cover: proving the absence of the data race. Without a
    // sanitizer this hammer passes against the previous unsynchronized version too
    // (verified), because a torn unordered_map usually corrupts silently instead of
    // crashing. The fix is justified by the code, not by this test: actors_ was
    // written by the CEF/editor threads (register_actor, set_actor_ownership,
    // unregister_actor, clear) while the network thread scanned it every tick
    // (actor_guid_for_storage_seq, local_ownership_for_storage_seq) and cleared it on
    // session stop, with no lock anywhere.
    const auto hammer_actor = make_actor(hub);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> registrations{0};

    std::thread registrar([&] {
        for (int i = 0; i < 4000 && !stop.load(std::memory_order_relaxed); ++i) {
            const std::string guid = "guid-" + std::to_string(i);
            if (registry.register_actor(guid, hammer_actor, (i % 2) == 0)) {
                registrations.fetch_add(1, std::memory_order_relaxed);
            }
            if ((i % 3) == 0) registry.unregister_actor("guid-" + std::to_string(i / 2));
        }
        stop.store(true, std::memory_order_relaxed);
    });

    std::vector<std::thread> scanners;
    for (int i = 0; i < 2; ++i) {
        scanners.emplace_back([&, i] {
            std::uint64_t spins = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                // Walk the whole table: what the network thread does every tick.
                (void)registry.actor_guid_for_storage_seq(
                    StorageID::ST_ACTOR, static_cast<std::uint64_t>(spins + i));
                (void)registry.local_ownership_for_storage_seq(
                    StorageID::ST_ACTOR, static_cast<std::uint64_t>(spins));
                (void)registry.resolve_actor("guid-" + std::to_string(spins % 512));
                ++spins;
            }
        });
    }

    std::thread owner([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            registry.set_actor_ownership("guid-1", true);
            registry.set_actor_ownership("guid-2", false);
        }
    });

    registrar.join();
    for (auto& scanner : scanners) {
        scanner.join();
    }
    owner.join();

    // Registration has no reason to fail while the actor stays alive: the registry
    // must not lose writes to a concurrent mutation.
    require(registrations.load() == 4000,
            "every registration of a live actor must be recorded");
    require(registry.storage_seq_for_actor_guid(StorageID::ST_ACTOR, "guid-3999").has_value(),
            "the last registered guid must be present after the hammer");

    // A clear() (session stop) must leave the registry empty instead of resurrecting
    // entries from a scan in flight.
    registry.clear();
    require(!registry.resolve_actor("guid-3999").has_value(),
            "clear must empty the registry even after concurrent mutation");
    require(registry.actor_guid_for_storage_seq(StorageID::ST_ACTOR, 0).empty(),
            "an emptied registry must not resolve any storage seq");

    require(registry.register_actor("guid-final", hammer_actor),
            "the registry must stay usable after concurrent mutation");
    const auto final_entry = registry.resolve_actor("guid-final");
    require(final_entry.has_value() && final_entry->actor_handle == hammer_actor,
            "the registry must still hold a consistent entry");

    release_actor(hub, hammer_actor);
    return 0;
}
