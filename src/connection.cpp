#include "connection.hpp"
#include "http.hpp"

#include <sys/epoll.h>
#include <charconv>
#include <string_view>
#include <utility>

void install_response(Connection& connection, PreparedResponse response) {
    connection.output = std::move(response.bytes);
    connection.output_sent = 0;
    connection.external_file_offset = response.external_file_offset;
    connection.external_size = response.external_size;
    connection.external_sent = 0;
    connection.close_after_response = response.close_after;
    connection.snapshot_guard = std::move(response.snapshot_guard);
}

void reset_response(Connection& connection) {
    connection.output.clear();
    connection.output_sent = 0;
    connection.external_file_offset = 0;
    connection.external_size = 0;
    connection.external_sent = 0;
    connection.close_after_response = false;
    connection.snapshot_guard.reset();
}

std::size_t buffered_input_size(const Connection& connection) {
    return connection.input.size() - connection.input_offset;
}

void compact_input(Connection& connection) {
    if (connection.input_offset == connection.input.size()) {
        connection.input.clear();
        connection.input_offset = 0;
    } else if (connection.input_offset >= static_cast<std::size_t>(16) * 1024) {
        connection.input.erase(0, connection.input_offset);
        connection.input_offset = 0;
    }
}

bool prepare_next_response(Connection& connection,
                           const Options& options,
                           const SnapshotPtr& snapshot) {
    if (connection.has_response()) {
        return true;
    }
    const std::string_view input(connection.input.data() + connection.input_offset,
                                 buffered_input_size(connection));
    const std::size_t header_end = input.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        if (input.size() > kMaxHeaderBytes) {
            install_response(
                connection,
                closed_response(431, "Request Header Fields Too Large",
                                "request header too large\n"));
            connection.input.clear();
            connection.input_offset = 0;
            return true;
        }
        return false;
    }

    const std::size_t header_size = header_end + 4;
    if (header_size > kMaxHeaderBytes) {
        install_response(
            connection,
            closed_response(431, "Request Header Fields Too Large",
                            "request header too large\n"));
        connection.input.clear();
        connection.input_offset = 0;
        return true;
    }
    // for git POST, need body too
    std::size_t content_length = 0;
    {
        // quick extract Content-Length for sizing
        std::string_view headers = input.substr(0, header_end);
        std::string lower;
        lower.reserve(headers.size());
        for (char c : headers) lower.push_back(ascii_lower(c));
        std::string_view low = lower;
        size_t pos = low.find("content-length:");
        if (pos != std::string_view::npos) {
            size_t start = pos + 15;
            while (start < low.size() && (low[start] == ' ' || low[start] == '\t')) ++start;
            size_t end = start;
            while (end < low.size() && low[end] >= '0' && low[end] <= '9') ++end;
            if (start < end) {
                auto r = std::from_chars(low.data() + start, low.data() + end, content_length);
                if (r.ec != std::errc{}) content_length = 0;
            }
        }
    }
    const std::size_t total_needed = header_size + content_length;
    if (input.size() < total_needed) {
        // need more data for body
        if (total_needed > kMaxBufferedInputBytes) {
            install_response(
                connection,
                closed_response(431, "Request Header Fields Too Large",
                                "too much pipelined input\n"));
            connection.input.clear();
            connection.input_offset = 0;
            return true;
        }
        return false;
    }

    PreparedResponse response =
        parse_http_request(options, snapshot, input.substr(0, total_needed));
    connection.input_offset += total_needed;
    compact_input(connection);
    install_response(connection, std::move(response));
    return true;
}
