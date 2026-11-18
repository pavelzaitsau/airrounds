#pragma once

#include <inttypes.h>
#include <stdio.h>

#include "passive_round.h"

namespace airrounds {
namespace uart {

class Protocol {
public:
    enum : size_t { replyCapacity = wireReplyCapacity };

    Protocol(VisitBackend& visit, uint64_t bootId) : round_(visit), boot_(bootId) {}

    bool receive(TimedByte byte, char (&reply)[replyCapacity]) {
        if (!reader_.push(byte)) {
            return false;
        }
        Fields fields(reader_.body());
        Envelope request;
        if (!request.read(fields)) {
            return false;
        }
        const Field verb = fields.take();
        const bool hello = verb.is("hello");
        const Code code = dispatch(request, verb, fields);
        // A rejection must keep the IDs from the request it rejected.
        const auto replyVersion = static_cast<unsigned>(hello ? version : request.wireVersion);
        const uint64_t replyBoot = hello ? boot_ : request.key.boot;
        const bool extended = verb.is("round") || verb.is("round_status") || verb.is("round_stop");
        const int count = snprintf(reply, sizeof(reply),
                                   "!%02x %016" PRIx64 " %016" PRIx64 " %08" PRIx64 " %s %08" PRIx32
                                   " %s %s %016" PRIx64 " %02x",
                                   replyVersion, replyBoot, request.key.request, request.key.scope,
                                   wireName(code), session_, wireName(round_.state()),
                                   wireName(round_.outcome()), lastMutation_, features);
        if (count <= 0 || static_cast<size_t>(count) >= sizeof(reply)) {
            return false;
        }
        return finishReply(reply, static_cast<size_t>(count), extended);
    }

    void poll() { round_.poll(); }

private:
    bool finishReply(char (&reply)[replyCapacity], size_t used, bool extended) const {
        if (extended) {
            constexpr char marker[] = " aps";
            if (sizeof(reply) - used < sizeof(marker)) {
                return false;
            }
            memcpy(reply + used, marker, sizeof(marker));
            used += sizeof(marker) - 1;
            const auto& status = round_.status();
            for (size_t index = 0; index < status.profile.size(); ++index) {
                const auto& target = status.profile[index];
                const int count =
                    snprintf(reply + used, sizeof(reply) - used, " %012" PRIx64 "%02x%x",
                             session::addressValue(target), static_cast<unsigned>(target.channel),
                             static_cast<unsigned>(status.results[index]));
                if (count != 16 || static_cast<size_t>(count) >= sizeof(reply) - used) {
                    return false;
                }
                used += static_cast<size_t>(count);
            }
        }
        if (sizeof(reply) - used < 2) {
            return false;
        }
        reply[used] = '\n';
        reply[used + 1] = '\0';
        return true;
    }

    Code dispatch(const Envelope& request, Field verb, Fields& fields) {
        if (request.wireVersion != version) {
            return Code::unsupported_version;
        }
        const auto scope = static_cast<uint32_t>(request.key.scope);
        const bool readRequest = (request.key.request & readRequestBit) != 0;
        if (verb.is("hello")) {
            return readRequest && request.key.boot == 0 && scope == 0 && fields.atEnd()
                       ? Code::ok
                       : Code::bad_request;
        }
        if (request.key.boot != boot_) {
            return Code::wrong_boot;
        }
        if (readRequest && (verb.is("status") || verb.is("round_status")) && fields.atEnd()) {
            return scope == 0 || scope == session_ ? Code::ok : Code::unknown_session;
        }
        const bool starts = verb.is("start") || verb.is("round");
        if (readRequest || (!starts && !verb.is("stop") && !verb.is("round_stop"))) {
            return Code::bad_request;
        }
        return mutate(request.key, verb, starts, fields);
    }

    Code mutate(const ReplyKey& key, Field verb, bool starts, Fields& fields) {
        const uint64_t request = key.request;
        const auto scope = static_cast<uint32_t>(key.scope);
        if (request < lastMutation_) {
            return Code::stale_request;
        }
        if (request == lastMutation_) {
            return strcmp(lastCommand_, reader_.body()) == 0 ? lastCode_ : Code::conflict;
        }
        lastMutation_ = request;
        memcpy(lastCommand_, reader_.body(), sizeof(lastCommand_));
        if (starts) {
            lastCode_ = start(scope, fields, verb.is("round"));
        } else {
            lastCode_ = fields.atEnd() ? stop(scope) : Code::bad_request;
        }
        return lastCode_;
    }

    Code start(uint32_t scope, Fields& fields, bool multiple) {
        session::Profile profile;
        do {
            if (profile.size() == session::Profile::capacity) {
                return Code::bad_request;
            }
            uint64_t bssid = 0;
            uint64_t channel = 0;
            if (!fields.take().hex(12, bssid) || !fields.take().hex(2, channel) ||
                (!multiple && !fields.atEnd())) {
                return Code::bad_request;
            }
            session::Target target{{}, static_cast<uint8_t>(channel)};
            session::setAddress(target, bssid);
            if (!profile.add(target)) {
                return Code::bad_target;
            }
        } while (!fields.atEnd());
        return startSession(scope, profile);
    }

    Code startSession(uint32_t scope, const session::Profile& profile) {
        if (scope == 0) {
            return Code::unknown_session;
        }
        if (scope == session_) {
            return round_.status().profile.matches(profile) ? Code::ok : Code::conflict;
        }
        if (scope - 1 != session_) {
            return Code::unknown_session;
        }
        if (!round_.start({boot_, scope}, profile)) {
            return Code::busy;
        }

        session_ = scope;
        return Code::ok;
    }

    Code stop(uint32_t scope) {
        if (scope == 0 || scope != session_) {
            return Code::unknown_session;
        }
        round_.stop();
        return Code::ok;
    }

    PassiveRound round_;
    const uint64_t boot_ = 0;
    FrameReader reader_;
    uint64_t lastMutation_ = 0;
    char lastCommand_[frameCapacity]{};
    Code lastCode_ = Code::bad_request;
    uint32_t session_ = 0;
};

} // namespace uart
} // namespace airrounds
