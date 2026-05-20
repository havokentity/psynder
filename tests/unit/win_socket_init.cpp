// SPDX-License-Identifier: MIT
// Process-wide Winsock init for the unit-test binary.
//
// Several IPC tests open raw client sockets (connect_local, send_all, ...)
// and the editor IPC server binds a listener. On Windows every socket call
// fails until WSAStartup() has run once per process. In a real engine run
// the platform-win32 layer does that at startup, but the unit-test binary
// never initializes the platform layer — so ::socket() returned
// INVALID_SOCKET and ~6 IPC tests failed at `REQUIRE(sock_valid(s))`.
//
// A Catch2 listener runs WSAStartup before any test and WSACleanup after
// the run, process-wide, regardless of which test or TU touches sockets
// first. No-op on non-Windows.

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#endif

namespace {

class WinsockInitListener : public Catch::EventListenerBase {
   public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(Catch::TestRunInfo const&) override {
#if defined(_WIN32)
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }

    void testRunEnded(Catch::TestRunStats const&) override {
#if defined(_WIN32)
        WSACleanup();
#endif
    }
};

}  // namespace

CATCH_REGISTER_LISTENER(WinsockInitListener)
