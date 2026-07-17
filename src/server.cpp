#include "generation.hpp"
#include "search.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <simdjson.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxHeaderBytes = static_cast<std::size_t>(64) * 1024;
constexpr std::size_t kMaxBufferedInputBytes = static_cast<std::size_t>(1024) * 1024;
constexpr std::size_t kMaxResults = 500;
constexpr std::size_t kMaxResponsesPerDispatch = 64;
volatile std::sig_atomic_t g_running = 1;

using SnapshotPtr = std::shared_ptr<const aurhub::GenerationView>;

struct Options {
    std::string snapshot;
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t workers =
        std::max(1U, std::thread::hardware_concurrency());
};

struct QueryParam {
    std::string key;
    std::string value;
};

enum class ClientConnection : std::uint8_t {
    close,
    implicit_keep_alive,
    explicit_keep_alive,
};

struct PreparedResponse {
    std::string bytes;
    std::uint64_t external_file_offset = 0;
    std::size_t external_size = 0;
    bool close_after = false;
    SnapshotPtr snapshot_guard;
};

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

struct ReloadResult {
    SnapshotPtr snapshot;
    std::string error;
    std::uint64_t request_id = 0;
};

class SnapshotReloader {
public:
    explicit SnapshotReloader(std::string path) : path_(std::move(path)),
        event_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
        if (event_fd_ < 0) {
            throw std::runtime_error(std::string("eventfd: ") +
                                     std::strerror(errno));
        }
        worker_ = std::thread([this] { run(); });
    }

    ~SnapshotReloader() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        condition_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        ::close(event_fd_);
    }

    SnapshotReloader(const SnapshotReloader&) = delete;
    SnapshotReloader& operator=(const SnapshotReloader&) = delete;
    SnapshotReloader(SnapshotReloader&&) = delete;
    SnapshotReloader& operator=(SnapshotReloader&&) = delete;

    int event_fd() const {
        return event_fd_;
    }

    void request() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++requested_;
        }
        condition_.notify_one();
    }

    std::optional<ReloadResult> take_result() {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!result_.has_value()) {
            return std::nullopt;
        }
        std::optional<ReloadResult> result = std::move(result_);
        result_.reset();
        return result;
    }

private:
    void notify() const {
        constexpr std::uint64_t one = 1;
        while (::write(event_fd_, &one, sizeof(one)) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }

    void run() {
        std::uint64_t handled = 0;
        while (true) {
            std::uint64_t target = 0;
            {
                // NOLINTNEXTLINE(misc-const-correctness)
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this, handled] {
                    return stopping_ || requested_ != handled;
                });
                if (stopping_) {
                    return;
                }
                target = requested_;
            }

            ReloadResult result;
            result.request_id = target;
            try {
                result.snapshot =
                    std::make_shared<aurhub::GenerationView>(
                        path_, aurhub::SnapshotValidation::records_only);
            } catch (const std::exception& error) {
                result.error = error.what();
            }

            {
                const std::lock_guard<std::mutex> lock(mutex_);
                handled = target;
                result_ = std::move(result);
            }
            notify();
        }
    }

    std::string path_;
    int event_fd_ = -1;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    std::uint64_t requested_ = 0;
    bool stopping_ = false;
    std::optional<ReloadResult> result_;
};

void stop_server(int /*signum*/) {
    g_running = 0;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') { return ch - '0'; }
    if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
    if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
    return -1;
}

char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch + ('a' - 'A'));
    }
    return ch;
}

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (ascii_lower(left[i]) != ascii_lower(right[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim_ows(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool token_list_contains(std::string_view values, std::string_view wanted) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    while (!values.empty()) {
        const std::size_t comma = values.find(',');
        const std::string_view token =
            trim_ows(values.substr(0, comma));
        if (ascii_iequals(token, wanted)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        values.remove_prefix(comma + 1);
    }
    return false;
}

std::string url_decode(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const int high = hex_value(value[i + 1]);
            const int low = hex_value(value[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::vector<QueryParam> parse_query(std::string_view query) {
    std::vector<QueryParam> params;
    while (!query.empty()) {
        const std::size_t ampersand = query.find('&');
        const std::string_view part = ampersand == std::string_view::npos
                                          ? query
                                          : query.substr(0, ampersand);
        query = ampersand == std::string_view::npos
                    ? std::string_view{}
                    : query.substr(ampersand + 1);
        const std::size_t equals = part.find('=');
        params.push_back(QueryParam{
            url_decode(part.substr(0, equals)),
            equals == std::string_view::npos
                ? std::string{}
                : url_decode(part.substr(equals + 1)),
        });
    }
    return params;
}

std::string_view query_value(const std::vector<QueryParam>& params,
                             std::string_view key) {
    for (const QueryParam& param : params) {
        if (param.key == key) {
            return param.value;
        }
    }
    return {};
}

std::optional<std::size_t> exact_package(const aurhub::GenerationView& snapshot,
                                         std::string_view name) {
    return snapshot.find_package(name);
}

std::string rpc_result(const aurhub::GenerationView& snapshot,
                       std::string_view type,
                       const std::vector<std::size_t>& indexes) {
    std::size_t capacity = 96;
    for (const std::size_t index : indexes) {
        capacity += snapshot.json(index).size() + 1;
    }
    simdjson::builder::string_builder sb(capacity);
    sb.start_object();
    sb.append_key_value("version", 5);
    sb.append_comma();
    sb.append_key_value("type", type);
    sb.append_comma();
    sb.append_key_value("resultcount", indexes.size());
    sb.append_raw(",\"results\":");
    sb.start_array();
    for (std::size_t i = 0; i < indexes.size(); ++i) {
        if (i != 0) { sb.append_comma(); }
        sb.append_raw(snapshot.json(indexes[i]));
    }
    sb.end_array();
    sb.end_object();
    return std::string(sb);
}

std::string rpc_error(std::string_view message) {
    simdjson::builder::string_builder sb(256);
    sb.start_object();
    sb.append_key_value("version", 5);
    sb.append_comma();
    sb.append_raw(R"("type":"error","resultcount":0,"results":[],"error":)");
    sb.escape_and_append_with_quotes(message);
    sb.end_object();
    return std::string(sb);
}

std::string http_header(int status,
                        std::string_view reason,
                        std::string_view content_type,
                        std::size_t content_length,
                        ClientConnection connection) {
    std::string header;
    header.reserve(224);
    header.append("HTTP/1.1 ")
        .append(std::to_string(status))
        .push_back(' ');
    header.append(reason).append("\r\nContent-Type: ").append(content_type);
    header.append("\r\nContent-Length: ")
        .append(std::to_string(content_length))
        .append("\r\n");
    if (connection == ClientConnection::close) {
        header.append("Connection: close\r\n");
    } else if (connection == ClientConnection::explicit_keep_alive) {
        header.append("Connection: keep-alive\r\n");
    }
    header.append("Access-Control-Allow-Origin: *")
        .append("\r\nAccess-Control-Allow-Methods: GET, OPTIONS\r\n\r\n");
    return header;
}

PreparedResponse owned_response(int status,
                                std::string_view reason,
                                std::string_view content_type,
                                const std::string& body,
                                ClientConnection connection) {
    PreparedResponse response;
    response.bytes =
        http_header(status, reason, content_type, body.size(), connection);
    response.bytes.append(body);
    response.close_after = connection == ClientConnection::close;
    return response;
}

PreparedResponse closed_response(int status,
                                 std::string_view reason,
                                 std::string_view body) {
    return owned_response(status, reason, "text/plain", std::string(body),
                          ClientConnection::close);
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
PreparedResponse route_request(const SnapshotPtr& snapshot,
                               std::string_view method,
                               std::string_view target,
                               ClientConnection connection) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    if (method == "OPTIONS") {
        return owned_response(204, "No Content", "text/plain", {}, connection);
    }
    if (method != "GET") {
        return owned_response(405, "Method Not Allowed", "text/plain",
                              "method not allowed\n", connection);
    }

    const std::size_t question = target.find('?');
    const std::string_view path = target.substr(0, question);
    const std::vector<QueryParam> params =
        question == std::string_view::npos
            ? std::vector<QueryParam>{}
            : parse_query(target.substr(question + 1));

    if (path == "/health") {
        const std::string body =
            "ok\npackages: " + std::to_string(snapshot->package_count()) +
            "\ncreated_at: " + std::to_string(snapshot->created_at()) + "\n";
        return owned_response(200, "OK", "text/plain", body,
                              connection);
    }
    if (path == "/packages.gz") {
        const std::span<const std::byte> gzip = snapshot->packages_gz();
        PreparedResponse response;
        response.bytes =
            http_header(200, "OK", "application/gzip", gzip.size(), connection);
        response.external_file_offset =
            snapshot->packages_gz_file_offset();
        response.external_size = gzip.size();
        response.close_after = connection == ClientConnection::close;
        response.snapshot_guard = snapshot;
        return response;
    }
    if (path != "/rpc") {
        return owned_response(404, "Not Found", "text/plain", "not found\n",
                              connection);
    }

    const std::string_view version = query_value(params, "v");
    if (!version.empty() && version != "5") {
        return owned_response(200, "OK", "application/json",
                              rpc_error("unsupported RPC version"), connection);
    }
    const std::string_view type = query_value(params, "type");
    if (type == "search") {
        const std::string_view argument = query_value(params, "arg");
        if (argument.empty()) {
            return owned_response(200, "OK", "application/json",
                                  rpc_error("missing arg"), connection);
        }
        const std::string_view by = query_value(params, "by");
        std::vector<std::size_t> indexes;
        if (by == "name") {
            if (const auto index = exact_package(*snapshot, argument); index) {
                indexes.push_back(*index);
            }
        } else {
            const std::string needle = aurhub::normalize_search_query(argument);
            snapshot->search(needle, kMaxResults, indexes);
        }
        return owned_response(200, "OK", "application/json",
                              rpc_result(*snapshot, "search", indexes),
                              connection);
    }
    if (type == "info" || type == "multiinfo") {
        std::vector<std::size_t> indexes;
        for (const QueryParam& param : params) {
            if (param.key == "arg" || param.key == "arg[]") {
                if (const auto index = exact_package(*snapshot, param.value);
                    index) {
                    indexes.push_back(*index);
                }
            }
        }
        if (indexes.empty() && query_value(params, "arg").empty() &&
            query_value(params, "arg[]").empty()) {
            return owned_response(200, "OK", "application/json",
                                  rpc_error("missing arg"), connection);
        }
        return owned_response(200, "OK", "application/json",
                              rpc_result(*snapshot, "multiinfo", indexes),
                              connection);
    }
    return owned_response(200, "OK", "application/json",
                          rpc_error("unknown rpc type"), connection);
}

PreparedResponse parse_http_request(const SnapshotPtr& snapshot,
                                    std::string_view request) {
    const std::size_t header_end = request.find("\r\n\r\n");
    const std::size_t line_end = request.find("\r\n");
    if (header_end == std::string_view::npos ||
        line_end == std::string_view::npos || line_end > header_end) {
        return closed_response(400, "Bad Request", "bad request\n");
    }

    const std::string_view line = request.substr(0, line_end);
    const std::size_t first_space = line.find(' ');
    const std::size_t second_space =
        first_space == std::string_view::npos
            ? std::string_view::npos
            : line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos ||
        second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos ||
        first_space == 0 || second_space == first_space + 1 ||
        second_space + 1 == line.size()) {
        return closed_response(400, "Bad Request", "bad request\n");
    }

    const std::string_view method = line.substr(0, first_space);
    const std::string_view target =
        line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);
    const bool http11 = version == "HTTP/1.1";
    const bool http10 = version == "HTTP/1.0";
    if (!http11 && !http10) {
        return closed_response(505, "HTTP Version Not Supported",
                               "HTTP version not supported\n");
    }

    bool connection_close = false;
    bool connection_keep_alive = false;
    bool content_length_seen = false;
    std::uint64_t content_length = 0;
    bool transfer_encoding = false;
    std::size_t offset = line_end + 2;
    while (offset < header_end) {
        const std::size_t next = request.find("\r\n", offset);
        if (next == std::string_view::npos || next > header_end) {
            return closed_response(400, "Bad Request", "bad request\n");
        }
        const std::string_view header = request.substr(offset, next - offset);
        const std::size_t colon = header.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            header.front() == ' ' || header.front() == '\t') {
            return closed_response(400, "Bad Request", "bad request\n");
        }
        const std::string_view name = header.substr(0, colon);
        const std::string_view value = trim_ows(header.substr(colon + 1));
        if (ascii_iequals(name, "connection")) {
            connection_close =
                connection_close || token_list_contains(value, "close");
            connection_keep_alive =
                connection_keep_alive || token_list_contains(value, "keep-alive");
        } else if (ascii_iequals(name, "content-length")) {
            std::uint64_t parsed_length = 0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), parsed_length);
            if (value.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() ||
                (content_length_seen && parsed_length != content_length)) {
                return closed_response(400, "Bad Request", "bad request\n");
            }
            content_length_seen = true;
            content_length = parsed_length;
        } else if (ascii_iequals(name, "transfer-encoding")) {
            transfer_encoding = !value.empty();
        }
        offset = next + 2;
    }

    ClientConnection connection = ClientConnection::close;
    if (!connection_close) {
        if (http11) {
            connection = ClientConnection::implicit_keep_alive;
        } else if (connection_keep_alive) {
            connection = ClientConnection::explicit_keep_alive;
        }
    }
    if (transfer_encoding || content_length != 0) {
        return closed_response(400, "Bad Request",
                               "request bodies are not supported\n");
    }
    return route_request(snapshot, method, target, connection);
}

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

    const std::size_t request_size = header_end + 4;
    if (request_size > kMaxHeaderBytes) {
        install_response(
            connection,
            closed_response(431, "Request Header Fields Too Large",
                            "request header too large\n"));
        connection.input.clear();
        connection.input_offset = 0;
        return true;
    }

    PreparedResponse response =
        parse_http_request(snapshot, input.substr(0, request_size));
    connection.input_offset += request_size;
    compact_input(connection);
    install_response(connection, std::move(response));
    return true;
}

int create_listener(const Options& options) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }
    int enabled = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled)) !=
        0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("SO_REUSEPORT: " + error);
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.address.c_str(), &address.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid IPv4 listen address");
    }
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0 ||
        ::listen(fd, SOMAXCONN) != 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("listen: " + error);
    }
    return fd;
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void register_epoll(int epoll_fd, int fd, std::uint32_t events) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
        throw std::runtime_error(std::string("epoll_ctl add: ") +
                                 std::strerror(errno));
    }
}

void close_connection(int epoll_fd,
                      int fd,
                      std::unordered_map<int, Connection>& connections) {
    ::epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    connections.erase(fd);
}

bool update_interest(int epoll_fd, int fd, Connection& connection) {
    std::uint32_t desired = EPOLLRDHUP;
    if (!connection.peer_read_closed) {
        desired |= EPOLLIN;
    }
    if (connection.has_response()) {
        desired |= EPOLLOUT;
    }
    if (desired == connection.registered_events) {
        return true;
    }
    epoll_event event{};
    event.events = desired;
    event.data.fd = fd;
    if (::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) != 0) {
        return false;
    }
    connection.registered_events = desired;
    return true;
}

bool read_requests(int fd, Connection& connection) {
    std::array<char, static_cast<std::size_t>(16) * 1024> buffer{};
    while (true) {
        const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (received > 0) {
            connection.input.append(buffer.data(),
                                    static_cast<std::size_t>(received));
            if (buffered_input_size(connection) > kMaxBufferedInputBytes) {
                if (connection.has_response()) {
                    connection.close_after_response = true;
                    connection.input.clear();
                    connection.input_offset = 0;
                } else {
                    install_response(
                        connection,
                        closed_response(431, "Request Header Fields Too Large",
                                        "too much pipelined input\n"));
                    connection.input.clear();
                    connection.input_offset = 0;
                }
                return true;
            }
        } else if (received == 0) {
            connection.peer_read_closed = true;
            return true;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        } else {
            return false;
        }
    }
}

bool drive_output(int fd,
                  Connection& connection,
                  const SnapshotPtr& active_snapshot) {
    std::size_t completed = 0;
    while (connection.has_response()) {
        while (connection.output_sent < connection.output.size()) {
            const ssize_t sent =
                ::send(fd, connection.output.data() + connection.output_sent,
                       connection.output.size() - connection.output_sent,
                       MSG_NOSIGNAL);
            if (sent > 0) {
                connection.output_sent += static_cast<std::size_t>(sent);
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            } else {
                return false;
            }
        }

        while (connection.external_sent < connection.external_size) {
            if (!connection.snapshot_guard) {
                return false;
            }
            const std::uint64_t absolute_offset =
                connection.external_file_offset + connection.external_sent;
            if (absolute_offset >
                static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max())) {
                return false;
            }
            off_t offset = static_cast<off_t>(absolute_offset);
            const ssize_t sent =
                ::sendfile(
                    fd,
                    connection.snapshot_guard->packages_gz_file_descriptor(),
                    &offset,
                    connection.external_size - connection.external_sent);
            if (sent > 0) {
                connection.external_sent += static_cast<std::size_t>(sent);
            } else if (sent < 0 && errno == EINTR) {
                continue;
            } else if (sent < 0 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            } else {
                return false;
            }
        }

        const bool close_after = connection.close_after_response;
        reset_response(connection);
        ++completed;
        if (close_after) {
            return false;
        }
        if (!prepare_next_response(connection, active_snapshot)) {
            return !connection.peer_read_closed;
        }
        if (completed >= kMaxResponsesPerDispatch) {
            return true;
        }
    }
    return !connection.peer_read_closed;
}

int create_snapshot_watch(const std::string& snapshot,
                          std::string& watched_name) {
    const std::filesystem::path path(snapshot);
    watched_name = path.filename().string();
    const std::string directory = path.parent_path().string();
    const int fd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(std::string("inotify_init1: ") +
                                 std::strerror(errno));
    }
    if (::inotify_add_watch(fd, directory.c_str(),
                            IN_MOVED_TO | IN_CLOSE_WRITE) < 0) {
        const std::string error = std::strerror(errno);
        ::close(fd);
        throw std::runtime_error("inotify_add_watch: " + error);
    }
    return fd;
}

bool snapshot_changed(int inotify_fd, std::string_view watched_name) {
    alignas(inotify_event) std::array<char, static_cast<std::size_t>(16) * 1024> buffer{};
    bool changed = false;
    while (true) {
        const ssize_t size = ::read(inotify_fd, buffer.data(), buffer.size());
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return changed;
            }
            throw std::runtime_error(std::string("read inotify: ") +
                                     std::strerror(errno));
        }
        if (size == 0) {
            return changed;
        }

        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <=
               static_cast<std::size_t>(size)) {
            const auto* event = reinterpret_cast<const inotify_event*>(
                buffer.data() + offset);
            if ((event->mask & IN_Q_OVERFLOW) != 0 ||
                (event->len != 0 &&
                 std::string_view(event->name) == watched_name &&
                 (event->mask & (IN_MOVED_TO | IN_CLOSE_WRITE)) != 0)) {
                changed = true;
            }
            offset += sizeof(inotify_event) + event->len;
        }
    }
}

void drain_eventfd(int fd) {
    std::uint64_t value = 0;
    while (::read(fd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
}

void serve_worker(int listener,
                  const std::atomic<SnapshotPtr>& snapshots) {
    const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        ::close(listener);
        throw std::runtime_error(std::string("epoll_create1: ") +
                                 std::strerror(errno));
    }
    register_epoll(epoll_fd, listener, EPOLLIN);

    std::unordered_map<int, Connection> connections;
    connections.reserve(1024);
    std::array<epoll_event, 256> events{};

    while (g_running != 0) {
        const int count =
            ::epoll_wait(epoll_fd, events.data(), events.size(), 250);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("epoll_wait: ") +
                                     std::strerror(errno));
        }
        const SnapshotPtr active_snapshot =
            snapshots.load(std::memory_order_acquire);
        for (int i = 0; i < count; ++i) {
            const int fd = events[static_cast<std::size_t>(i)].data.fd;
            const std::uint32_t flags =
                events[static_cast<std::size_t>(i)].events;

            if (fd == listener) {
                while (true) {
                    const int client = ::accept4(
                        listener, nullptr, nullptr,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (client < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        if (errno == EINTR) {
                            continue;
                        }
                        throw std::runtime_error(std::string("accept4: ") +
                                                 std::strerror(errno));
                    }
                    int enabled = 1;
                    ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled,
                                 sizeof(enabled));
                    connections.emplace(client, Connection{});
                    try {
                        register_epoll(epoll_fd, client,
                                       EPOLLIN | EPOLLRDHUP);
                    } catch (...) {
                        close_connection(epoll_fd, client, connections);
                        throw;
                    }
                }
                continue;
            }

            auto connection_it = connections.find(fd);
            if (connection_it == connections.end()) {
                continue;
            }
            bool keep = (flags & (EPOLLERR | EPOLLHUP)) == 0;
            Connection& connection = connection_it->second;

            if (keep && (flags & (EPOLLIN | EPOLLRDHUP)) != 0) {
                keep = read_requests(fd, connection);
            }
            if (keep && !connection.has_response()) {
                prepare_next_response(connection, active_snapshot);
            }
            if (keep && connection.has_response()) {
                keep = drive_output(fd, connection, active_snapshot);
            }
            if (keep && connection.peer_read_closed &&
                !connection.has_response()) {
                keep = false;
            }
            if (keep) {
                keep = update_interest(epoll_fd, fd, connection);
            }
            if (!keep) {
                close_connection(epoll_fd, fd, connections);
            }
        }
    }

    for (const auto& [fd, connection] : connections) {
        (void)connection;
        ::close(fd);
    }
    ::close(epoll_fd);
    ::close(listener);
}

void serve(const Options& options,
           SnapshotPtr initial_snapshot) {
    const std::size_t initial_packages = initial_snapshot->package_count();
    std::atomic<SnapshotPtr> snapshots(std::move(initial_snapshot));

    const int control_epoll = ::epoll_create1(EPOLL_CLOEXEC);
    if (control_epoll < 0) {
        throw std::runtime_error(std::string("epoll_create1: ") +
                                 std::strerror(errno));
    }
    std::string watched_name;
    const int inotify_fd =
        create_snapshot_watch(options.snapshot, watched_name);
    SnapshotReloader reloader(options.snapshot);
    register_epoll(control_epoll, inotify_fd, EPOLLIN);
    register_epoll(control_epoll, reloader.event_fd(), EPOLLIN);

    std::vector<int> listeners;
    listeners.reserve(options.workers);
    try {
        for (std::size_t i = 0; i < options.workers; ++i) {
            listeners.push_back(create_listener(options));
        }
    } catch (...) {
        for (const int listener : listeners) {
            ::close(listener);
        }
        ::close(inotify_fd);
        ::close(control_epoll);
        throw;
    }

    std::vector<std::thread> workers;
    workers.reserve(options.workers);
    try {
        for (std::size_t i = 0; i < listeners.size(); ++i) {
            const int listener = listeners[i];
            workers.emplace_back([&snapshots, listener, i] {
                try {
                    serve_worker(listener, snapshots);
                } catch (const std::exception& error) {
                    std::cerr << "aurhubd: worker " << i << ": "
                              << error.what() << '\n';
                    g_running = 0;
                }
            });
        }
    } catch (...) {
        g_running = 0;
        for (std::size_t i = workers.size(); i < listeners.size(); ++i) {
            ::close(listeners[i]);
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        ::close(inotify_fd);
        ::close(control_epoll);
        throw;
    }

    std::cerr << "aurhubd: listening on " << options.address << ':'
              << options.port << " with " << options.workers
              << " SO_REUSEPORT workers and " << initial_packages
              << " mmap packages; watching " << options.snapshot << '\n';

    std::array<epoll_event, 8> events{};
    std::exception_ptr failure;
    try {
        while (g_running != 0) {
            const int count =
                ::epoll_wait(control_epoll, events.data(), events.size(), 250);
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("epoll_wait: ") +
                                         std::strerror(errno));
            }
            for (int i = 0; i < count; ++i) {
                const int fd = events[static_cast<std::size_t>(i)].data.fd;
                if (fd == inotify_fd) {
                    if (snapshot_changed(inotify_fd, watched_name)) {
                        reloader.request();
                    }
                } else if (fd == reloader.event_fd()) {
                    drain_eventfd(fd);
                    if (std::optional<ReloadResult> result =
                            reloader.take_result();
                        result.has_value()) {
                        if (result->snapshot) {
                            const SnapshotPtr next = std::move(result->snapshot);
                            snapshots.store(next, std::memory_order_release);
                            std::cerr
                                << "aurhubd: activated snapshot generation "
                                << result->request_id << " with "
                                << next->package_count()
                                << " packages (created_at="
                                << next->created_at() << ")\n";
                        } else {
                            std::cerr
                                << "aurhubd: rejected snapshot generation "
                                << result->request_id << ": "
                                << result->error << '\n';
                        }
                    }
                }
            }
        }
    } catch (...) {
        failure = std::current_exception();
        g_running = 0;
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
    ::close(inotify_fd);
    ::close(control_epoll);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        Options options;
        options.workers = std::max(1U, std::thread::hardware_concurrency());
        CLI::App app{"AUR RPC mirror server"};
        app.add_option("--snapshot", options.snapshot,
                       "Snapshot file")->required();
        app.add_option("--address", options.address, "Listen IPv4 address");
        app.add_option("--port", options.port, "Listen port")
            ->check(CLI::Range(1, 65535));
        app.add_option("--workers", options.workers, "Worker count")
            ->check(CLI::Range(1, 1024));
        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& e) {
            return app.exit(e);
        }
        options.snapshot = std::filesystem::absolute(options.snapshot)
                               .lexically_normal()
                               .string();
        if (!std::filesystem::exists(options.snapshot)) {
            std::cerr << "aurhubd: snapshot not found: "
                      << options.snapshot << '\n'
                      << "  Run aurhub-indexer first:\n"
                      << "  aurhub-indexer --repo <url>"
                      << " --output " << options.snapshot << '\n';
            return 1;
        }
        static_cast<void>(std::signal(SIGINT, stop_server));
        static_cast<void>(std::signal(SIGTERM, stop_server));
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
        SnapshotPtr snapshot =
            std::make_shared<aurhub::GenerationView>(
                options.snapshot, aurhub::SnapshotValidation::records_only);
        serve(options, std::move(snapshot));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhubd: " << error.what() << '\n';
        return 1;
    }
}
