#pragma once

#include "../session/target.h"
#include "scan.h"
#include "uart_frame.h"

namespace airrounds {
namespace uart {

enum class Code {
    ok,
    unsupported_version,
    wrong_boot,
    bad_request,
    bad_target,
    unknown_session,
    busy,
    conflict,
    stale_request,
    radio_error
};

enum class State { idle, running, finalizing, complete, stopped, error };
enum class Outcome { none = 0, not_observed = 1, saved = 2, radio_error = 3, storage_error = 4 };

enum class JournalState { none, pending, saved, failed };
enum class VisitInterruption { unknown, no, yes };
constexpr uint8_t sessionJournalFeature = 0x04;
constexpr uint8_t roundDetailFeature = 0x10;
constexpr size_t wireReplyCapacity = 240;

inline const char* wireName(Code code) {
    static constexpr const char* names[]{"ok",
                                         "unsupported_version",
                                         "wrong_boot",
                                         "bad_request",
                                         "bad_target",
                                         "unknown_session",
                                         "busy",
                                         "conflict",
                                         "stale_request",
                                         "radio_error"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Code::radio_error) + 1,
                  "every code needs a wire name");
    return names[static_cast<size_t>(code)];
}

inline const char* wireName(State state) {
    static constexpr const char* names[]{"idle",     "running", "finalizing",
                                         "complete", "stopped", "error"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(State::error) + 1,
                  "every state needs a wire name");
    return names[static_cast<size_t>(state)];
}

inline const char* wireName(Outcome outcome) {
    static constexpr const char* names[]{"none", "not_observed", "saved", "radio_error",
                                         "storage_error"};
    static_assert(sizeof(names) / sizeof(names[0]) ==
                      static_cast<size_t>(Outcome::storage_error) + 1,
                  "every outcome needs a wire name");
    return names[static_cast<size_t>(outcome)];
}

inline const char* wireName(JournalState state) {
    static constexpr const char* names[]{"none", "pending", "saved", "failed"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(JournalState::failed) + 1,
                  "every journal state needs a wire name");
    return names[static_cast<size_t>(state)];
}

template <typename Word> bool readWireName(Field field, Word last, Word& value) {
    for (size_t index = 0; index <= static_cast<size_t>(last); ++index) {
        const auto candidate = static_cast<Word>(index);
        if (field.is(wireName(candidate))) {
            value = candidate;
            return true;
        }
    }
    return false;
}

struct VisitId {
    uint64_t boot;
    uint32_t session;

    bool matches(const VisitId& other) const {
        return boot == other.boot && session == other.session;
    }
};

struct RoundStatus {
    session::Profile profile;
    Outcome results[session::Profile::capacity]{};
    VisitInterruption interruption = VisitInterruption::unknown;

    // The first unfinished slot is the active one while the board is busy.
    size_t next() const {
        size_t index = 0;
        while (index < profile.size() && results[index] != Outcome::none) {
            ++index;
        }
        return index;
    }
};

struct Reply {
    Envelope envelope;
    Code code = Code::bad_request;
    uint32_t session = 0;
    State state = State::idle;
    Outcome outcome = Outcome::none;
    uint64_t lastMutation = 0;
    uint8_t features = 0;
    bool hasRound = false;
    RoundStatus round;
    bool hasJournal = false;
    JournalState journal = JournalState::none;
    bool hasScan = false;
    ScanReply scan{};
    bool hasFirmware = false;
    char firmware[16]{}; // Up to 15 characters of digits and dots, then a terminator.
};

inline bool readFirmware(Fields& fields, Reply& reply) {
    const auto value = fields.take();
    if (value.size == 0 || value.size >= sizeof(reply.firmware) || !fields.atEnd()) {
        return false;
    }
    for (size_t index = 0; index < value.size; ++index) {
        const char character = value.data[index];
        if (character != '.' && (character < '0' || character > '9')) {
            return false;
        }
    }
    memcpy(reply.firmware, value.data, value.size);
    reply.firmware[value.size] = '\0';
    reply.hasFirmware = true;
    return true;
}

inline bool readRound(Fields& fields, RoundStatus& round) {
    for (size_t index = round.profile.size(); index < session::Profile::capacity && !fields.atEnd();
         ++index) {
        const auto entry = fields.take();
        if (entry.size != 15) {
            return false;
        }
        uint64_t address = 0;
        uint64_t channel = 0;
        uint64_t result = 0;
        const Field bssid{entry.data, 12};
        const Field channelField{entry.data + 12, 2};
        const Field resultField{entry.data + 14, 1};
        if (!bssid.hex(12, address) || !channelField.hex(2, channel) ||
            !resultField.hex(1, result) || result > static_cast<uint64_t>(Outcome::storage_error)) {
            return false;
        }
        session::Target target{{}, static_cast<uint8_t>(channel)};
        session::setAddress(target, address);
        if (!round.profile.add(target)) {
            return false;
        }
        round.results[index] = static_cast<Outcome>(result);
    }
    return fields.atEnd();
}

inline bool readScanReply(Fields& fields, ScanReply& scan) {
    uint64_t count = 0;
    uint64_t index = 0;
    if (!readWireName(fields.take(), ScanState::error, scan.state) ||
        !fields.take().hex(2, count) || !fields.take().hex(2, index) ||
        count > ScanList::capacity) {
        return false;
    }
    scan.count = static_cast<size_t>(count);
    scan.index = static_cast<size_t>(index);
    if (scan.state != ScanState::complete || scan.count == 0) {
        return scan.count == 0 && scan.index == 0 && fields.atEnd();
    }
    return scan.index < scan.count && readScanEntry(fields, scan.entry);
}

inline bool readReplyDetails(Fields& fields, Reply& reply) {
    if (fields.atEnd()) {
        return true;
    }
    auto marker = fields.take();
    if (marker.is("firmware")) {
        return readFirmware(fields, reply);
    }
    if (marker.is("scan")) {
        reply.hasScan = true;
        return (reply.features & scanFeature) != 0 && readScanReply(fields, reply.scan);
    }
    if (marker.is("journal")) {
        if ((reply.features & sessionJournalFeature) == 0 ||
            !readWireName(fields.take(), JournalState::failed, reply.journal)) {
            return false;
        }
        reply.hasJournal = true;
        marker = fields.take();
    }
    if (marker.is("interrupted")) {
        uint64_t interrupted = 0;
        if ((reply.features & roundDetailFeature) == 0 || !fields.take().hex(1, interrupted) ||
            interrupted > 1) {
            return false;
        }
        reply.round.interruption =
            interrupted == 1 ? VisitInterruption::yes : VisitInterruption::no;
        marker = fields.take();
    }
    reply.hasRound = marker.is("aps");
    return reply.hasRound && (reply.features & passiveRoundFeature) != 0 &&
           readRound(fields, reply.round) &&
           ((reply.features & scanFeature) != 0 ||
            reply.round.profile.size() <= session::Profile::legacyCapacity);
}

// Pass one line including its line feed.
inline bool readReply(const char* line, Reply& reply) {
    const char* marker = strrchr(line, '!');
    if (marker == nullptr) {
        return false;
    }
    const size_t length = strlen(marker);
    if (length < 2 || length >= wireReplyCapacity || marker[length - 1] != '\n') {
        return false;
    }

    // Boot logs can precede the reply on the same line.
    char body[wireReplyCapacity];
    memcpy(body, marker + 1, length - 2);
    body[length - 2] = '\0';
    Fields fields(body);
    uint64_t session = 0;
    uint64_t featureBits = 0;
    Reply parsed;
    if (!parsed.envelope.read(fields) ||
        !readWireName(fields.take(), Code::radio_error, parsed.code) ||
        !fields.take().hex(8, session) ||
        !readWireName(fields.take(), State::error, parsed.state) ||
        !readWireName(fields.take(), Outcome::storage_error, parsed.outcome) ||
        !fields.take().hex(16, parsed.lastMutation) ||
        (parsed.lastMutation & readRequestBit) != 0 || !fields.take().hex(2, featureBits)) {
        return false;
    }
    parsed.session = static_cast<uint32_t>(session);
    parsed.features = static_cast<uint8_t>(featureBits);
    if (!readReplyDetails(fields, parsed)) {
        return false;
    }
    reply = parsed;
    return true;
}

// A zero expected boot is for hello only.
inline bool matchesReplyKey(const ReplyKey& reply, const ReplyKey& expected) {
    const bool bootMatches = expected.boot == 0
                                 ? expected.scope == 0 && (expected.request & readRequestBit) != 0
                                 : reply.boot == expected.boot;
    return reply.boot != 0 && bootMatches && reply.request == expected.request &&
           reply.scope == expected.scope;
}

inline bool matchesReply(const char* line, const ReplyKey& expected) {
    Reply reply;
    return readReply(line, reply) && reply.envelope.wireVersion == version &&
           matchesReplyKey(reply.envelope.key, expected);
}

} // namespace uart
} // namespace airrounds
