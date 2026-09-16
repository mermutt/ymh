// Deterministic fake MCP server (15 §11.2): newline-delimited JSON-RPC 2.0 over
// stdio, driven by a scenario argument. Never a production binary.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#include <nlohmann/json.hpp>
#include <unistd.h>

namespace {

using json = nlohmann::json;

void send(const json& message) {
    std::cout << message.dump() << '\n' << std::flush;
}

json ok(const json& id, json result) {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

json rpc_error(const json& id, int code, std::string message) {
    return {{"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", code}, {"message", std::move(message)}}}};
}

json tool_def(const std::string& name, bool bad_schema = false) {
    json schema = {{"type", "object"}, {"properties", json::object()}};
    if (bad_schema) {
        schema["additionalProperties"] = true;
    }
    return {{"name", name}, {"description", "tool " + name}, {"inputSchema", schema}};
}

void write_pidfile(const char* path) {
    if (path == nullptr) {
        return;
    }
    std::ofstream out(path);
    out << ::getpid() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::string scenario = argc > 1 ? argv[1] : "happy";
    if (scenario == "ignore_sigterm") {
        write_pidfile(argc > 2 ? argv[2] : nullptr);
    }

    if (scenario == "ignore_sigterm") {
        ::signal(SIGTERM, SIG_IGN);
    }

    int list_calls = 0;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        json message;
        try {
            message = json::parse(line);
        } catch (const std::exception&) {
            continue;
        }
        const std::string method = message.value("method", std::string{});
        const bool has_id = message.contains("id") && !message["id"].is_null();
        if (!has_id) {
            continue;
        }
        const json id = message["id"];

        if (method == "initialize") {
            if (scenario == "hang") {
                continue;
            }
            if (scenario == "bad_revision") {
                send(ok(id, {{"protocolVersion", "1999-01-01"},
                             {"capabilities", json::object()},
                             {"serverInfo", {{"name", "fake"}, {"version", "1"}}}}));
                continue;
            }
            send(ok(id, {{"protocolVersion", "2025-06-18"},
                         {"capabilities", json::object()},
                         {"serverInfo", {{"name", "fake"}, {"version", "1"}}}}));
        } else if (method == "tools/list") {
            ++list_calls;
            if (scenario == "happy") {
                const bool paged = message.contains("params") &&
                                   message["params"].contains("cursor");
                if (!paged) {
                    send(ok(id, {{"tools", json::array({tool_def("alpha")})},
                                 {"nextCursor", "page2"}}));
                } else {
                    send(ok(id, {{"tools", json::array({tool_def("beta")})}}));
                }
            } else if (scenario == "bad_schema") {
                send(ok(id, {{"tools", json::array({tool_def("good"), tool_def("bad", true)})}}));
            } else if (scenario == "list_changed") {
                if (list_calls == 1) {
                    send(ok(id, {{"tools", json::array({tool_def("alpha")})}}));
                    send({{"jsonrpc", "2.0"},
                          {"method", "notifications/tools/list_changed"}});
                } else {
                    send(ok(id, {{"tools",
                                  json::array({tool_def("alpha"), tool_def("beta")})}}));
                }
            } else {
                send(ok(id, {{"tools", json::array({tool_def("echo")})}}));
            }
        } else if (method == "tools/call") {
            if (scenario == "crash_mid_call") {
                ::_exit(3);
            }
            if (scenario == "crash_once") {
                const char* marker = argc > 2 ? argv[2] : nullptr;
                if (marker != nullptr) {
                    std::ifstream existing(marker);
                    if (!existing.good()) {
                        std::ofstream(marker).close();
                        ::_exit(3);
                    }
                }
            }
            if (scenario == "slow_call" || scenario == "ignore_cancel") {
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (scenario == "is_error") {
                send(ok(id, {{"content", json::array({{{"type", "text"}, {"text", "boom"}}})},
                             {"isError", true}}));
                continue;
            }
            if (scenario == "rpc_error") {
                send(rpc_error(id, -32000, "server error"));
                continue;
            }
            if (scenario == "huge") {
                const std::string blob(16u * 1024u * 1024u, 'x');
                send(ok(id, {{"content", json::array({{{"type", "text"}, {"text", blob}}})}}));
                continue;
            }
            const std::string name =
                message.value("params", json::object()).value("name", std::string{});
            send(ok(id, {{"content",
                          json::array({{{"type", "text"}, {"text", "echo:" + name}}})}}));
        } else if (method == "ping") {
            send(ok(id, json::object()));
        } else {
            send(rpc_error(id, -32601, "Method not found"));
        }
    }

    if (scenario == "ignore_sigterm") {
        for (;;) {
            ::pause();
        }
    }
    return 0;
}
