#include "sikradio.h"

#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <getopt.h>
#include <poll.h>
#include <unistd.h>

// ==========================================
//    UTILITIES
// ==========================================

std::string get_timestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y.%m.%d %H.%M.%S");
    return oss.str();
}

void log_msg(const app_config_t &cfg, int level, const std::string &msg) {
    if (level <= cfg.verbosity) std::cerr << msg << "\n";
}

void fatal_error(const app_config_t &cfg, const std::string &msg) {
    if (cfg.verbosity >= 2) std::cerr << "ERROR: " << msg << "\n";
    exit(1);
}

void fatal_usage(const std::string &msg, int verbosity) {
    if (verbosity >= 2) std::cerr << "ERROR: " << msg << "\n";
    exit(1);
}

// --- ARGUMENT PARSING ---

// Parses the URL into scheme, host, port, and path components.
static void parse_url_wrapper(app_config_t& cfg) {
    std::string url = cfg.url;

    size_t scheme_pos = url.find("://");
    if (scheme_pos != std::string::npos) {
        cfg.scheme = url.substr(0, scheme_pos);
        url = url.substr(scheme_pos + 3);
        if (cfg.scheme == "https") cfg.port = "443";
    }

    size_t path_pos = url.find("/");
    if (path_pos != std::string::npos) {
        cfg.path = url.substr(path_pos);
        url = url.substr(0, path_pos);
    } else {
        cfg.path = "/";
    }

    if (!url.empty() && url[0] == '[') {
        size_t bracket_end = url.find(']');
        if (bracket_end != std::string::npos) {
            cfg.host = url.substr(0, bracket_end + 1);
            if (bracket_end + 1 < url.length() && url[bracket_end + 1] == ':') {
                cfg.port = url.substr(bracket_end + 2);
            }
        } else fatal_usage("Invalid IPv6 URL", cfg.verbosity);
    } else {
        size_t port_pos = url.find(":");
        if (port_pos != std::string::npos) {
            cfg.port = url.substr(port_pos + 1);
            cfg.host = url.substr(0, port_pos);
        } else {
            cfg.host = url;
        }
    }

    if (cfg.host.empty()) fatal_usage("Invalid URL: missing host", cfg.verbosity);

    cfg.hostname_for_dns = cfg.host;
    if (cfg.hostname_for_dns.front() == '[' && cfg.hostname_for_dns.back() == ']') {
        cfg.hostname_for_dns = cfg.hostname_for_dns.substr(1, cfg.hostname_for_dns.length() - 2);
    }
}

// Parses command-line arguments using getopt.
static app_config_t parse_arguments(int argc, char* argv[]) {
    app_config_t cfg;
    cfg.scheme = "http";
    cfg.port = "80";
    cfg.path = "/";
    cfg.req_meta = false;
    cfg.timeout_ms = 5000;
    cfg.force_ipv4 = false;
    cfg.force_ipv6 = false;
    cfg.verbosity = 2;

    int opt;
    bool url_provided = false;

    while ((opt = getopt(argc, argv, "u:mt:46v:q")) != -1) {
        switch (opt) {
            case 'u': cfg.url = optarg; url_provided = true; break;
            case 'm': cfg.req_meta = true; break;
            case 't':
                try {
                    cfg.timeout_ms = std::stoi(optarg);
                    if (cfg.timeout_ms < 100 || cfg.timeout_ms > 100000) fatal_usage("Timeout limits", cfg.verbosity);
                } catch (...) { fatal_usage("Invalid timeout", cfg.verbosity); }
                break;
            case '4': cfg.force_ipv4 = true; break;
            case '6': cfg.force_ipv6 = true; break;
            case 'v':
                try {
                    cfg.verbosity = std::stoi(optarg);
                    if (cfg.verbosity < 0 || cfg.verbosity > 4) fatal_usage("Verbosity limits", cfg.verbosity);
                } catch (...) { fatal_usage("Invalid verbosity", cfg.verbosity); }
                break;
            case 'q': cfg.verbosity = 0; break;
            default: fatal_usage("Unknown parameter", 2);
        }
    }

    if (!url_provided) fatal_usage("Missing -u", cfg.verbosity);
    parse_url_wrapper(cfg);
    return cfg;
}

// ==========================================
//    MAIN EVENT LOOP
// ==========================================

int main(int argc, char* argv[]) {
    app_config_t base_cfg = parse_arguments(argc, argv);
    bool user_quit = false;
    std::string stdin_buffer;
    bool ignore_stdin = false;

    while (!user_quit) {
        std::unique_ptr<NetConn> conn;
        handshake_result_t hs_result;

        app_config_t active_cfg = base_cfg;
        std::map<std::string, std::string> current_cookies;
        int redirect_count = 0;

        // --- 1. Connection & Handshake ---
        while (true) {
            try {
                if (active_cfg.verbosity >= 1) {
                    std::cerr << get_timestamp() << "\n";
                    std::cerr << "resolving name " << active_cfg.hostname_for_dns << "\n";
                }

                conn = connect_to_server(active_cfg);
                hs_result = perform_handshake(*conn, active_cfg, current_cookies);

                // Follow HTTP redirects up to a maximum depth
                if (hs_result.is_redirect) {
                    redirect_count++;
                    if (redirect_count > 10) fatal_error(base_cfg, "Too many redirects");

                    active_cfg.url = hs_result.new_url;
                    parse_url_wrapper(active_cfg);

                    for (const auto& kv : hs_result.new_cookies) {
                        current_cookies[kv.first] = kv.second;
                    }
                    continue;
                }
                break;
            } catch (const std::exception& e) {
                std::string msg = e.what();
                if (msg == "timeout") {
                    log_msg(base_cfg, 1, "data receiving timeout");
                    active_cfg = base_cfg;
                    current_cookies.clear();
                    redirect_count = 0;
                    continue;
                }
                log_msg(base_cfg, 2, "Connection error: " + msg);
                return 1;
            }
        }

        StreamProcessor processor(hs_result.metaint);
        if (!hs_result.leftover_body.empty()) {
            processor.process(hs_result.leftover_body.data(), hs_result.leftover_body.size());
        }

        struct pollfd fds[2];
        bool server_closed = false;
        bool timeout_triggered = false;

        // --- 2. I/O Multiplexing Loop ---
        while (!user_quit && !server_closed && !timeout_triggered) {
            fds[0].fd = ignore_stdin ? -1 : STDIN_FILENO;
            fds[0].events = POLLIN;
            fds[1].fd = conn->fd();
            fds[1].events = POLLIN;

            int ret = poll(fds, 2, base_cfg.timeout_ms);

            if (ret < 0) {
                if (errno == EINTR) continue;
                fatal_error(base_cfg, "Poll fatal error");
            }
            if (ret == 0) {
                log_msg(base_cfg, 1, "data receiving timeout");
                timeout_triggered = true;
                break;
            }

            // Handle user input from STDIN
            if (fds[0].revents & POLLIN) {
                char buf[256];
                ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
                if (r > 0) {
                    stdin_buffer.append(buf, r);
                    if (stdin_buffer.find("quit\n") != std::string::npos) {
                        user_quit = true;
                        break;
                    }
                } else if (r == 0) {
                    ignore_stdin = true;
                }
            }
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                ignore_stdin = true;
            }

            // Handle incoming stream from network socket
            if (fds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
                uint8_t buf[16384];
                ssize_t r = conn->recv(buf, sizeof(buf));

                if (r == 0) {
                    server_closed = true;
                    break;
                } else if (r < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (fds[1].revents & POLLHUP) {
                            server_closed = true;
                            break;
                        }
                        continue;
                    }
                    fatal_error(base_cfg, "Socket read error");
                } else {
                    processor.process(buf, r);
                }
            }
        }

        if (server_closed) break;
    }

    return 0;
}