#include "server_runtime.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
volatile std::sig_atomic_t g_stop = 0;
void SignalHandler(int) { g_stop = 1; }
} // namespace

int main(int argc, char** argv) {
    uu::server::ServerConfig cfg{};
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            cfg.rendezvous_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((arg == "-r" || arg == "--relay-port") && i + 1 < argc) {
            cfg.relay_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-h" || arg == "--help") {
            std::printf("Usage: dir-server [-p hbbs_port (default 21116)] [-r hbbr_port (default 21117)]\n");
            return 0;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    uu::server::LiteServer server(cfg);
    if (!server.Start()) {
        std::fprintf(stderr, "[dir-server] Failed to bind UDP ports %u / %u\n",
                     cfg.rendezvous_port, cfg.relay_port);
        return 1;
    }

    std::printf("[dir-server] Started DirRemote Rendezvous (UDP :%u) + Zero-Copy Relay (UDP :%u)\n",
                server.BoundRendezvousPort(), server.BoundRelayPort());

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        auto st = server.GetStats();
        std::printf("[dir-server] peers=%llu punch_req=%llu active_relays=%llu relayed_pkts=%llu relayed_KB=%.1f\n",
                    static_cast<unsigned long long>(st.registered_peers),
                    static_cast<unsigned long long>(st.punch_requests),
                    static_cast<unsigned long long>(st.active_relay_sessions),
                    static_cast<unsigned long long>(st.relayed_packets),
                    static_cast<double>(st.relayed_bytes) / 1024.0);
        std::fflush(stdout);
    }

    server.Stop();
    std::printf("[dir-server] Gracefully stopped.\n");
    return 0;
}
