#pragma once

#include "session_client.h"

namespace airrounds {
namespace flipper {

struct VisitLabels {
    const char* state;
    const char* detail;
};

inline VisitLabels visitLabels(const SessionClient& client) {
    switch (client.pending()) {
    case Request::start:
        return {"Starting...", "Waiting for ESP32"};
    case Request::stop:
        return {"Stopping...", "Waiting for ESP32"};
    default:
        break;
    }

    switch (client.link()) {
    case Link::connecting:
        return {"Checking status", "Waiting for ESP32"};
    case Link::lost:
        return {"UART link lost", "Reconnecting..."};
    case Link::incompatible:
        return {"Wrong protocol", "Use AirRounds firmware"};
    case Link::ready:
        break;
    }

    if (client.state() != uart::State::error && client.rejected() != Request::none) {
        return {client.rejected() == Request::start ? "Start rejected" : "Stop rejected",
                "Press Refresh"};
    }

    switch (client.state()) {
    case uart::State::idle:
        return {"Ready", "No visit yet"};
    case uart::State::running:
        return {"Listening", "Waiting for a beacon"};
    case uart::State::finalizing:
        return {"Reception stopped", "Saving..."};
    case uart::State::complete:
    case uart::State::stopped:
        return {client.state() == uart::State::stopped ? "Stopped" : "Complete",
                client.outcome() == uart::Outcome::saved ? "Saved" : "No beacon"};
    case uart::State::error:
        return {"Error",
                client.outcome() == uart::Outcome::radio_error ? "Radio failed" : "SD save failed"};
    }
    return {"Error", "Unknown visit state"};
}

} // namespace flipper
} // namespace airrounds
