// SPDX-License-Identifier: MIT
// Psynder — public Host surface impl. Forwards to a singleton HostImpl that
// uses the in-process LoopbackBus as its transport substrate.
// Lane 14.

#include "Net.h"
#include "HostImpl.h"

#include <vector>

namespace psynder::net {

namespace {

HostImpl& default_impl() {
    static HostImpl s;
    return s;
}

// Cached scratch buffer for poll() — avoids allocation churn each frame.
// Single-threaded by Host::Get() contract (the engine runs net polling on
// one thread per host).
struct PollScratch {
    std::vector<InboundMessage> buf;
};
PollScratch& poll_scratch() {
    static PollScratch s;
    return s;
}

}  // namespace

// ─── Host singleton ──────────────────────────────────────────────────────

Host& Host::Get() {
    static Host s;
    return s;
}

bool Host::start(const HostDesc& desc) {
    return default_impl().start(desc);
}

void Host::stop() {
    default_impl().stop();
}

PeerId Host::connect(const char* /*host*/, u16 port) {
    // Wave-A: the public Net.h surface accepts `(host, port)` so it'll bind
    // to a real socket transport in Wave B. For now we route over the
    // loopback bus by ignoring the host string and dialling the port.
    return default_impl().connect(port);
}

void Host::send(PeerId peer, std::span<const u8> bytes, bool reliable) {
    default_impl().send(peer, bytes, reliable, kChannelDefault);
}

void Host::poll(void (*on_message)(PeerId, std::span<const u8>, void*) noexcept, void* user) {
    if (!on_message)
        return;
    auto& scratch = poll_scratch().buf;
    scratch.clear();
    default_impl().poll(scratch);
    for (const InboundMessage& m : scratch) {
        on_message(m.from, std::span<const u8>(m.bytes.data(), m.bytes.size()), user);
    }
    // Drive a logical tick on each poll — Wave-A consumers (samples, tests)
    // poll once per frame so this gives us a 60Hz logical clock for RTO.
    default_impl().tick();
}

}  // namespace psynder::net
