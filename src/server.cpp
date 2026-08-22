#include "connection.hpp"
#include "generation.hpp"
#include "http.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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
#include <sys/wait.h>
#include <fcntl.h>
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

volatile std::sig_atomic_t g_running = 1;

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
                  const Options& options,
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
        if (!prepare_next_response(connection, options, active_snapshot)) {
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
                  const std::atomic<SnapshotPtr>& snapshots,
                  const Options& options) {
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
                prepare_next_response(connection, options, active_snapshot);
            }
            if (keep && connection.has_response()) {
                keep = drive_output(fd, connection, options, active_snapshot);
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
            workers.emplace_back([&snapshots, &options, listener, i] {
                try {
                    serve_worker(listener, snapshots, options);
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
        app.add_option("--mirror", options.mirror, "Bare mirror path for git clone");
        app.add_option("--git-root", options.git_root, "Per-pkgbase git repos dir");
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
