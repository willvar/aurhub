#include "http.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <simdjson.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

constexpr std::size_t kMaxResults = 500;

std::string git_repos_dir(const Options& options) {
    if (!options.git_root.empty()) {
        return options.git_root;
    }
    if (options.mirror.empty()) {
        return {};
    }
    return (std::filesystem::path(options.mirror).parent_path() / "repos").string();
}

bool run_command(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) { execvp(argv[0], argv.data()); _exit(127); }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string run_git_output(const std::vector<std::string>& args,
                           std::string_view stdin_data) {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    if (!stdin_data.empty() && pipe(in_pipe) != 0) return {};
    if (pipe(out_pipe) != 0) {
        if (in_pipe[0] != -1) { close(in_pipe[0]); close(in_pipe[1]); }
        return {};
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        if (in_pipe[0] != -1) { close(in_pipe[0]); close(in_pipe[1]); }
        close(out_pipe[0]); close(out_pipe[1]);
        return {};
    }
    if (pid == 0) {
        if (!stdin_data.empty()) { dup2(in_pipe[0], STDIN_FILENO); }
        dup2(out_pipe[1], STDOUT_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        if (in_pipe[0] != -1) { close(in_pipe[0]); close(in_pipe[1]); }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    if (in_pipe[0] != -1) { close(in_pipe[0]); }
    close(out_pipe[1]);
    if (!stdin_data.empty()) {
        size_t off = 0;
        while (off < stdin_data.size()) {
            ssize_t w = write(in_pipe[1], stdin_data.data() + off, stdin_data.size() - off);
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) break;
            off += static_cast<size_t>(w);
        }
        close(in_pipe[1]);
    }
    std::string out;
    char buf[8192];
    while (true) {
        ssize_t r = read(out_pipe[0], buf, sizeof(buf));
        if (r > 0) out.append(buf, static_cast<size_t>(r));
        else if (r == 0) break;
        else if (errno == EINTR) continue;
        else break;
    }
    close(out_pipe[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    return out;
}

std::string ensure_git_repo(std::string_view pkgbase, const Options& options) {
    std::string repos = git_repos_dir(options);
    if (repos.empty() || options.mirror.empty()) return {};
    // validate pkgbase characters to avoid path traversal
    if (pkgbase.empty() || pkgbase.find('/') != std::string_view::npos ||
        pkgbase.find('.') != std::string_view::npos) {
        return {};
    }
    std::string repo = repos + "/" + std::string(pkgbase) + ".git";
    if (std::filesystem::exists(repo + "/objects")) {
        run_command({"git", "--git-dir=" + repo, "fetch", "-q", options.mirror,
                     "refs/heads/" + std::string(pkgbase) + ":refs/heads/master"});
        return repo;
    }
    // verify branch exists in mirror
    if (!run_command({"git", "--git-dir=" + options.mirror, "rev-parse", "--verify",
                      "refs/heads/" + std::string(pkgbase)})) {
        return {};
    }
    std::filesystem::create_directories(repos);
    if (!run_command({"git", "init", "--bare", "-q", repo})) return {};
    std::string alt = repo + "/objects/info/alternates";
    std::filesystem::create_directories(std::filesystem::path(alt).parent_path());
    {
        std::string obj = options.mirror + "/objects\n";
        int fd = open(alt.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return {};
        write(fd, obj.c_str(), obj.size());
        close(fd);
    }
    if (!run_command({"git", "--git-dir=" + repo, "fetch", "-q", options.mirror,
                      "refs/heads/" + std::string(pkgbase) + ":refs/heads/master"})) {
        std::filesystem::remove_all(repo);
        return {};
    }
    run_command({"git", "--git-dir=" + repo, "symbolic-ref", "HEAD", "refs/heads/master"});
    return repo;
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
PreparedResponse handle_git(const Options& options,
                               std::string_view method,
                               std::string_view target,
                               std::string_view body,
                               ClientConnection connection) {
    // target like /jre-jetbrains.git/info/refs?service=git-upload-pack
    // or /jre-jetbrains.git/git-upload-pack
    std::string_view path = target;
    std::string_view query;
    if (auto q = target.find('?'); q != std::string_view::npos) {
        path = target.substr(0, q);
        query = target.substr(q + 1);
    }
    // extract pkgbase: /<pkgbase>.git/...
    if (!path.starts_with("/") || (!path.ends_with(".git") && path.find(".git/") == std::string_view::npos)) {
        // check for /<name>.git/info/refs or /<name>.git/git-upload-pack
        if (path.find(".git/") == std::string_view::npos) {
            return owned_response(404, "Not Found", "text/plain", "not found\n", connection);
        }
    }
    // find .git suffix
    size_t git_pos = path.find(".git");
    if (git_pos == std::string_view::npos || git_pos == 1) {
        return owned_response(404, "Not Found", "text/plain", "not found\n", connection);
    }
    std::string_view pkgbase = path.substr(1, git_pos - 1);
    std::string_view suffix = path.substr(git_pos + 4); // after .git
    std::string repo = ensure_git_repo(pkgbase, options);
    if (repo.empty()) {
        return owned_response(404, "Not Found", "text/plain", "not found\n", connection);
    }
    if (method == "GET" && suffix == "/info/refs") {
        auto params = parse_query(query);
        if (query_value(params, "service") != "git-upload-pack") {
            return owned_response(404, "Not Found", "text/plain", "not found\n", connection);
        }
        std::string out = run_git_output({"git", "upload-pack", "--stateless-rpc", "--advertise-refs", repo});
        if (out.empty() && !std::filesystem::exists(repo + "/HEAD")) {
            return owned_response(500, "Internal Server Error", "text/plain", "git error\n", connection);
        }
        std::string body_out = "001e# service=git-upload-pack\n0000" + out;
        std::string hdr = http_header(200, "OK", "application/x-git-upload-pack-advertisement", body_out.size(), connection);
        PreparedResponse resp;
        resp.bytes = hdr + body_out;
        resp.close_after = connection == ClientConnection::close;
        return resp;
    }
    if (method == "POST" && suffix == "/git-upload-pack") {
        std::string out = run_git_output({"git", "upload-pack", "--stateless-rpc", repo}, body);
        if (out.empty()) {
            // git upload-pack may return empty on error, check repo exists
        }
        std::string hdr = http_header(200, "OK", "application/x-git-upload-pack-result", out.size(), connection);
        PreparedResponse resp;
        resp.bytes = hdr + out;
        resp.close_after = connection == ClientConnection::close;
        return resp;
    }
    return owned_response(404, "Not Found", "text/plain", "not found\n", connection);
}

PreparedResponse route_request(const Options& options,
                               const SnapshotPtr& snapshot,
                               std::string_view method,
                               std::string_view target,
                               std::string_view body,
                               ClientConnection connection) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    // git handling before method check (needs POST)
    if (target.find(".git") != std::string_view::npos) {
        return handle_git(options, method, target, body, connection);
    }
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
        const std::string health_body =
            "ok\npackages: " + std::to_string(snapshot->package_count()) +
            "\ncreated_at: " + std::to_string(snapshot->created_at()) + "\n";
        return owned_response(200, "OK", "text/plain", health_body,
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
                              rpc_result(snapshot, "search", indexes),
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
                              rpc_result(snapshot, "multiinfo", indexes),
                              connection);
    }
    return owned_response(200, "OK", "application/json",
                          rpc_error("unknown rpc type"), connection);
}

PreparedResponse parse_http_request(const Options& options,
                                    const SnapshotPtr& snapshot,
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
    bool is_git = target.find(".git") != std::string_view::npos;
    if (transfer_encoding) {
        return closed_response(400, "Bad Request",
                               "request bodies are not supported\n");
    }
    if (content_length != 0 && !(is_git && method == "POST" && target.find("git-upload-pack") != std::string_view::npos)) {
        return closed_response(400, "Bad Request",
                               "request bodies are not supported\n");
    }
    // body is after \r\n\r\n
    std::string_view body;
    if (content_length != 0) {
        if (request.size() < header_end + 4 + static_cast<std::size_t>(content_length)) {
            return closed_response(400, "Bad Request", "incomplete body\n");
        }
        body = request.substr(header_end + 4, static_cast<std::size_t>(content_length));
    }
    return route_request(options, snapshot, method, target, body, connection);
}

// overload required by http.hpp (SnapshotPtr) – forwards to GenerationView version
std::string rpc_result(const SnapshotPtr& snapshot,
                       std::string_view type,
                       const std::vector<std::size_t>& indexes) {
    std::size_t capacity = 96;
    for (const std::size_t index : indexes) {
        capacity += snapshot->json(index).size() + 1;
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
        sb.append_raw(snapshot->json(indexes[i]));
    }
    sb.end_array();
    sb.end_object();
    return std::string(sb);
}
