#include "../../flipper/profile_record.h"

#include <cassert>

using namespace airrounds;
using namespace airrounds::flipper;

int main() {
    const session::Target first{{0x02, 0, 0, 0, 0, 1}, 6};
    const session::Target second{{0x02, 0, 0, 0, 0, 2}, 11};
    const session::Target third{{0x02, 0, 0, 0, 0, 3}, 13};
    session::Profile profile;
    const bool added = profile.add(first) && profile.add(second) && profile.add(third);
    assert(added);

    const auto encoded = encodeProfile(profile, UINT32_MAX);
    StoredProfile old = decodeProfile(encoded);
    assert(old.valid && old.points.matches(profile) && old.generation == UINT32_MAX);

    session::Profile updated;
    const bool removed = editProfile(profile, 1, nullptr, updated);
    assert(removed);
    assert(updated.size() == 2 && session::sameAddress(updated[1], third));
    const bool duplicate = editProfile(profile, 0, &second, updated);
    assert(!duplicate);
    assert(updated.size() == 2 && session::sameAddress(updated[1], third));
    const bool overCapacity = editProfile(profile, 3, &third, updated);
    assert(!overCapacity);
    const bool badSlot = editProfile(profile, 4, nullptr, updated);
    assert(!badSlot);

    const auto wrapped = encodeProfile(updated, 0);
    StoredProfile newer = decodeProfile(wrapped);
    assert(newer.valid && newestProfile(old, newer) == 1);
    for (size_t index = 0; index < sizeof(wrapped.bytes); ++index) {
        auto damaged = wrapped;
        damaged.bytes[index] ^= 1U;
        const auto invalid = decodeProfile(damaged);
        assert(newestProfile(old, invalid) == 0);
    }

    const auto empty = encodeProfile({}, 1);
    const auto cleared = decodeProfile(empty);
    assert(cleared.valid && cleared.points.size() == 0);
    assert(newestProfile(newer, cleared) == 1);
    assert(newestProfile({}, {}) == -1);
}
