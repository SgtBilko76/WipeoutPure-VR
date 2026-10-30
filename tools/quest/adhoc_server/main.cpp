// Wipeout Pure VR: the standalone ad hoc server for a dedicated Linux machine.
//
// PortableKit's own PSP ad hoc server (host/adhoc/server.cpp: adhocctl matchmaking on TCP 27312, the relay that
// carries the game's traffic on TCP 27313), built without the game: no recompiled code, no game data, only the
// two protocols. Players' consoles (the Quest app, PortableKit desktop builds, PPSSPP) log in with their product
// code and meet in the groups the game creates; everything they send goes through the relay, so no player needs a
// reachable address.
//
//   wipeout-adhoc-server [port]      (default 27312; the relay listens on the next port up)
//
// Built by tools/quest/adhoc_server/build.sh into a static binary; installed as a systemd service by install.sh.
#include "adhoc/client.hpp"
#include "adhoc/protocol.hpp"
#include "adhoc/server.hpp"
#include "profile.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace portablekit {

// What the server's code reads of a game profile: names for its log, and PURE_ for its switches
// (PURE_TRACE_ADHOC=1 logs every packet).
const GameProfile &game() {
    static const GameProfile profile = [] {
        GameProfile p{};
        p.app_name = "wipeout-adhoc-server";
        p.project_name = "Wipeout Pure VR";
        p.env_prefix = "PURE";
        p.adhoc_product_code = "UCUS98612";
        return p;
    }();
    return profile;
}

const char *env(const char *name) {
    const std::string full = std::string(game().env_prefix) + '_' + name;
    return std::getenv(full.c_str());
}

std::string env_name(const char *name) { return std::string(game().env_prefix) + '_' + name; }

} // namespace portablekit

namespace {

std::atomic<bool> stop{false};
extern "C" void on_signal(int) { stop = true; }

std::string now() {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_r(&t, &local);
    std::ostringstream out;
    out << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

} // namespace

int main(int argc, char **argv) {
    using namespace portablekit::adhoc;
    ServerConfig config;
    config.print_events = true;
    if (argc > 1) {
        const unsigned long port = std::strtoul(argv[1], nullptr, 10);
        if (port < 1024u || port > 65534u) {
            std::cerr << "usage: wipeout-adhoc-server [port 1024..65534]\n";
            return 2;
        }
        config.adhocctl_port = static_cast<std::uint16_t>(port);
    }
    Server server;
    if (!server.start(config)) {
        std::cerr << "cannot start the ad hoc server: " << server.status().error << "\n";
        return 1;
    }
    std::cout << now() << " Wipeout Pure VR ad hoc server: matchmaking on TCP " << config.adhocctl_port
              << ", relay on TCP " << relay_port_for(config.adhocctl_port) << std::endl;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::size_t players = 0;
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const ServerStatus status = server.status();
        if (status.players.size() != players) {
            players = status.players.size();
            std::cout << now() << " " << players << " player" << (players == 1 ? "" : "s") << " logged in"
                      << std::endl;
        }
    }
    server.stop();
    Client::get().shutdown();
    std::cout << now() << " stopped" << std::endl;
    return 0;
}
