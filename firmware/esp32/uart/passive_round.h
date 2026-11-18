#pragma once

#include "uart_reply.h"

namespace airrounds {
namespace uart {

struct CaptureId {
    VisitId visit;
    unsigned slot; // Zero keeps the filename used for a single visit.
};

class VisitBackend {
public:
    virtual ~VisitBackend() = default;
    virtual bool start(const session::Target& target) = 0;
    virtual bool observationDone() const = 0;
    virtual bool stopReception() = 0;
    virtual void beginSave(const CaptureId& id) = 0;
    virtual Outcome saveResult() const = 0;
};

class PassiveRound {
public:
    explicit PassiveRound(VisitBackend& visit) : visit_(visit) {}

    bool start(const VisitId& id, const session::Profile& profile) {
        if (profile.size() == 0 || state_ == State::running || state_ == State::finalizing) {
            return false;
        }
        id_ = id;
        status_ = {};
        status_.profile = profile;
        current_ = 0;
        stopRequested_ = false;
        anySaved_ = false;
        startCurrent();
        return true;
    }

    void stop() {
        if (state_ == State::running ||
            (state_ == State::finalizing && current_ + 1 < status_.profile.size())) {
            // A stop during a save keeps the current file and skips later visits.
            stopRequested_ = true;
        }
        if (state_ == State::running) {
            endObservation();
        }
    }

    void poll() {
        if (state_ == State::running && visit_.observationDone()) {
            endObservation();
        } else if (state_ == State::finalizing) {
            updateSaveResult();
        }
    }

    State state() const { return state_; }
    Outcome outcome() const { return outcome_; }
    const RoundStatus& status() const { return status_; }

private:
    void startCurrent() {
        if (visit_.start(status_.profile[current_])) {
            state_ = State::running;
            outcome_ = Outcome::none;
        } else {
            fail(Outcome::radio_error);
        }
    }

    void fail(Outcome result) {
        status_.results[current_] = result;
        state_ = State::error;
        outcome_ = result;
    }

    void endObservation() {
        if (!visit_.stopReception()) {
            fail(Outcome::radio_error);
            return;
        }
        state_ = State::finalizing;
        const auto slot = status_.profile.size() == 1 ? 0U : static_cast<unsigned>(current_ + 1);
        visit_.beginSave({id_, slot});
        updateSaveResult();
    }

    void updateSaveResult() {
        const Outcome result = visit_.saveResult();
        if (result == Outcome::none) {
            return;
        }
        if (result != Outcome::saved && result != Outcome::not_observed) {
            fail(result);
            return;
        }
        status_.results[current_++] = result;
        anySaved_ = anySaved_ || result == Outcome::saved;
        if (!stopRequested_ && current_ < status_.profile.size()) {
            startCurrent();
            return;
        }
        state_ = stopRequested_ ? State::stopped : State::complete;
        outcome_ = anySaved_ ? Outcome::saved : Outcome::not_observed;
    }

    VisitBackend& visit_;
    VisitId id_{};
    RoundStatus status_;
    size_t current_ = 0;
    bool stopRequested_ = false;
    bool anySaved_ = false;
    State state_ = State::idle;
    Outcome outcome_ = Outcome::none;
};

} // namespace uart
} // namespace airrounds
