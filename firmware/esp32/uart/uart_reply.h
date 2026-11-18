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

template <typename Word> bool validWireName(Field field, Word last) {
    for (size_t index = 0; index <= static_cast<size_t>(last); ++index) {
        if (field.is(wireName(static_cast<Word>(index)))) {
            return true;
        }
    }
    return false;
}

inline bool validReplyFields(Fields& fields) {
    uint64_t session = 0;
    uint64_t lastMutation = 0;
    uint64_t featureBits = 0;
    return validWireName(fields.take(), Code::stale_request) && fields.take().hex(8, session) &&
           validWireName(fields.take(), State::error) &&
           validWireName(fields.take(), Outcome::storage_error) &&
           fields.take().hex(16, lastMutation) && (lastMutation & readRequestBit) == 0 &&
           fields.take().hex(2, featureBits) && fields.atEnd();
}

// A zero expected boot is for hello only. Pass one line including its line feed.
inline bool matchesReply(const char* line, const ReplyKey& expected) {
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
    Envelope reply;
    if (!reply.read(fields) || reply.wireVersion != version || reply.key.boot == 0) {
        return false;
    }
    const bool bootMatches = expected.boot == 0
                                 ? expected.scope == 0 && (expected.request & readRequestBit) != 0
                                 : reply.key.boot == expected.boot;
    return bootMatches && reply.key.request == expected.request &&
           reply.key.scope == expected.scope && validReplyFields(fields);
}

} // namespace uart
} // namespace airrounds
