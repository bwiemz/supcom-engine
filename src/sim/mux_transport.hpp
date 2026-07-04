#pragma once

#include "sim/net_transport.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace osc::sim {

// Multiplexes two logical channels (lobby + lockstep) over one INetTransport
// using a 1-byte tag. `pump()` drains the underlying transport once and sorts
// each message into the matching channel's inbox; each channel's `receive()`
// returns only its own messages. This keeps a lobby message from ever being
// parsed as a lockstep frame (or vice versa) across the lobby→game handoff.
class MuxTransport {
public:
    static constexpr u8 TAG_LOBBY = 0x4C; // 'L'
    static constexpr u8 TAG_GAME = 0x46;  // 'F'

    explicit MuxTransport(std::unique_ptr<INetTransport> inner)
        : inner_(std::move(inner)), lobby_(this, TAG_LOBBY), game_(this, TAG_GAME) {}

    INetTransport& lobby_channel() { return lobby_; }
    INetTransport& game_channel() { return game_; }
    INetTransport* inner() { return inner_.get(); }

    // Drain the underlying transport once, routing messages by tag.
    void pump() {
        for (auto& msg : inner_->receive()) {
            if (msg.empty()) continue;
            std::vector<u8> body(msg.begin() + 1, msg.end());
            if (msg[0] == TAG_LOBBY) lobby_.inbox_.push_back(std::move(body));
            else if (msg[0] == TAG_GAME) game_.inbox_.push_back(std::move(body));
        }
    }

private:
    struct Channel : INetTransport {
        Channel(MuxTransport* m, u8 tag) : mux_(m), tag_(tag) {}
        void broadcast(const std::vector<u8>& msg) override {
            std::vector<u8> tagged;
            tagged.reserve(msg.size() + 1);
            tagged.push_back(tag_);
            tagged.insert(tagged.end(), msg.begin(), msg.end());
            mux_->inner_->broadcast(tagged);
        }
        std::vector<std::vector<u8>> receive() override {
            std::vector<std::vector<u8>> out;
            out.swap(inbox_);
            return out;
        }
        MuxTransport* mux_;
        u8 tag_;
        std::vector<std::vector<u8>> inbox_;
    };

    std::unique_ptr<INetTransport> inner_;
    Channel lobby_, game_;
};

} // namespace osc::sim
