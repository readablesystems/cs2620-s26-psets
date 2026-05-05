/*
  examples/jsond-curl-client.cc
     Like `jsond-client.cc`, but uses Cotamer's libcurl integration
     (`cot::curl_fetch`) instead of speaking HTTP directly. Same
     end-to-end examples; libcurl manages the TCP connection pool, so
     HTTP keepalive happens automatically without our help.

     Usage:
         ./jsond -p 21997 &
         ./jsond-curl-client -p 21997
*/

#include "cotamer/cotamer.hh"
#include "cotamer/curl.hh"
#include <nlohmann/json.hpp>
#include <llhttp.h>
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

// `host:port` of the server; assigned by `main` from -h/-p flags.
std::string server_address;

struct jsond_response {
    unsigned status = 0;
    json body;
};


// rpc(method, url, body)
//    Task to make an RPC to a running `jsond` and return the result.
//
// libcurl manages its own connection pool: successive calls to
// `curl_fetch` reuse the underlying TCP connection automatically, so
// HTTP keepalive Just Works without us tracking an fd ourselves.

cot::task<jsond_response> rpc(llhttp_method method, std::string url,
                              std::string body = "") {
    // libcurl wants a full URL; jsond's path is just the tail.
    auto full_url = std::format("http://{}{}", server_address, url);

    // print request
    std::print(std::cout, "{} {}", llhttp_method_name(method), url);
    if (!body.empty()) {
        std::print(std::cout, " {}", body);
    }
    std::print("\n");

    // Drive libcurl. The configure lambda runs once before the transfer
    // to set verb-specific options. CURLOPT_COPYPOSTFIELDS makes libcurl
    // copy the body, so `body` need not outlive the lambda.
    auto resp = co_await cot::curl_fetch(std::move(full_url),
        [method, body = std::move(body)](CURL* easy) {
            // GET is the default; POST has its own option; everything
            // else (PUT, DELETE, PATCH) goes via CUSTOMREQUEST.
            if (method == HTTP_POST) {
                curl_easy_setopt(easy, CURLOPT_POST, 1L);
            } else if (method != HTTP_GET) {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST,
                                 llhttp_method_name(method));
            }
            if (!body.empty()) {
                curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, body.c_str());
            }
        });

    // print response
    std::print("→ {} {}\n", resp.status, resp.body);

    auto j = resp.body.empty() ? json() : json::parse(resp.body);
    co_return jsond_response{static_cast<unsigned>(resp.status), std::move(j)};
}



cot::task<> normal_examples() {
    // Example 1: Create a new note, and check that the response matches.
    //
    // Notes:
    // * C++11 raw strings, which look like `R"( ... )"`, avoid
    //   backslash escapes on the double-quotes in the JSON body.
    // * Read about `nlohmann::json`:
    //   https://www.studyplan.dev/pro-cpp/json - tutorial
    //   https://json.nlohmann.me/api/basic_json/ - API documentation

    auto r1 = co_await rpc(HTTP_POST, "/notes",
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
    auto r2 = co_await rpc(HTTP_POST, "/notes",
                           R"({"title":"second","body":"note"})");
    assert(r2.status == 201);
    uint64_t id2 = r2.body["id"];
    assert(id2 == id1 + 1);


    // Example 3: List all notes.
    auto r3 = co_await rpc(HTTP_GET, "/notes");
    assert(r3.status == 200);
    assert(r3.body["notes"].is_array());
    assert(r3.body["notes"].size() >= 2);


    // Example 4: Fetch a specific note.
    auto r4 = co_await rpc(HTTP_GET, std::format("/notes/{}", id1));
    assert(r4.status == 200);
    assert(r4.body["title"] == "hello");


    // Example 5: Change that note.
    auto r5 = co_await rpc(HTTP_PUT, std::format("/notes/{}", id1),
                           R"({"title":"renamed"})");
    assert(r5.status == 200);
    assert(r5.body["title"] == "renamed");
    assert(r5.body["body"] == "world");


    // Example 6: Server statistics.
    auto r6 = co_await rpc(HTTP_GET, "/stats");
    assert(r6.status == 200);
    assert(r6.body["count"] >= 2);
    assert(r6.body["recent_ids"].is_array());
    assert(r6.body["server"]["name"] == "jsond");
    uint64_t count6 = r6.body["count"];


    // Example 7: Delete a note we added.
    auto r7 = co_await rpc(HTTP_DELETE, std::format("/notes/{}", id2));
    assert(r7.status == 200);
    assert(r7.body["deleted"] == id2);


    // Example 8: That decrements the `count` statistic.
    auto r8 = co_await rpc(HTTP_GET, "/stats");
    assert(r8.status == 200);
    assert(r8.body["count"] == count6 - 1);


    // Example 9: Construct a JSON object piece by piece, then post it.
    // A `json` object behaves like a dictionary — assign with `j[key] =
    // value`. `dump()` serializes it back to a string for the request body.
    json note9;
    note9["title"] = "constructed";
    note9["body"] = "built field by field";
    auto r9 = co_await rpc(HTTP_POST, "/notes", note9.dump());
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
    auto r10 = co_await rpc(HTTP_POST, "/notes", note10.dump());
    assert(r10.status == 201);
    assert(r10.body["title"] == "tagged");
    assert(!r10.body.contains("metadata"));   // server dropped our extra field
}


// Error-path tests: bad routes, wrong methods, malformed bodies, missing
// ids. We only check the status code; rpc() prints the response body.
cot::task<> error_examples() {
    auto r1 = co_await rpc(HTTP_GET, "/notes/9999");
    assert(r1.status == 404);

    auto r2 = co_await rpc(HTTP_GET, "/unknown");
    assert(r2.status == 404);

    auto r3 = co_await rpc(HTTP_PATCH, "/notes");
    assert(r3.status == 405);

    auto r4 = co_await rpc(HTTP_POST, "/notes", "{not-json");
    assert(r4.status == 400);

    auto r5 = co_await rpc(HTTP_POST, "/notes", R"({"title":"x"})");
    assert(r5.status == 400);

    auto r6 = co_await rpc(HTTP_DELETE, "/notes/9999");
    assert(r6.status == 404);
}


cot::task<> main_task() {
    try {
        co_await normal_examples();
        co_await error_examples();
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        cot::curl_reset();
        std::exit(1);
    }
    cot::curl_reset();   // release the curl driver and its connection pool
    std::cerr << "PASS\n";
    std::exit(0);
}

void usage() {
    std::cerr << "Usage: jsond-curl-client [-h HOST] [-p PORT]\n"
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

    server_address = std::format("{}:{}", host, port);
    cot::set_clock(cot::clock::real_time);
    main_task().detach();
    cot::loop();
    return 0;
}
