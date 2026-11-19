#pragma once

#include "../firmware/esp32/session/target.h"
#include "../firmware/esp32/uart/uart_reply.h"

namespace airrounds {
namespace flipper {

enum class Link { connecting, ready, lost, incompatible };
enum class Request { none, hello, status, start, stop, scan, scan_read, scan_stop };

class SessionClient {
public:
    using Send = void (*)(void*, const char*, size_t);

    SessionClient(Send send, void* context, uint64_t nonce)
        : send_(send), context_(context), nonce_(nonce & ~uart::readRequestBit) {}

    Link link() const { return link_; }
    Request pending() const { return pending_; }
    Request rejected() const { return rejected_; }
    uart::VisitId visitId() const { return {boot_, snapshot_.session}; }
    uart::State state() const { return snapshot_.state; }
    uart::Outcome outcome() const { return snapshot_.outcome; }
    uart::JournalState journalState() const { return snapshot_.journal; }

    const uart::RoundStatus& round() const { return snapshot_.round; }
    bool supportsRounds() const { return link_ == Link::ready && supportsRounds_; }

    bool supportsScan() const { return supportsScan_; }
    bool supportsRoundDetail() const { return supportsRoundDetail_; }
    uart::ScanState scanState() const { return scanState_; }
    const uart::ScanList& scanResults() const { return scanResults_; }

    bool canStart(size_t count = 1) const {
        const size_t limit = supportsScan_      ? session::Profile::capacity
                             : supportsRounds() ? session::Profile::legacyCapacity
                                                : 1U;
        return count > 0 && count <= limit && canMutate() && snapshot_.session != UINT32_MAX &&
               state() != uart::State::running && state() != uart::State::finalizing &&
               scanState_ != uart::ScanState::running;
    }

    bool scan(uint32_t now) {
        if (!supportsScan_ || !canMutate() || state() == uart::State::running ||
            state() == uart::State::finalizing || scanState_ == uart::ScanState::running) {
            return false;
        }
        scanResults_.clear();
        scanExpectedCount_ = 0;
        scanState_ = uart::ScanState::running;
        request(Request::scan, now);
        return true;
    }

    bool cancelScan(uint32_t now) {
        if (scanState_ != uart::ScanState::running || link_ != Link::ready) {
            return false;
        }
        if (pending_ == Request::scan) {
            cancelScanPending_ = true;
        } else if (pending_ != Request::scan_stop && canMutate()) {
            request(Request::scan_stop, now);
        }
        return true;
    }

    bool canStop() const {
        return canMutate() &&
               (state() == uart::State::running || (state() == uart::State::finalizing &&
                                                    round().next() + 1 < round().profile.size()));
    }

    bool start(const session::Target& target, uint32_t now) {
        session::Profile profile;
        return profile.add(target) && start(profile, now);
    }

    bool start(const session::Profile& profile, uint32_t now) {
        if (!canStart(profile.size())) {
            return false;
        }
        requestedProfile_ = profile;
        request(Request::start, now, &requestedProfile_);
        return true;
    }

    bool stop(const uart::VisitId& expected, uint32_t now) {
        if (!canStop() || !expected.matches(visitId())) {
            return false;
        }
        request(Request::stop, now);
        return true;
    }

    void refresh(uint32_t now) {
        // A new read can replace an old read. Keep any command already on the wire.
        if (!mutationPending()) {
            pending_ = Request::none;
        }
        rejected_ = Request::none;
        link_ = Link::connecting;
        wait_ = {now, 0};
    }

    void transportLost(uint32_t now) { reconnect(Link::lost, now); }

    void poll(uint32_t now) {
        if (pending_ != Request::none) {
            if (now - sentAt_ >= requestTimeoutMs) {
                transportLost(now);
            }
            return;
        }
        if (now - wait_.since >= wait_.duration) {
            if (boot_ == 0) {
                request(Request::hello, now);
            } else if (cancelScanPending_) {
                cancelScanPending_ = false;
                request(Request::scan_stop, now);
            } else if (scanState_ == uart::ScanState::running) {
                request(Request::scan_read, now);
            } else {
                request(Request::status, now);
            }
        }
    }

    void receive(uart::TimedByte input) {
        const uint8_t byte = input.value;
        if (byte == '!') {
            line_[0] = '!';
            length_ = 1;
            return;
        }
        if (length_ == 0) {
            return;
        }
        if (length_ >= sizeof(line_) - 1 || (byte != '\n' && (byte < ' ' || byte > '~'))) {
            length_ = 0;
            return;
        }
        line_[length_++] = static_cast<char>(byte);
        if (byte == '\n') {
            line_[length_] = '\0';
            length_ = 0;
            handleReply(input.timeMs);
        }
    }

private:
    static constexpr uint32_t requestTimeoutMs = 900;
    static constexpr uint32_t reconnectMs = 300;
    static constexpr uint32_t pollMs = 350;

    bool mutationPending() const {
        return pending_ == Request::start || pending_ == Request::stop ||
               pending_ == Request::scan || pending_ == Request::scan_stop;
    }

    bool canMutate() const {
        return link_ == Link::ready && !mutationPending() &&
               mutationHighWater_ < uart::readRequestBit - 1;
    }

    void reconnect(Link reason, uint32_t now) {
        boot_ = 0;
        if (scanState_ == uart::ScanState::running) {
            scanState_ = uart::ScanState::error;
        }
        cancelScanPending_ = false;
        link_ = reason;
        pending_ = Request::none;
        length_ = 0;
        wait_ = {now, reconnectMs};
    }

    template <size_t width> static char* hex(char* output, uint64_t value) {
        constexpr char digits[] = "0123456789abcdef";
        for (size_t i = width; i > 0; --i) {
            output[i - 1] = digits[value & 0x0fU];
            value >>= 4U;
        }
        return output + width;
    }

    void request(Request kind, uint32_t now, const session::Profile* profile = nullptr) {
        pending_ = kind;
        sentAt_ = now;
        const bool mutation = mutationPending();
        const auto id = mutation ? ++mutationHighWater_ : nextNonce();
        if (mutation) {
            rejected_ = Request::none;
        }
        const char* verb = "hello";
        uint32_t scope = 0;
        expectsRound_ = false;
        expectsJournal_ = false;
        if (kind == Request::start) {
            scope = snapshot_.session + 1U;
            expectsRound_ = supportsRounds_;
            verb = expectsRound_ ? "round" : "start";
        } else if (kind == Request::stop) {
            scope = snapshot_.session;
            expectsRound_ = supportsRounds_;
            verb = expectsRound_ ? "round_stop" : "stop";
        } else if (kind == Request::scan) {
            scanId_ = id;
            verb = "scan";
        } else if (kind == Request::scan_read) {
            verb = "scan_read";
        } else if (kind == Request::scan_stop) {
            verb = "scan_stop";
        } else if (kind == Request::status) {
            expectsRound_ = supportsRounds_;
            expectsJournal_ = supportsJournal_;
            verb = expectsRound_ ? "round_status" : "status";
            if (supportsRoundDetail_) {
                verb = "round_status_detail";
            } else if (expectsJournal_) {
                verb = "round_status_log";
            }
        }
        key_ = {boot_, id, scope};

        char frame[uart::frameCapacity + 1];
        char* out = frame;
        *out++ = '@';
        out = hex<2>(out, uart::version);
        *out++ = ' ';
        out = hex<16>(out, key_.boot);
        *out++ = ' ';
        out = hex<16>(out, key_.request);
        *out++ = ' ';
        out = hex<8>(out, key_.scope);
        *out++ = ' ';
        const size_t verbLength = strlen(verb);
        memcpy(out, verb, verbLength);
        out += verbLength;
        if (kind == Request::scan_read || kind == Request::scan_stop) {
            *out++ = ' ';
            out = hex<16>(out, scanId_);
        }
        if (kind == Request::scan_read) {
            *out++ = ' ';
            out = hex<2>(out, scanResults_.size());
        }
        for (size_t index = 0; profile != nullptr && index < profile->size(); ++index) {
            const auto& target = (*profile)[index];
            *out++ = ' ';
            out = hex<12>(out, session::addressValue(target));
            *out++ = ' ';
            out = hex<2>(out, target.channel);
        }
        *out++ = '\n';
        send_(context_, frame, static_cast<size_t>(out - frame));
    }

    uint64_t nextNonce() {
        nonce_ = (nonce_ + 1) & ~uart::readRequestBit;
        return nonce_ | uart::readRequestBit;
    }

    void handleReply(uint32_t now) {
        uart::Reply reply;
        if (pending_ == Request::none || !uart::readReply(line_, reply) ||
            !uart::matchesReplyKey(reply.envelope.key, key_)) {
            return;
        }
        if (now - sentAt_ >= requestTimeoutMs) {
            transportLost(now);
            return;
        }
        if (reply.envelope.wireVersion != uart::version ||
            reply.code == uart::Code::unsupported_version) {
            reconnect(Link::incompatible, now);
            return;
        }
        if (reply.code == uart::Code::wrong_boot) {
            transportLost(now);
            return;
        }
        if (pending_ == Request::hello) {
            if (reply.code != uart::Code::ok || (reply.features & uart::passiveVisitFeature) == 0) {
                reconnect(Link::incompatible, now);
                return;
            }
            supportsRounds_ = (reply.features & uart::passiveRoundFeature) != 0;
            supportsScan_ = supportsRounds_ && (reply.features & uart::scanFeature) != 0;
            supportsRoundDetail_ =
                supportsRounds_ && (reply.features & uart::roundDetailFeature) != 0;
            supportsJournal_ =
                supportsRounds_ && (reply.features & uart::sessionJournalFeature) != 0;
            boot_ = reply.envelope.key.boot;
            if (boot_ != mutationBoot_) {
                mutationBoot_ = boot_;
                mutationHighWater_ = 0;
                rejected_ = Request::none;
            }
            rememberMutation(reply.lastMutation);
            pending_ = Request::none;
            link_ = Link::connecting;
            wait_ = {now, 0};
            return;
        }

        if (pending_ == Request::scan || pending_ == Request::scan_read ||
            pending_ == Request::scan_stop) {
            acceptScan(reply, now);
        } else {
            acceptStatus(reply, now);
        }
    }

    void acceptScan(const uart::Reply& reply, uint32_t now) {
        using uart::ScanState;
        if (reply.hasRound || reply.hasJournal ||
            (reply.code == uart::Code::ok &&
             (reply.hasScan != (pending_ == Request::scan_read) ||
              (mutationPending() && reply.lastMutation != key_.request)))) {
            return;
        }
        rememberMutation(reply.lastMutation);
        if (reply.code != uart::Code::ok) {
            scanState_ = ScanState::error;
        } else if (pending_ == Request::scan_stop) {
            scanState_ = ScanState::idle;
        } else if (pending_ == Request::scan_read && !cancelScanPending_) {
            collectScan(reply.scan);
        }
        pending_ = Request::none;
        if (scanState_ != ScanState::running) {
            cancelScanPending_ = false;
        }
        wait_ = {now, reply.hasScan && reply.scan.state == ScanState::running && !cancelScanPending_
                          ? pollMs
                          : 0};
    }

    void collectScan(const uart::ScanReply& result) {
        using uart::ScanState;
        if (result.state == ScanState::running && scanExpectedCount_ == 0) {
            return;
        }
        if (result.state != ScanState::complete || result.index != scanResults_.size() ||
            (scanExpectedCount_ != 0 && result.count != scanExpectedCount_)) {
            scanState_ = ScanState::error;
            return;
        }
        scanExpectedCount_ = result.count;
        if (result.count != 0 &&
            !scanResults_.add(result.entry.target, result.entry.name, result.entry.nameSize)) {
            scanState_ = ScanState::error;
        } else if (scanResults_.size() == result.count) {
            scanState_ = ScanState::complete;
        }
    }

    static bool validSnapshot(const uart::Reply& reply) {
        using uart::Outcome;
        using uart::State;
        if ((reply.session == 0) != (reply.state == State::idle)) {
            return false;
        }
        switch (reply.state) {
        case State::idle:
        case State::running:
        case State::finalizing:
            return reply.outcome == Outcome::none;
        case State::complete:
        case State::stopped:
            return reply.outcome == Outcome::saved || reply.outcome == Outcome::not_observed;
        case State::error:
            return reply.outcome == Outcome::radio_error || reply.outcome == Outcome::storage_error;
        }
        return false;
    }

    static bool validRound(const uart::Reply& reply) {
        using uart::Outcome;
        using uart::State;
        const auto& round = reply.round;
        const size_t count = round.profile.size();
        const size_t next = round.next();
        if ((count == 0) != (reply.state == State::idle) ||
            (round.interruption == uart::VisitInterruption::yes &&
             (reply.state != State::stopped || next == 0 ||
              round.results[next - 1] != Outcome::not_observed))) {
            return false;
        }
        bool saved = false;
        for (size_t index = 0; index < count; ++index) {
            const auto result = round.results[index];
            if (index >= next) {
                if (result != Outcome::none) {
                    return false;
                }
            } else if (reply.state == State::error && index + 1 == next) {
                if (result != reply.outcome) {
                    return false;
                }
            } else if (result == Outcome::saved) {
                saved = true;
            } else if (result != Outcome::not_observed) {
                return false;
            }
        }
        switch (reply.state) {
        case State::idle:
            return true;
        case State::running:
        case State::finalizing:
            return next < count;
        case State::error:
            return next > 0;
        case State::complete:
        case State::stopped:
            return next > 0 && (reply.state == State::stopped || next == count) &&
                   reply.outcome == (saved ? Outcome::saved : Outcome::not_observed);
        }
        return false;
    }

    bool consistentRound(const uart::Reply& reply) const {
        if (pending_ == Request::start && !requestedProfile_.matches(reply.round.profile)) {
            return false;
        }
        if (!snapshot_.hasRound || snapshot_.envelope.key.boot != reply.envelope.key.boot ||
            snapshot_.session != reply.session) {
            return true;
        }

        using uart::State;
        const bool finished =
            state() == State::complete || state() == State::stopped || state() == State::error;
        if (!round().profile.matches(reply.round.profile) || (finished && state() != reply.state)) {
            return false;
        }
        for (size_t index = 0; index < round().profile.size(); ++index) {
            const auto result = round().results[index];
            if ((finished || result != uart::Outcome::none) &&
                result != reply.round.results[index]) {
                return false;
            }
        }
        return true;
    }

    void acceptStatus(const uart::Reply& reply, uint32_t now) {
        const bool mutation = mutationPending();
        if (reply.code != uart::Code::ok) {
            if (!mutation) {
                transportLost(now);
                return;
            }
            rejected_ = pending_;
            pending_ = Request::none;
            link_ = Link::connecting;
            wait_ = {now, 0};
            return;
        }
        const bool expectsDetail = pending_ == Request::status && supportsRoundDetail_;
        const bool hasDetail = reply.round.interruption != uart::VisitInterruption::unknown;
        if (!validSnapshot(reply) || reply.hasScan || reply.hasRound != expectsRound_ ||
            reply.hasJournal != expectsJournal_ || hasDetail != expectsDetail ||
            (reply.hasRound && (!validRound(reply) || !consistentRound(reply))) ||
            (mutation && (reply.session != key_.scope || reply.lastMutation != key_.request))) {
            return;
        }
        snapshot_ = reply;
        rememberMutation(reply.lastMutation);
        if (!mutation) {
            link_ = Link::ready;
        }
        pending_ = Request::none;
        wait_ = {now, link_ == Link::ready ? pollMs : 0};
    }

    void rememberMutation(uint64_t boardId) {
        if (boardId > mutationHighWater_) {
            mutationHighWater_ = boardId;
        }
    }

    Send send_;
    void* context_;
    uint64_t nonce_;
    uint64_t boot_ = 0;
    uint64_t mutationBoot_ = 0;
    uint64_t mutationHighWater_ = 0;
    uart::ReplyKey key_{};
    uart::Reply snapshot_;
    session::Profile requestedProfile_;
    bool supportsRounds_ = false;
    bool supportsScan_ = false;
    bool supportsRoundDetail_ = false;
    bool cancelScanPending_ = false;
    uint64_t scanId_ = 0;
    size_t scanExpectedCount_ = 0;
    uart::ScanState scanState_ = uart::ScanState::idle;
    uart::ScanList scanResults_;
    bool expectsRound_ = false;
    bool supportsJournal_ = false;
    bool expectsJournal_ = false;
    Link link_ = Link::connecting;
    Request pending_ = Request::none;
    Request rejected_ = Request::none;
    struct Delay {
        uint32_t since;
        uint32_t duration;
    };
    Delay wait_{};
    uint32_t sentAt_ = 0;
    char line_[uart::wireReplyCapacity]{};
    size_t length_ = 0;
};

} // namespace flipper
} // namespace airrounds
