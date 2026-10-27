#ifndef SIKRADIO_H
#define SIKRADIO_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <sys/types.h>

// ==========================================
//    CONFIG & UTILITIES
// ==========================================

// Holds the application configuration parsed from command-line arguments.
typedef struct {
    std::string url;
    std::string scheme;
    std::string host;
    std::string hostname_for_dns;
    std::string port;
    std::string path;

    bool req_meta;
    int timeout_ms;
    bool force_ipv4;
    bool force_ipv6;
    int verbosity;
} app_config_t;

// Generates a current timestamp in "YYYY.MM.DD HH.MM.SS" format.
std::string get_timestamp();

// Logs a message to standard error if the verbosity level is sufficient.
void log_msg(const app_config_t &cfg, int level, const std::string &msg);

// Prints an error message and terminates the program with status code 1.
void fatal_error(const app_config_t &cfg, const std::string &msg);

// Prints a usage error message and terminates the program with status code 1.
void fatal_usage(const std::string &msg, int verbosity = 2);

// ==========================================
//    NETWORK INTERFACES
// ==========================================

// Abstract base class representing a generic network connection.
class NetConn {
public:
    virtual ~NetConn() = default;

    // Receives data from the connection into the buffer.
    virtual ssize_t recv(void* buf, size_t count) = 0;

    // Sends data from the buffer through the connection.
    virtual ssize_t send(const void* buf, size_t count) = 0;

    // Returns the underlying socket file descriptor (used for poll/select).
    virtual int fd() const = 0;
};

// Holds the result of the HTTP/ICY handshake process.
typedef struct {
    bool is_redirect;
    std::string new_url;
    std::map<std::string, std::string> new_cookies;
    size_t metaint;
    std::vector<uint8_t> leftover_body;
} handshake_result_t;

// Resolves the server hostname and establishes a TCP or SSL connection.
std::unique_ptr<NetConn> connect_to_server(const app_config_t &cfg);

// Sends the HTTP request and parses the server's response headers.
handshake_result_t perform_handshake(NetConn &conn, const app_config_t &cfg,
                                     const std::map<std::string, std::string> &cookies);

// ==========================================
//    STREAM PROCESSOR
// ==========================================

// State machine responsible for demultiplexing the interleaved audio and metadata stream.
class StreamProcessor {
    size_t metaint;
    size_t audio_bytes_left;
    size_t meta_bytes_left;
    bool expect_meta;
    std::vector<uint8_t> meta_buffer;

    // Possible states of the demultiplexer
    enum class State { AUDIO, META_LEN, META_DATA };
    State state;

public:
    // Initializes the processor with the metadata interval extracted from headers.
    explicit StreamProcessor(size_t metaint);

    // Processes a chunk of incoming data, separating audio from metadata.
    void process(const uint8_t* data, size_t len);
};

#endif // SIKRADIO_H