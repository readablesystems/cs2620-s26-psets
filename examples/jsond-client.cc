/*
  examples/jsond-client.cc
     End-to-end test of `jsond` using Cotamer's native HTTP client
     (`cot::tcp_connect` + `cot::http_parser`). Exercises the same
     scenarios as `jsond-curl-client`, but speaks HTTP directly with
     no third-party HTTP library involved.

     Usage:
         ./jsond -p 21997 &
         ./jsond-client -p 21997
*/

#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <unistd.h>

namespace cot = cotamer;
using json = nlohmann::json;

namespace {

// Address of server; assigned by `main` from -h/-p flags.
std::string server_address;

struct jsond_response {
    unsigned status = 0;
    json body;
};


// rpc(http_parser, method, url, body)
//    Send a JSON client request to the `http_parser` connection and
//    return the response. All RPCs share one parser and TCP
//    connection: HTTP keepalive carries it across sequential
//    requests, and the ticket returned by `send_request` keeps each
//    response paired with its caller, so concurrent coroutines can
//    have requests in flight at once.

cot::task<jsond_response> rpc(cot::http_parser& hp, llhttp_method method,
                              std::string url, std::string body = "") {
    // construct request
    cot::http_message m(method, url);
    if (!body.empty()) {
        m.header("Content-Type", "application/json").body(body);
    }
    // If we had set the header `Connection: close`, the server would close
    // the connection after processing this RPC. We don’t set that, so the
    // connection remains active for the next RPC via the HTTP keepalive
    // mechanism.

    // print request
    std::print(std::cout, "{} {}", m.method_name(), m.url());
    if (!body.empty()) {
        std::print(std::cout, " {}", body);
    }
    std::print("\n");

    // send request, receive response
    auto ticket = co_await hp.send_request(std::move(m));
    auto resp = co_await hp.receive(std::move(ticket));

    // print response
    std::string b = resp.body();
    while (!b.empty() && isspace((unsigned char) b.back())) {
        b.pop_back();
    }
    std::print("→ {} {}\n", resp.status_code(), b);

    // check response and return result
    if (!hp.ok()) {
        throw std::runtime_error(std::format("http parse error: {}", hp.error_name()));
    }
    auto j = resp.body().empty() ? json() : json::parse(resp.body());
    co_return jsond_response{resp.status_code(), std::move(j)};
}


cot::task<> normal_examples(cot::http_parser& hp) {
    // Example 1: Create a new note, and check that the response matches.
    //
    // Notes:
    // * C++11 raw strings, which look like `R"( ... )"`, avoid
    //   backslash escapes on the double-quotes in the JSON body.
    // * Read about `nlohmann::json`:
    //   https://www.studyplan.dev/pro-cpp/json - tutorial
    //   https://json.nlohmann.me/api/basic_json/ - API documentation

    auto r1 = co_await rpc(hp, HTTP_POST, "/notes",
                           R"({"title":"hello","body":"world"})");
    assert(r1.status == 201); // `201 Created`
    // Check components of response
    assert(r1.body["title"] == "hello");   // json compares directly against strings, ints, bool, etc.
    assert(r1.body["body"] == "world");
    assert(r1.body["id"].is_number_unsigned());
    uint64_t id1 = r1.body["id"];
    // Alternate ways of deconstructing `json` objects:
    // - `get<T>()` throws an error if the type is wrong
    assert(r1.body["title"].get<std::string>() == "hello");
    // - `value(KEY, DEFAULT)` returns `DEFAULT` if `KEY` is missing.
    //   Use this for tolerant lookup, not for test assertions: a
    //   missing key would silently default rather than fail loudly.
    assert(r1.body.value("title", "") == "hello");


    // Example 2: Create another new note.
    auto r2 = co_await rpc(hp, HTTP_POST, "/notes",
                           R"({"title":"second","body":"note"})");
    assert(r2.status == 201);
    uint64_t id2 = r2.body["id"];
    assert(id2 == id1 + 1);


    // Example 3: List all notes.
    auto r3 = co_await rpc(hp, HTTP_GET, "/notes");
    assert(r3.status == 200);
    assert(r3.body["notes"].is_array());
    assert(r3.body["notes"].size() >= 2);


    // Example 4: Fetch a specific note.
    auto r4 = co_await rpc(hp, HTTP_GET, std::format("/notes/{}", id1));
    assert(r4.status == 200);
    assert(r4.body["title"] == "hello");


    // Example 5: Change that note.
    auto r5 = co_await rpc(hp, HTTP_PUT, std::format("/notes/{}", id1),
                           R"({"title":"renamed"})");
    assert(r5.status == 200);
    assert(r5.body["title"] == "renamed");
    assert(r5.body["body"] == "world");


    // Example 6: Server statistics.
    auto r6 = co_await rpc(hp, HTTP_GET, "/stats");
    assert(r6.status == 200);
    assert(r6.body["count"] >= 2);
    assert(r6.body["recent_ids"].is_array());
    assert(r6.body["server"]["name"] == "jsond");
    uint64_t count6 = r6.body["count"];


    // Example 7: Delete a note we added.
    auto r7 = co_await rpc(hp, HTTP_DELETE, std::format("/notes/{}", id2));
    assert(r7.status == 200);
    assert(r7.body["deleted"] == id2);


    // Example 8: That decrements the `count` statistic.
    auto r8 = co_await rpc(hp, HTTP_GET, "/stats");
    assert(r8.status == 200);
    assert(r8.body["count"] == count6 - 1);


    // Example 9: Construct a JSON object piece by piece, then post it.
    // A `json` object behaves like a dictionary — assign with `j[key] =
    // value`. `dump()` serializes it back to a string for the request body.
    json note9;
    note9["title"] = "constructed";
    note9["body"] = "built field by field";
    auto r9 = co_await rpc(hp, HTTP_POST, "/notes", note9.dump());
    assert(r9.status == 201);
    assert(r9.body["title"] == "constructed");
    assert(r9.body["body"] == "built field by field");


    // Example 10: JSON can carry richer values than `jsond` itself uses.
    // Here we build a note with brace-initialization and tack on an
    // unused `metadata` field whose value is a nested object holding an
    // array of strings and an integer.
    json note10 = {
        {"title", "tagged"},
        {"body", "see metadata"},
        {"metadata", {
            {"tags", {"work", "urgent", "draft"}},
            {"priority", 2}
        }}
    };
    auto r10 = co_await rpc(hp, HTTP_POST, "/notes", note10.dump());
    assert(r10.status == 201);
    assert(r10.body["title"] == "tagged");
    assert(!r10.body.contains("metadata"));   // server dropped our extra field
}


// Error-path tests: bad routes, wrong methods, malformed bodies, missing
// ids. We only check the status code; rpc() prints the response body.
cot::task<> error_examples(cot::http_parser& hp) {
    auto r1 = co_await rpc(hp, HTTP_GET, "/notes/9999");
    assert(r1.status == 404);

    auto r2 = co_await rpc(hp, HTTP_GET, "/unknown");
    assert(r2.status == 404);

    auto r3 = co_await rpc(hp, HTTP_PATCH, "/notes");
    assert(r3.status == 405);

    auto r4 = co_await rpc(hp, HTTP_POST, "/notes", "{not-json");
    assert(r4.status == 400);

    auto r5 = co_await rpc(hp, HTTP_POST, "/notes", R"({"title":"x"})");
    assert(r5.status == 400);

    auto r6 = co_await rpc(hp, HTTP_DELETE, "/notes/9999");
    assert(r6.status == 404);
}


cot::task<> main_task(std::string host, unsigned port) {
    // connect to server, construct parser
    auto cfd = co_await cot::tcp_connect(std::format("{}:{}", host, port));
    cot::http_parser hp(std::move(cfd), cot::http_parser::client, host);

    // pass parser through normal and error examples
    try {
        co_await normal_examples(hp);
        co_await error_examples(hp);
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        std::exit(1);
    }

    std::cerr << "PASS\n";
    std::exit(0);
}

void usage() {
    std::cerr << "Usage: jsond-client [-h HOST] [-p PORT]\n"
              << "  -h HOST  jsond hostname (default localhost)\n"
              << "  -p PORT  jsond port (default 11112, matches jsond)\n";
}

} // namespace


int main(int argc, char* argv[]) {
    std::string host = "localhost";
    int port = 11112;

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
