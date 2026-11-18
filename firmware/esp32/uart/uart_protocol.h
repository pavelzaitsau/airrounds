#pragma once

#include <inttypes.h>
#include <stdio.h>

#include "target.h"
#include "uart_reply.h"

namespace airrounds {
namespace uart {

struct VisitId {
    uint64_t boot;
    uint32_t session;
};

class VisitBackend {
public:
    virtual ~VisitBackend() = default;
    virtual bool start(const session::Target& target) = 0;
    virtual bool observationDone() const = 0;
    virtual bool stopReception() = 0;
    virtual void beginSave(VisitId id) = 0;
    virtual Outcome saveResult() const = 0;
};

class Protocol {
public:
    enum : size_t { replyCapacity = wireReplyCapacity };

    Protocol(VisitBackend& visit, uint64_t bootId) : visit_(visit), boot_(bootId) {}

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
        const int count = snprintf(reply, sizeof(reply),
                                   "!%02x %016" PRIx64 " %016" PRIx64 " %08" PRIx64 " %s %08" PRIx32
                                   " %s %s %016" PRIx64 " %02x\n",
                                   replyVersion, replyBoot, request.key.request, request.key.scope,
                                   wireName(code), session_, wireName(state_), wireName(outcome_),
                                   lastMutation_, features);
        return count > 0 && static_cast<size_t>(count) < sizeof(reply);
    }

    void poll() {
        if (state_ == State::running && visit_.observationDone()) {
            endObservation(State::complete);
        }
        if (state_ == State::finalizing) {
            outcome_ = visit_.saveResult();
            if (outcome_ != Outcome::none) {
                state_ = outcome_ == Outcome::saved || outcome_ == Outcome::not_observed
                             ? afterSave_
                             : State::error;
            }
        }
    }

private:
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
        if (readRequest && verb.is("status") && fields.atEnd()) {
            return scope == 0 || scope == session_ ? Code::ok : Code::unknown_session;
        }
        const bool starts = verb.is("start");
        if (readRequest || (!starts && !verb.is("stop"))) {
            return Code::bad_request;
        }
        return mutate(request.key, starts, fields);
    }

    Code mutate(const ReplyKey& key, bool starts, Fields& fields) {
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
            lastCode_ = start(scope, fields);
        } else {
            lastCode_ = fields.atEnd() ? stop(scope) : Code::bad_request;
        }
        return lastCode_;
    }

    Code start(uint32_t scope, Fields& fields) {
        uint64_t bssid = 0;
        uint64_t channel = 0;
        if (!fields.take().hex(12, bssid) || !fields.take().hex(2, channel) || !fields.atEnd()) {
            return Code::bad_request;
        }
        session::Target target{};
        target.channel = static_cast<uint8_t>(channel);
        for (size_t index = sizeof(target.bssid); index > 0; --index) {
            target.bssid[index - 1] = static_cast<uint8_t>(bssid);
            bssid >>= 8U;
        }
        if (!session::validTarget(target)) {
            return Code::bad_target;
        }
        return startSession(scope, target);
    }

    Code startSession(uint32_t scope, const session::Target& target) {
        if (scope == 0) {
            return Code::unknown_session;
        }
        if (scope == session_) {
            const bool same = target.channel == target_.channel &&
                              memcmp(target.bssid, target_.bssid, sizeof(target.bssid)) == 0;
            return same ? Code::ok : Code::conflict;
        }
        if (scope - 1 != session_) {
            return Code::unknown_session;
        }
        if (state_ == State::running || state_ == State::finalizing) {
            return Code::busy;
        }

        session_ = scope;
        target_ = target;
        if (visit_.start(target)) {
            state_ = State::running;
            outcome_ = Outcome::none;
        } else {
            state_ = State::error;
            outcome_ = Outcome::radio_error;
        }
        return Code::ok;
    }

    Code stop(uint32_t scope) {
        if (scope == 0 || scope != session_) {
            return Code::unknown_session;
        }
        if (state_ == State::running) {
            endObservation(State::stopped);
        }
        return Code::ok;
    }

    void endObservation(State afterSave) {
        if (!visit_.stopReception()) {
            state_ = State::error;
            outcome_ = Outcome::radio_error;
            return;
        }
        afterSave_ = afterSave;
        state_ = State::finalizing;
        visit_.beginSave({boot_, session_});
    }

    VisitBackend& visit_;
    const uint64_t boot_ = 0;
    FrameReader reader_;
    uint64_t lastMutation_ = 0;
    char lastCommand_[frameCapacity]{};
    Code lastCode_ = Code::bad_request;
    uint32_t session_ = 0;
    session::Target target_{};
    State state_ = State::idle;
    State afterSave_ = State::idle;
    Outcome outcome_ = Outcome::none;
};

} // namespace uart
} // namespace airrounds
