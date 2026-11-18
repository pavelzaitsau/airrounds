#pragma once

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
    stale_request
};

enum class State { idle, running, finalizing, complete, stopped, error };
enum class Outcome { none, not_observed, saved, radio_error, storage_error };

constexpr size_t wireReplyCapacity = 128;

inline const char* wireName(Code code) {
    static constexpr const char* names[]{
        "ok",           "unsupported_version", "wrong_boot", "bad_request",
        "bad_target",   "unknown_session",     "busy",       "conflict",
        "stale_request"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Code::stale_request) + 1,
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

struct Reply {
    Envelope envelope;
    Code code = Code::bad_request;
    uint32_t session = 0;
    State state = State::idle;
    Outcome outcome = Outcome::none;
    uint64_t lastMutation = 0;
    uint8_t features = 0;
};

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
        !readWireName(fields.take(), Code::stale_request, parsed.code) ||
        !fields.take().hex(8, session) ||
        !readWireName(fields.take(), State::error, parsed.state) ||
        !readWireName(fields.take(), Outcome::storage_error, parsed.outcome) ||
        !fields.take().hex(16, parsed.lastMutation) ||
        (parsed.lastMutation & readRequestBit) != 0 || !fields.take().hex(2, featureBits) ||
        !fields.atEnd()) {
        return false;
    }
    parsed.session = static_cast<uint32_t>(session);
    parsed.features = static_cast<uint8_t>(featureBits);
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
