#include "sikradio.h"

#include <iostream>
#include <unistd.h>

// --- HELPER FUNCTIONS ---

// Safely writes exactly 'count' bytes to a file descriptor, ignoring EINTR interruptions.
static void write_all(int fd, const void* buf, size_t count) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buf);
    while (count > 0) {
        ssize_t written = write(fd, ptr, count);
        if (written < 0) {
            if (errno == EINTR) continue;
            break;
        }
        ptr += written;
        count -= written;
    }
}

// --- STREAM PROCESSOR IMPLEMENTATION ---

StreamProcessor::StreamProcessor(size_t metaint)
    : metaint(metaint), audio_bytes_left(metaint), meta_bytes_left(0), state(State::AUDIO) {
    expect_meta = (metaint > 0);
}

void StreamProcessor::process(const uint8_t* data, size_t len) {
    size_t idx = 0;
    while (idx < len) {
        // If metadata is not expected, push everything to STDOUT immediately
        if (!expect_meta) {
            write_all(STDOUT_FILENO, data + idx, len - idx);
            break;
        }

        if (state == State::AUDIO) {
            size_t to_write = std::min(len - idx, audio_bytes_left);
            write_all(STDOUT_FILENO, data + idx, to_write);
            idx += to_write;
            audio_bytes_left -= to_write;

            if (audio_bytes_left == 0) state = State::META_LEN;
        }
        else if (state == State::META_LEN) {
            uint8_t mlen = data[idx++];
            meta_bytes_left = mlen * 16; // ICY metadata length is in 16-byte blocks

            if (meta_bytes_left == 0) {
                audio_bytes_left = metaint;
                state = State::AUDIO;
            } else {
                state = State::META_DATA;
            }
        }
        else if (state == State::META_DATA) {
            size_t to_read = std::min(len - idx, meta_bytes_left);
            meta_buffer.insert(meta_buffer.end(), data + idx, data + idx + to_read);
            idx += to_read;
            meta_bytes_left -= to_read;

            // Once the full metadata block is buffered, process and output it
            if (meta_bytes_left == 0) {
                // Trim trailing null bytes (required by protocol standards)
                while (!meta_buffer.empty() && meta_buffer.back() == '\0') {
                    meta_buffer.pop_back();
                }

                if (!meta_buffer.empty()) {
                    meta_buffer.push_back('\n');
                    write_all(STDERR_FILENO, meta_buffer.data(), meta_buffer.size());
                }

                meta_buffer.clear();
                audio_bytes_left = metaint;
                state = State::AUDIO;
            }
        }
    }
}