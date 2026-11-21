#pragma once

#include <inttypes.h>
#include <stdio.h>

#include "firmware_version.h"
#include "passive_round.h"

namespace airrounds {
namespace uart {

class Protocol {
public:
    enum : size_t { replyCapacity = wireReplyCapacity };

    Protocol(VisitBackend& visit, uint64_t bootId, ScanBackend* scan = nullptr)
        : round_(visit), boot_(bootId), scan_(scan) {}

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
        size_t scanIndex = 0;
        const Code code = dispatch(request, verb, fields, scanIndex);
        // A rejection must keep the IDs from the request it rejected.
        const auto replyVersion = static_cast<unsigned>(hello ? version : request.wireVersion);
        const uint64_t replyBoot = hello ? boot_ : request.key.boot;
        const auto advertised = features | (round_.supportsJournal() ? sessionJournalFeature : 0U) |
                                (scan_ != nullptr ? scanFeature | roundDetailFeature : 0U);
        const int count = snprintf(reply, sizeof(reply),
                                   "!%02x %016" PRIx64 " %016" PRIx64 " %08" PRIx64 " %s %08" PRIx32
                                   " %s %s %016" PRIx64 " %02x",
                                   replyVersion, replyBoot, request.key.request, request.key.scope,
                                   wireName(code), session_, wireName(round_.state()),
                                   wireName(round_.outcome()), lastMutation_, advertised);
        if (count <= 0 || static_cast<size_t>(count) >= sizeof(reply)) {
            return false;
        }
        size_t used = static_cast<size_t>(count);
        if (code == Code::ok && verb.is("scan_read") && !appendScan(reply, used, scanIndex)) {
            return false;
        }
        if (code == Code::ok && verb.is("firmware") && !appendFirmware(reply, used)) {
            return false;
        }
        return finishReply(reply, used, verb);
    }

    void poll() {
        if (scan_ != nullptr) {
            scan_->pollScan();
        }
        round_.poll();
    }

private:
    bool scanning() const { return scan_ != nullptr && scan_->scanState() == ScanState::running; }

    bool validScanId(Fields& fields) const {
        uint64_t id = 0;
        return fields.take().hex(16, id) && id != 0 && id == scanId_;
    }

    bool appendScan(char (&reply)[replyCapacity], size_t& used, size_t index) const {
        if (scan_ == nullptr) {
            return false;
        }
        const auto state = scan_->scanState();
        const auto count = state == ScanState::complete ? scan_->scanList().size() : 0;
        const int header =
            snprintf(reply + used, sizeof(reply) - used, " scan %s %02x %02x", wireName(state),
                     static_cast<unsigned>(count), static_cast<unsigned>(index));
        if (header <= 0 || static_cast<size_t>(header) >= sizeof(reply) - used) {
            return false;
        }
        used += static_cast<size_t>(header);
        if (count == 0) {
            return true;
        }
        const auto& entry = scan_->scanList()[index];
        const int target = snprintf(reply + used, sizeof(reply) - used, " %012" PRIx64 " %02x ",
                                    session::addressValue(entry.target),
                                    static_cast<unsigned>(entry.target.channel));
        if (target <= 0 || static_cast<size_t>(target) >= sizeof(reply) - used) {
            return false;
        }
        used += static_cast<size_t>(target);
        if (sizeof(reply) - used < entry.nameSize * 2 + 2) {
            return false;
        }
        if (entry.nameSize == 0) {
            reply[used++] = '-';
        }
        constexpr char digits[] = "0123456789abcdef";
        for (size_t offset = 0; offset < entry.nameSize; ++offset) {
            const auto byte = entry.name[offset];
            reply[used++] = digits[byte >> 4U];
            reply[used++] = digits[byte & 0x0fU];
        }
        return true;
    }

    static bool appendFirmware(char (&reply)[replyCapacity], size_t& used) {
        const int count =
            snprintf(reply + used, sizeof(reply) - used, " firmware %s", firmwareVersion);
        if (count <= 0 || static_cast<size_t>(count) >= sizeof(reply) - used) {
            return false;
        }
        used += static_cast<size_t>(count);
        return true;
    }

    bool detailQuery(Field verb) const {
        return scan_ != nullptr && verb.is("round_status_detail");
    }

    bool journalQuery(Field verb) const {
        return round_.supportsJournal() && (verb.is("round_status_log") || detailQuery(verb));
    }

    bool finishReply(char (&reply)[replyCapacity], size_t used, Field verb) const {
        const bool journal = journalQuery(verb);
        const bool detail = detailQuery(verb);
        if (journal) {
            const auto count = snprintf(reply + used, sizeof(reply) - used, " journal %s",
                                        wireName(round_.journalState()));
            if (count <= 0 || static_cast<size_t>(count) >= sizeof(reply) - used) {
                return false;
            }
            used += static_cast<size_t>(count);
        }
        if (detail) {
            char marker[] = " interrupted 0";
            if (sizeof(reply) - used < sizeof(marker)) {
                return false;
            }
            if (round_.status().interruption == VisitInterruption::yes) {
                marker[sizeof(marker) - 2] = '1';
            }
            memcpy(reply + used, marker, sizeof(marker));
            used += sizeof(marker) - 1;
        }
        if (detail || journal || verb.is("round") || verb.is("round_status") ||
            verb.is("round_stop")) {
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

    Code dispatch(const Envelope& request, Field verb, Fields& fields, size_t& scanIndex) {
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
        if (readRequest &&
            (verb.is("status") || verb.is("round_status") || journalQuery(verb) ||
             detailQuery(verb)) &&
            fields.atEnd()) {
            return scope == 0 || scope == session_ ? Code::ok : Code::unknown_session;
        }
        if (readRequest && verb.is("firmware") && fields.atEnd()) {
            return scope == 0 ? Code::ok : Code::unknown_session;
        }
        if (readRequest && verb.is("scan_read")) {
            return readScan(scope, fields, scanIndex);
        }
        const bool starts = verb.is("start") || verb.is("round");
        const bool scanMutation = scan_ != nullptr && (verb.is("scan") || verb.is("scan_stop"));
        if (readRequest ||
            (!starts && !verb.is("stop") && !verb.is("round_stop") && !scanMutation)) {
            return Code::bad_request;
        }
        return mutate(request.key, verb, starts, fields);
    }

    Code readScan(uint32_t scope, Fields& fields, size_t& scanIndex) const {
        if (scan_ == nullptr) {
            return Code::bad_request;
        }
        if (scope != 0 || !validScanId(fields)) {
            return Code::unknown_session;
        }
        uint64_t index = 0;
        if (!fields.take().hex(2, index) || !fields.atEnd()) {
            return Code::bad_request;
        }
        const auto count = scan_->scanState() == ScanState::complete ? scan_->scanList().size() : 0;
        if (index >= (count == 0 ? 1U : count)) {
            return Code::bad_request;
        }
        scanIndex = static_cast<size_t>(index);
        return Code::ok;
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
        } else if (verb.is("scan")) {
            lastCode_ = scope == 0 && fields.atEnd() ? beginScan(request) : Code::bad_request;
        } else if (verb.is("scan_stop")) {
            lastCode_ = scope == 0 && validScanId(fields) && fields.atEnd() ? cancelScan()
                                                                            : Code::bad_request;
        } else {
            lastCode_ = fields.atEnd() ? stop(scope) : Code::bad_request;
        }
        return lastCode_;
    }

    Code beginScan(uint64_t id) {
        if (scanning() || round_.state() == State::running || round_.state() == State::finalizing) {
            return Code::busy;
        }
        scanId_ = id;
        scan_->startScan();
        return Code::ok;
    }

    Code cancelScan() {
        if (scanning()) {
            scan_->stopScan();
        }
        return scan_->scanState() == ScanState::error ? Code::radio_error : Code::ok;
    }

    Code start(uint32_t scope, Fields& fields, bool multiple) {
        session::Profile profile;
        const size_t limit =
            scan_ == nullptr ? session::Profile::legacyCapacity : session::Profile::capacity;
        do {
            if (profile.size() == limit) {
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
        if (scanning() || !round_.start({boot_, scope}, profile)) {
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
    ScanBackend* scan_ = nullptr;
    uint64_t scanId_ = 0;
    FrameReader reader_;
    uint64_t lastMutation_ = 0;
    char lastCommand_[frameCapacity]{};
    Code lastCode_ = Code::bad_request;
    uint32_t session_ = 0;
};

} // namespace uart
} // namespace airrounds
