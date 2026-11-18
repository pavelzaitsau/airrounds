#pragma once

#include "../firmware/esp32/session/target.h"
#include "../firmware/esp32/uart/uart_reply.h"

namespace airrounds {
namespace flipper {

enum class Link { connecting, ready, lost, incompatible };
enum class Request { none, hello, status, start, stop };

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

    bool canStart() const {
        return canMutate() && snapshot_.session != UINT32_MAX && state() != uart::State::running &&
               state() != uart::State::finalizing;
    }

    bool canStop() const { return canMutate() && state() == uart::State::running; }

    bool start(const session::Target& target, uint32_t now) {
        if (!canStart() || !session::validTarget(target)) {
            return false;
        }
        request(Request::start, "start", now, &target);
        return true;
    }

    bool stop(const uart::VisitId& expected, uint32_t now) {
        if (!canStop() || !expected.matches(visitId())) {
            return false;
        }
        request(Request::stop, "stop", now);
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
                request(Request::hello, "hello", now);
            } else {
                request(Request::status, "status", now);
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

    bool mutationPending() const { return pending_ == Request::start || pending_ == Request::stop; }

    bool canMutate() const {
        return link_ == Link::ready && !mutationPending() &&
               mutationHighWater_ < uart::readRequestBit - 1;
    }

    void reconnect(Link reason, uint32_t now) {
        boot_ = 0;
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

    void request(Request kind, const char* verb, uint32_t now,
                 const session::Target* target = nullptr) {
        pending_ = kind;
        sentAt_ = now;
        const bool mutation = mutationPending();
        const auto id = mutation ? ++mutationHighWater_ : nextNonce();
        if (mutation) {
            rejected_ = Request::none;
        }
        uint32_t scope = 0;
        if (kind == Request::start) {
            scope = snapshot_.session + 1U;
        } else if (kind == Request::stop) {
            scope = snapshot_.session;
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
        if (target != nullptr) {
            *out++ = ' ';
            constexpr size_t addressSize = sizeof(target->bssid);
            for (size_t index = 0; index < addressSize; ++index) {
                hex<2>(out + 2 * index, target->bssid[index]);
            }
            out += 2 * addressSize;
            *out++ = ' ';
            out = hex<2>(out, target->channel);
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
            boot_ = reply.envelope.key.boot;
            if (boot_ != mutationBoot_) {
                mutationBoot_ = boot_;
                mutationHighWater_ = 0;
                rejected_ = Request::none;
            }
            rememberMutation(reply.lastMutation);
            pending_ = Request::none;
            wait_ = {now, 0};
            return;
        }

        acceptStatus(reply, now);
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
        if (!validSnapshot(reply) ||
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
