// The checks below must run in every build, including one with NDEBUG.
#undef NDEBUG
#include <cassert>

#include <cstring>

#include "../../flipper/profile_record.h"

using namespace airrounds;
using namespace airrounds::flipper;

namespace {
const session::Target first{{0x02, 0, 0, 0, 0, 1}, 6};
const session::Target second{{0x02, 0, 0, 0, 0, 2}, 11};
const session::Target third{{0x02, 0, 0, 0, 0, 3}, 13};

session::Profile threePoints() {
    session::Profile profile;
    const bool added = profile.add(first) && profile.add(second) && profile.add(third);
    assert(added);
    return profile;
}

// Remove a point, then check edits that must leave the result alone. Returns the two-point profile.
session::Profile editsKeepTheProfileValid(const session::Profile& profile) {
    session::Profile updated;
    const bool removed = editProfile(profile, 1, nullptr, updated);
    assert(removed);
    assert(updated.size() == 2 && session::sameAddress(updated[1], third));
    const bool duplicate = editProfile(profile, 0, &second, updated);
    assert(!duplicate);
    assert(updated.size() == 2 && session::sameAddress(updated[1], third));
    const bool overCapacity = editProfile(profile, 3, &third, updated);
    assert(!overCapacity);
    // The saved profile holds three points, even when the fourth is new.
    const session::Target fourth{{0x02, 0, 0, 0, 0, 4}, 1};
    const bool fourPoints = editProfile(profile, 3, &fourth, updated);
    assert(!fourPoints);
    assert(updated.size() == 2);
    const bool badSlot = editProfile(profile, 4, nullptr, updated);
    assert(!badSlot);
    return updated;
}

void damagedRecordsAreOlder(const StoredProfile& old, const ProfileRecord& wrapped) {
    for (size_t index = 0; index < sizeof(wrapped.bytes); ++index) {
        auto damaged = wrapped;
        damaged.bytes[index] ^= 1U;
        const auto invalid = decodeProfile(damaged);
        assert(newestProfile(old, invalid) == 0);
    }
}

// A valid checksum must not let a record claim more points than its 34 bytes hold.
void countsBeyondTheRecordAreRejected(const ProfileRecord& encoded) {
    const uint8_t oversizedCounts[]{4, 255};
    for (const uint8_t count : oversizedCounts) {
        auto oversized = encoded;
        oversized.bytes[8] = count;
        putProfileWord(oversized.bytes + sizeof(oversized.bytes) - 4, profileChecksum(oversized));
        const auto rejected = decodeProfile(oversized);
        assert(!rejected.valid && rejected.points.size() == 0);
    }
}

// The card format: magic, big-endian generation, count, 7 bytes per point, FNV-1a checksum.
void recordLayout(const session::Profile& profile) {
    const uint8_t golden[]{0x41, 0x52, 0x50, 0x31, 0x01, 0x02, 0x03, 0x04, 0x03, 0x02, 0x00, 0x00,
                           0x00, 0x00, 0x01, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0b, 0x02,
                           0x00, 0x00, 0x00, 0x00, 0x03, 0x0d, 0x67, 0xef, 0x0b, 0xe2};
    const auto stored = encodeProfile(profile, 0x01020304U);
    assert(sizeof(stored.bytes) == sizeof(golden));
    assert(memcmp(stored.bytes, golden, sizeof(golden)) == 0);

    // A record with another magic is not a profile, even with a matching checksum.
    auto renamed = stored;
    renamed.bytes[3] = '2';
    putProfileWord(renamed.bytes + sizeof(renamed.bytes) - 4, profileChecksum(renamed));
    const bool renamedValid = decodeProfile(renamed).valid;
    assert(!renamedValid);
}

// The format cannot hold four points.
void fourPointsDoNotEncode(const session::Profile& profile) {
    session::Profile four = profile;
    const bool grown = four.add({{0x02, 0, 0, 0, 0, 4}, 1});
    assert(grown);
    const auto tooLarge = encodeProfile(four, 1);
    for (const uint8_t byte : tooLarge.bytes) {
        assert(byte == 0);
    }
}

// Half the generation space apart, neither record is newer.
void generationsHalfwayApart(const session::Profile& profile) {
    const StoredProfile low = decodeProfile(encodeProfile(profile, 0));
    StoredProfile high = decodeProfile(encodeProfile(profile, 0x80000000U));
    assert(newestProfile(low, high) == 0 && newestProfile(high, low) == 0);
    high = decodeProfile(encodeProfile(profile, 0x7fffffffU));
    assert(newestProfile(low, high) == 1 && newestProfile(high, low) == 0);
}

void emptyProfileIsARecord(const StoredProfile& newer) {
    const auto empty = encodeProfile({}, 1);
    const auto cleared = decodeProfile(empty);
    assert(cleared.valid && cleared.points.size() == 0);
    assert(newestProfile(newer, cleared) == 1);
    assert(newestProfile({}, {}) == -1);
}
} // namespace

int main() {
    const auto profile = threePoints();
    const auto encoded = encodeProfile(profile, UINT32_MAX);
    const StoredProfile old = decodeProfile(encoded);
    assert(old.valid && old.points.matches(profile) && old.generation == UINT32_MAX);

    const auto updated = editsKeepTheProfileValid(profile);
    const auto wrapped = encodeProfile(updated, 0);
    const StoredProfile newer = decodeProfile(wrapped);
    assert(newer.valid && newestProfile(old, newer) == 1);
    damagedRecordsAreOlder(old, wrapped);
    countsBeyondTheRecordAreRejected(encoded);
    recordLayout(profile);
    fourPointsDoNotEncode(profile);
    generationsHalfwayApart(profile);
    emptyProfileIsARecord(newer);
}
