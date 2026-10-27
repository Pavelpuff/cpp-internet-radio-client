#include "sikradio.h"

#include <iostream>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

// --- TCP CONNECTION ---

// Implementation of NetConn for standard unencrypted TCP connections (HTTP).
class TcpConn : public NetConn {
    int sock;
public:
    explicit TcpConn(int s) : sock(s) {}
    ~TcpConn() override { close(sock); }
    ssize_t recv(void* buf, size_t count) override { return ::read(sock, buf, count); }
    ssize_t send(const void* buf, size_t count) override { return ::write(sock, buf, count); }
    int fd() const override { return sock; }
};

// --- SSL CONNECTION ---

// Implementation of NetConn for TLS/SSL encrypted connections (HTTPS).
class SslConn : public NetConn {
    int sock;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
public:
    SslConn(int s, const std::string& host) : sock(s) {
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) throw std::runtime_error("SSL context error");

        ssl = SSL_new(ctx);
        if (!ssl) {
            SSL_CTX_free(ctx);
            throw std::runtime_error("SSL obj error");
        }

        // Set Server Name Indication (SNI) for proper handshake with modern servers
        SSL_set_tlsext_host_name(ssl, host.c_str());
        SSL_set_fd(ssl, sock);

        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            throw std::runtime_error("SSL handshake failed");
        }
    }
    ~SslConn() override {
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        if (ctx) { SSL_CTX_free(ctx); }
        close(sock);
    }
    ssize_t recv(void* buf, size_t count) override { return SSL_read(ssl, buf, count); }
    ssize_t send(const void* buf, size_t count) override { return SSL_write(ssl, buf, count); }
    int fd() const override { return sock; }
};

// --- HELPER FUNCTIONS ---

// Safely sends all bytes of a string to the network connection.
static void send_all(NetConn& conn, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = conn.send(data.data() + total_sent, data.size() - total_sent);
        if (sent <= 0) throw std::runtime_error("Socket write error");
        total_sent += sent;
    }
}

// --- CORE NETWORK FUNCTIONS ---

std::unique_ptr<NetConn> connect_to_server(const app_config_t &cfg) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    if (cfg.force_ipv4 && !cfg.force_ipv6) hints.ai_family = AF_INET;
    if (cfg.force_ipv6 && !cfg.force_ipv4) hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(cfg.hostname_for_dns.c_str(), cfg.port.c_str(), &hints, &res) != 0) {
        throw std::runtime_error("DNS resolution failed");
    }

    int sockfd = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char ipstr[INET6_ADDRSTRLEN];
        void *addr = (p->ai_family == AF_INET) ? (void*)&(((struct sockaddr_in*)p->ai_addr)->sin_addr)
                                               : (void*)&(((struct sockaddr_in6*)p->ai_addr)->sin6_addr);
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        std::string ip_str = ipstr;
        if (p->ai_family == AF_INET6) ip_str = "[" + ip_str + "]";

        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;

        // Apply hardware timeouts to protect against slowloris attacks
        struct timeval tv;
        tv.tv_sec = cfg.timeout_ms / 1000;
        tv.tv_usec = (cfg.timeout_ms % 1000) * 1000;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (cfg.verbosity >= 1) {
            std::cerr << "connecting to server " << ip_str << ":" << cfg.port << "\n";
        }

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) != -1) break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);

    if (sockfd == -1) throw std::runtime_error("Connection failed");

    if (cfg.scheme == "https") return std::make_unique<SslConn>(sockfd, cfg.hostname_for_dns);
    else return std::make_unique<TcpConn>(sockfd);
}

handshake_result_t perform_handshake(NetConn &conn, const app_config_t &cfg,
                                     const std::map<std::string, std::string> &cookies) {
    std::string request = "GET " + cfg.path + " HTTP/1.1\r\n";
    request += "Host: " + cfg.host + "\r\n";
    request += "Connection: Keep-Alive\r\n";
    if (cfg.req_meta) request += "Icy-MetaData: 1\r\n";

    // Append stored cookies to the request
    if (!cookies.empty()) {
        request += "Cookie: ";
        bool first = true;
        for (const auto& kv : cookies) {
            if (!first) request += "; ";
            request += kv.first + "=" + kv.second;
            first = false;
        }
        request += "\r\n";
    }
    request += "\r\n";

    if (cfg.verbosity >= 1) std::cerr << request;

    send_all(conn, request);

    std::string header_buffer;
    std::vector<uint8_t> read_buf(8192);
    size_t header_end_pos = std::string::npos;

    // Read until the double CRLF boundary
    while (true) {
        ssize_t bytes_read = conn.recv(read_buf.data(), read_buf.size());
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) throw std::runtime_error("timeout");
            throw std::runtime_error("Socket read error during handshake");
        }
        if (bytes_read == 0) throw std::runtime_error("Server closed connection during handshake");

        header_buffer.append(reinterpret_cast<char*>(read_buf.data()), bytes_read);
        header_end_pos = header_buffer.find("\r\n\r\n");
        if (header_end_pos != std::string::npos) break;
        if (header_buffer.size() > 65536) throw std::runtime_error("Headers too large");
    }

    if (cfg.verbosity >= 1) std::cerr << header_buffer.substr(0, header_end_pos + 4);

    handshake_result_t result;
    result.is_redirect = false;
    result.metaint = 0;

    size_t start = 0;
    int line_num = 0;

    // Safely parse HTTP headers line by line
    while (start <= header_end_pos) {
        size_t end = header_buffer.find("\r\n", start);
        if (end == std::string::npos || end > header_end_pos) break;
        std::string line = header_buffer.substr(start, end - start);
        start = end + 2;

        if (line_num == 0) {
            size_t space1 = line.find(' ');
            if (space1 != std::string::npos) {
                size_t space2 = line.find(' ', space1 + 1);
                std::string code_str = line.substr(space1 + 1, space2 - space1 - 1);
                try {
                    int code = std::stoi(code_str);
                    if (code == 200) { /* OK */ }
                    else if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308 || code == 300 || code == 399) {
                        result.is_redirect = true;
                    } else fatal_error(cfg, "Bad HTTP status code: " + std::to_string(code));
                } catch (...) { fatal_error(cfg, "Invalid HTTP status"); }
            } else fatal_error(cfg, "Invalid HTTP status");
        } else {
            std::string lower_line = line;
            std::transform(lower_line.begin(), lower_line.end(), lower_line.begin(), ::tolower);

            if (lower_line.find("location:") == 0) {
                size_t val_start = 9;
                while (val_start < line.size() && (line[val_start] == ' ' || line[val_start] == '\t')) val_start++;
                result.new_url = line.substr(val_start);
            } else if (lower_line.find("set-cookie:") == 0) {
                size_t val_start = 11;
                while (val_start < line.size() && (line[val_start] == ' ' || line[val_start] == '\t')) val_start++;
                size_t val_end = line.find(';', val_start);
                if (val_end == std::string::npos) val_end = line.size();
                std::string pair = line.substr(val_start, val_end - val_start);
                size_t eq_pos = pair.find('=');
                if (eq_pos != std::string::npos) {
                    result.new_cookies[pair.substr(0, eq_pos)] = pair.substr(eq_pos + 1);
                }
            } else if (lower_line.find("icy-metaint:") == 0 && cfg.req_meta) {
                size_t val_start = 12;
                while (val_start < line.size() && (line[val_start] == ' ' || line[val_start] == '\t')) val_start++;
                try { result.metaint = std::stoull(line.substr(val_start)); } catch(...) {}
            }
        }
        line_num++;
    }

    // Capture any audio data accidentally read during the handshake
    size_t body_start_idx = header_end_pos + 4;
    if (body_start_idx < header_buffer.size()) {
        const char* body_ptr = header_buffer.data() + body_start_idx;
        result.leftover_body.assign(body_ptr, body_ptr + (header_buffer.size() - body_start_idx));
    }

    return result;
}