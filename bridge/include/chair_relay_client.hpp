// Optional phone-to-chair right-stick relay. Default XAC builds do not use this.
#pragma once
#include "scx2.hpp"
#include <limits>

namespace scx2 {
class ChairRelayClient {
public:
    static constexpr std::uint64_t heartbeat_us = 50'000;
    static constexpr std::uint64_t ack_timeout_us = 250'000;
    static constexpr std::uint64_t hello_retry_us = 250'000;
    explicit ChairRelayClient(TokenSource random, void* context = nullptr)
        : random_(random), context_(context) {}

    // Call after each actual upstream USB completion, and whenever its session,
    // epoch or armed state changes. Only a live upstream may renew camera input.
    void source(const State& state, bool armed, std::uint64_t generation, std::uint64_t now) {
        if (!advance(now)) return;
        if (generation != source_generation_ || (source_active_ && !armed)) {
            stop(now);
            require_neutral_ = true;
        }
        source_generation_ = generation;
        source_active_ = armed;
        State clean;
        if (armed) { clean.axes[2] = state.axes[2]; clean.axes[3] = state.axes[3]; }
        if (!armed) require_neutral_ = true;
        else if (clean.neutral()) require_neutral_ = false;
        if (require_neutral_) clean = {};
        if (!desired_.neutral() && clean.neutral()) release_pending_ = true;
        desired_ = clean;
    }

    // The board adapter must also verify the configured peer IP and UDP port.
    void receive(const Packet& packet, std::uint64_t now) {
        if (!advance(now) || !valid(packet) || packet.format != Format::legacy ||
            packet.session != session_) return;
        if (phase_ == Phase::hello && packet.kind == Kind::ready && packet.hid_ready) {
            // If the chair only appears after an availability gap, a camera
            // hold made while it was absent must not suddenly start moving it.
            if (now - first_sent_ >= ack_timeout_us) { require_neutral_ = true; desired_ = {}; }
            boot_ = packet.boot; challenge_ = packet.challenge;
            sequence_ = 0; phase_ = Phase::arming; pending_.reset();
            return;
        }
        if (packet.boot != boot_ || packet.challenge != challenge_ || !pending_ ||
            packet.sequence != pending_->sequence) return;
        if (phase_ == Phase::stopping && packet.kind == Kind::stopped) {
            phase_ = Phase::idle; pending_.reset(); return;
        }
        if ((phase_ == Phase::arming || phase_ == Phase::live) && packet.kind == Kind::ack) {
            completed_ = pending_->state;
            pending_.reset(); phase_ = Phase::live;
        }
    }

    std::optional<Packet> take_packet(std::uint64_t now) {
        if (!advance(now)) return std::nullopt;
        if (phase_ == Phase::idle) {
            if (!source_active_ || now < next_hello_) return std::nullopt;
            Token next = random_ ? random_(context_) : Token{};
            if (next == Token{} || next == session_) {
                next_hello_ = now + hello_retry_us; return std::nullopt;
            }
            session_ = next; boot_ = {}; challenge_ = {}; sequence_ = 0;
            phase_ = Phase::hello; pending_.reset(); first_sent_ = now; last_sent_ = 0; sent_once_ = false;
        }
        if (phase_ == Phase::hello) {
            if (sent_once_ && now - last_sent_ < hello_retry_us) return std::nullopt;
            Packet hello; hello.session = session_; last_sent_ = now; sent_once_ = true;
            return hello;
        }
        if (phase_ == Phase::stopping) return retransmit(now);
        if (phase_ == Phase::arming && !pending_) return make_state({}, now);
        // A release cannot be coalesced away by a later press while USB or Wi-Fi
        // callbacks are drained in the same loop. It may supersede a press ACK.
        if (phase_ == Phase::live && release_pending_) {
            release_pending_ = false;
            if (!pending_ || !pending_->state.neutral()) return make_state({}, now);
        }
        if (pending_) return retransmit(now);
        if (phase_ == Phase::live &&
            (!(desired_ == completed_) || now - last_sent_ >= heartbeat_us))
            return make_state(desired_, now);
        return std::nullopt;
    }

    // Invalidates the source even if its last completed USB report was nonzero.
    void invalidate(std::uint64_t now) {
        if (!advance(now)) return;
        source_active_ = false; require_neutral_ = true; desired_ = {}; stop(now);
    }
    bool ready() const { return phase_ == Phase::live; }
    bool awaiting_neutral() const { return require_neutral_; }

private:
    enum class Phase { idle, hello, arming, live, stopping };
    TokenSource random_;
    void* context_;
    Phase phase_ = Phase::idle;
    Token session_{}, boot_{}, challenge_{};
    std::uint64_t source_generation_ = 0, sequence_ = 0, first_sent_ = 0, last_sent_ = 0, next_hello_ = 0;
    std::optional<std::uint64_t> last_time_;
    std::optional<Packet> pending_;
    State desired_{}, completed_{};
    bool source_active_ = false, require_neutral_ = true, release_pending_ = false, sent_once_ = false;

    bool advance(std::uint64_t now) {
        if ((last_time_ && now < *last_time_) ||
            now > std::numeric_limits<std::uint64_t>::max() - Receiver::lease_us) {
            phase_ = Phase::idle; pending_.reset(); source_active_ = false;
            desired_ = {}; require_neutral_ = true; last_time_ = now;
            next_hello_ = now; return false;
        }
        last_time_ = now;
        if (pending_ && now - first_sent_ >= ack_timeout_us) {
            if (phase_ == Phase::stopping) { phase_ = Phase::idle; pending_.reset(); }
            else { require_neutral_ = true; desired_ = {}; stop(now); }
        }
        return true;
    }
    Packet packet(Kind kind) {
        Packet out; out.kind = kind; out.session = session_; out.boot = boot_;
        out.challenge = challenge_; out.sequence = ++sequence_; return out;
    }
    void stop(std::uint64_t now) {
        release_pending_ = false; completed_ = {};
        if (phase_ == Phase::stopping) return;
        if ((phase_ == Phase::arming || phase_ == Phase::live) &&
            sequence_ < std::numeric_limits<std::uint64_t>::max() - 1) {
            pending_ = packet(Kind::stop); phase_ = Phase::stopping;
            first_sent_ = last_sent_ = now; sent_once_ = false;
        } else { phase_ = Phase::idle; pending_.reset(); }
    }
    std::optional<Packet> make_state(const State& state, std::uint64_t now) {
        if (sequence_ >= std::numeric_limits<std::uint64_t>::max() - 2) {
            require_neutral_ = true; desired_ = {}; stop(now); return retransmit(now);
        }
        pending_ = packet(Kind::state); pending_->state = state;
        first_sent_ = last_sent_ = now; sent_once_ = true; return pending_;
    }
    std::optional<Packet> retransmit(std::uint64_t now) {
        if (!pending_ || (sent_once_ && now - last_sent_ < heartbeat_us)) return std::nullopt;
        last_sent_ = now; sent_once_ = true; return pending_;
    }
};
} // namespace scx2
