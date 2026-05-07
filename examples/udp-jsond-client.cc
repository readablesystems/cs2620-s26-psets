/*
  examples/udp-jsond-client.cc
     End-to-end test of `udp-jsond`. Each RPC is one datagram out, one
     datagram back, on a single connected UDP socket.

     Usage:
         ./udp-jsond -p 21998 &
         ./udp-jsond-client -p 21998
*/

#include "cotamer/cotamer.hh"
#include "cotamer/io.hh"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <unistd.h>

namespace cot = cotamer;
using json = nlohmann::ordered_json;

namespace {

constexpr size_t max_datagram = 65507;


struct jsond_response {
    unsigned status = 0;
    json body;
};


// Send one request and receive one reply on `sock`. The socket must already
// be `udp_connect`ed to the server, so we can use plain `send`/`recv`. Both
// directions carry a single JSON document per datagram.
cot::task<jsond_response> rpc(cot::fd sock, std::string method,
                                 std::string path, json body = {}) {
    json req = {{"method", method}, {"path", path}};
    if (body.is_object()) {
        req.update(body);
    }
    std::string out = req.dump();
    std::print("→ {}\n", out);

    auto sr = co_await cot::send(sock, out.data(), out.size());
    if (!sr) {
        throw std::system_error(sr.error());
    }

    std::vector<char> buf(max_datagram);
    auto rr = co_await cot::recv(sock, buf.data(), buf.size(), 0);
    if (!rr) {
        throw std::system_error(rr.error());
    }

    std::string_view in(buf.data(), *rr);
    std::print("← {}\n", in);

    json resp = json::parse(in);
    co_return jsond_response{resp.value("status", 0U), std::move(resp)};
}


cot::task<> normal_examples(cot::fd& sock) {
    // Example 1: Create a new note.
    auto r1 = co_await rpc(sock, "POST", "/notes",
                           json{{"title", "hello"}, {"body", "world"}});
    assert(r1.status == 201);
    assert(r1.body["title"] == "hello");
    assert(r1.body["body"] == "world");
    assert(r1.body["id"].is_number_unsigned());
    uint64_t id1 = r1.body["id"];

    // Example 2: Create another.
    auto r2 = co_await rpc(sock, "POST", "/notes",
                           json{{"title", "second"}, {"body", "note"}});
    assert(r2.status == 201);
    uint64_t id2 = r2.body["id"];
    assert(id2 == id1 + 1);

    // Example 3: List all notes.
    auto r3 = co_await rpc(sock, "GET", "/notes");
    assert(r3.status == 200);
    assert(r3.body["notes"].is_array());
    assert(r3.body["notes"].size() >= 2);

    // Example 4: Fetch a specific note.
    auto r4 = co_await rpc(sock, "GET", std::format("/notes/{}", id1));
    assert(r4.status == 200);
    assert(r4.body["title"] == "hello");

    // Example 5: Modify it.
    auto r5 = co_await rpc(sock, "PUT", std::format("/notes/{}", id1),
                           json{{"title", "renamed"}});
    assert(r5.status == 200);
    assert(r5.body["title"] == "renamed");
    assert(r5.body["body"] == "world");

    // Example 6: Server statistics.
    auto r6 = co_await rpc(sock, "GET", "/stats");
    assert(r6.status == 200);
    assert(r6.body["count"] >= 2);
    assert(r6.body["server"]["name"] == "udp-jsond");
    uint64_t count6 = r6.body["count"];

    // Example 7: Delete a note.
    auto r7 = co_await rpc(sock, "DELETE", std::format("/notes/{}", id2));
    assert(r7.status == 200);
    assert(r7.body["deleted"] == id2);

    // Example 8: That decremented the count.
    auto r8 = co_await rpc(sock, "GET", "/stats");
    assert(r8.status == 200);
    assert(r8.body["count"] == count6 - 1);
}


cot::task<> error_examples(cot::fd& sock) {
    auto r1 = co_await rpc(sock, "GET", "/notes/9999");
    assert(r1.status == 404);

    auto r2 = co_await rpc(sock, "GET", "/unknown");
    assert(r2.status == 404);

    auto r3 = co_await rpc(sock, "PATCH", "/notes");
    assert(r3.status == 405);

    auto r4 = co_await rpc(sock, "POST", "/notes",
                           json{{"title", "x"}});  // missing "body"
    assert(r4.status == 400);

    auto r5 = co_await rpc(sock, "DELETE", "/notes/9999");
    assert(r5.status == 404);
}


cot::task<> main_task(std::string host, unsigned port) {
    auto sock = co_await cot::udp_connect(std::format("{}:{}", host, port));

    try {
        co_await normal_examples(sock);
        co_await error_examples(sock);
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        std::exit(1);
    }

    std::cerr << "PASS\n";
    std::exit(0);
}


void usage() {
    std::cerr << "Usage: udp-jsond-client [-h HOST] [-p PORT]\n"
              << "  -h HOST  udp-jsond hostname (default localhost)\n"
              << "  -p PORT  udp-jsond port (default 11113, matches udp-jsond)\n";
}

} // namespace


int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 11113;

    int opt;
    while ((opt = getopt(argc, argv, "?h:p:")) != -1) {
        switch (opt) {
        case 'h':
            host = optarg;
            break;
        case 'p':
            port = std::strtol(optarg, nullptr, 0);
            break;
        case '?':
        default:
            usage();
            return 1;
        }
    }
    if (optind != argc) {
        usage();
        return 1;
    }

    cot::set_clock(cot::clock::real_time);
    main_task(host, port).detach();
    cot::loop();
    return 0;
}
