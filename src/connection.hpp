#pragma once

#include "http.hpp"

#include <string>
#include <cstdint>
#include <cstddef>
#include <sys/epoll.h>

inline constexpr std::size_t kMaxHeaderBytes = static_cast<std::size_t>(64) * 1024;
inline constexpr std::size_t kMaxBufferedInputBytes = static_cast<std::size_t>(1024) * 1024;
inline constexpr std::size_t kMaxResponsesPerDispatch = 64;

struct Connection {
    std::string input;
    std::size_t input_offset = 0;
    std::string output;
    std::size_t output_sent = 0;
    std::uint64_t external_file_offset = 0;
    std::size_t external_size = 0;
    std::size_t external_sent = 0;
    bool close_after_response = false;
    bool peer_read_closed = false;
    std::uint32_t registered_events = EPOLLIN | EPOLLRDHUP;
    SnapshotPtr snapshot_guard;

    bool has_response() const {
        return !output.empty();
    }
};

void install_response(Connection& connection, PreparedResponse response);
void reset_response(Connection& connection);
std::size_t buffered_input_size(const Connection& connection);
void compact_input(Connection& connection);
bool prepare_next_response(Connection& connection, const Options& options, const SnapshotPtr& snapshot);
