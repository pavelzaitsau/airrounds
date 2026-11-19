#pragma once

#include "session_client.h"

namespace airrounds {
namespace flipper {

enum class HomeEntry { status, setup, start, board };

inline bool sessionBusy(uart::State state) {
    return state == uart::State::running || state == uart::State::finalizing;
}

inline HomeEntry homeEntry(Request pending, uart::State state, size_t count, bool canStart) {
    if (pending == Request::start || pending == Request::stop || sessionBusy(state)) {
        return HomeEntry::status;
    }
    if (count == 0) {
        return HomeEntry::setup;
    }
    return canStart ? HomeEntry::start : HomeEntry::board;
}

struct RunReport {
    uart::JournalState journal = uart::JournalState::none;
    uart::VisitId id{};
    uart::State state = uart::State::idle;
    uart::Outcome outcome = uart::Outcome::none;
    uart::RoundStatus round{};
};

inline size_t reportSize(const RunReport& report) {
    return report.round.profile.size() == 0 ? 1 : report.round.profile.size();
}

inline uart::Outcome pointOutcome(const RunReport& report, size_t point) {
    return report.round.profile.size() == 0 ? report.outcome : report.round.results[point];
}

inline unsigned savedPoints(const RunReport& report) {
    unsigned saved = 0;
    for (size_t index = 0; index < reportSize(report); ++index) {
        if (pointOutcome(report, index) == uart::Outcome::saved) {
            ++saved;
        }
    }
    return saved;
}

inline const char* outcomeTitle(const RunReport& report) {
    if (report.state == uart::State::error) {
        return report.outcome == uart::Outcome::storage_error ? "SD write failed" : "Radio failed";
    }
    return report.state == uart::State::stopped ? "Session stopped" : "Session ended";
}

inline const char* pointResult(uart::Outcome outcome, uart::State state) {
    switch (outcome) {
    case uart::Outcome::saved:
        return "Captured";
    case uart::Outcome::not_observed:
        return "Not seen";
    case uart::Outcome::radio_error:
        return "Radio error";
    case uart::Outcome::storage_error:
        return "SD error";
    case uart::Outcome::none:
        return state == uart::State::stopped ? "Stopped" : "Skipped";
    }
    return "Unknown";
}

inline const char* pointDetail(uart::Outcome outcome, uart::State state) {
    switch (outcome) {
    case uart::Outcome::saved:
        return "Captured to board SD";
    case uart::Outcome::not_observed:
        return "Point not seen";
    case uart::Outcome::storage_error:
        return "SD write failed";
    case uart::Outcome::none:
        return state == uart::State::stopped ? "Stopped before result" : "Skipped after error";
    default:
        return pointResult(outcome, state);
    }
}

} // namespace flipper
} // namespace airrounds
