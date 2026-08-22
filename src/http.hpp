#pragma once

#include "generation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using SnapshotPtr = std::shared_ptr<const aurhub::GenerationView>;

struct Options {
    std::string snapshot;
    std::string mirror;
    std::string git_root;
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::size_t workers = 1;
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

struct QueryParam {
    std::string key;
    std::string value;
};

int hex_value(char ch);
char ascii_lower(char ch);
bool ascii_iequals(std::string_view left, std::string_view right);
std::string_view trim_ows(std::string_view value);
bool token_list_contains(std::string_view values, std::string_view wanted);
std::string url_decode(std::string_view value);
std::vector<QueryParam> parse_query(std::string_view query);
std::string_view query_value(const std::vector<QueryParam>& params, std::string_view key);

std::string git_repos_dir(const Options& options);
bool run_command(const std::vector<std::string>& args);
std::string run_git_output(const std::vector<std::string>& args, std::string_view stdin_data = {});
std::string ensure_git_repo(std::string_view pkgbase, const Options& options);

PreparedResponse handle_git(const Options& options, std::string_view method,
                            std::string_view target, std::string_view body,
                            ClientConnection connection);

std::string rpc_result(const SnapshotPtr& snapshot, std::string_view type,
                       const std::vector<std::size_t>& indexes);
std::string rpc_error(std::string_view message);
std::string http_header(int status, std::string_view reason,
                        std::string_view content_type, std::size_t content_length,
                        ClientConnection connection);
PreparedResponse owned_response(int status, std::string_view reason,
                                std::string_view content_type, const std::string& body,
                                ClientConnection connection);
PreparedResponse closed_response(int status, std::string_view reason, std::string_view body);
PreparedResponse route_request(const Options& options, const SnapshotPtr& snapshot,
                               std::string_view method, std::string_view target,
                               std::string_view body, ClientConnection connection);
PreparedResponse parse_http_request(const Options& options, const SnapshotPtr& snapshot,
                                    std::string_view request);
