/*
  examples/udp-jsond.cc
     A UDP-based companion to `jsond.cc`. Same in-memory “notes” API, but
     each request and response is a single UDP datagram carrying a JSON
     payload.

     Wire format:
       request: JSON object {"method": <str>, "path": <str>, ...}
       response: JSON object {"status": <int>, ...}

     Methods are the same strings as in HTTP ("GET", "POST", "PUT",
     "DELETE"); the path looks like the jsond URLs. Method-specific
     parameters and response payload fields live as additional top-level
     fields alongside `method`/`path` and `status`.

     Try it with `nc`:
       printf '{"method":"GET","path":"/notes"}' | nc -u -w1 localhost 11113
       printf '{"method":"POST","path":"/notes","title":"hi","body":"x"}' \
           | nc -u -w1 localhost 11113

     Companion client is in `udp-jsond-client.cc`.

     Usage:
         ./udp-jsond [-p PORT | -l ADDR:PORT]
*/

#include "cotamer/cotamer.hh"
#include "cotamer/io.hh"
#include <nlohmann/json.hpp>
#include <charconv>
#include <map>
#include <netdb.h>
#include <unistd.h>
#include "examples/utils.hh"

namespace cot = cotamer;
using json = nlohmann::ordered_json;


namespace {

bool verbose = false;

// Maximum datagram payload we will receive. Chosen well above the typical
// path MTU but below the 64 KiB UDP limit.
constexpr size_t max_datagram = 65507;


struct note {
    uint64_t id;
    std::string title;
    std::string body;
};

[[maybe_unused]] void to_json(json& j, const note& n) {
    j = json{{"id", n.id}, {"title", n.title}, {"body", n.body}};
}


class notes_db {
public:
    note& create(std::string title, std::string body) {
        uint64_t id = next_id_++;
        auto [it, _] = notes_.emplace(id, note{id, std::move(title), std::move(body)});
        return it->second;
    }
    note* find(uint64_t id) {
        auto it = notes_.find(id);
        return it == notes_.end() ? nullptr : &it->second;
    }
    bool erase(uint64_t id) {
        return notes_.erase(id) != 0;
    }
    const std::map<uint64_t, note>& all() const {
        return notes_;
    }
private:
    std::map<uint64_t, note> notes_;
    uint64_t next_id_ = 1;
};


// Build a response object: `"status"` plus payload fields. `payload` must
// be a JSON object; its keys appear at the top level of the response
// alongside `status`.
static json make_response(unsigned status, json payload) {
    json out = {{"status", status}};
    out.update(payload);
    return out;
}

static json error_response(unsigned status, std::string_view msg) {
    return json{{"status", status}, {"error", msg}};
}


static bool parse_note_id(std::string_view path, uint64_t& out) {
    constexpr std::string_view prefix = "/notes/";
    if (!path.starts_with(prefix)) {
        return false;
    }
    auto rest = path.substr(prefix.size());
    if (rest.empty()) {
        return false;
    }
    auto [end, ec] = std::from_chars(rest.data(), rest.data() + rest.size(), out);
    return ec == std::errc{} && end == rest.data() + rest.size();
}


static json handle(const std::string& method, const std::string& path,
                   const json& req, notes_db& db) {
    if (path == "/notes") {
        if (method == "GET") {
            json arr = json::array();
            for (auto& [_, n] : db.all()) {
                arr.push_back(n);
            }
            return make_response(200, json{{"notes", std::move(arr)}});
        }
        if (method == "POST") {
            if (!req.contains("title") || !req["title"].is_string()
                || !req.contains("body") || !req["body"].is_string()) {
                return error_response(400, "expected \"title\" and \"body\" string fields");
            }
            note& n = db.create(req["title"].get<std::string>(),
                                req["body"].get<std::string>());
            return make_response(201, json(n));
        }
        return error_response(405, "method not allowed on /notes");
    }

    uint64_t id;
    if (parse_note_id(path, id)) {
        note* n = db.find(id);
        if (method == "GET") {
            if (!n) {
                return error_response(404, "no such note");
            }
            return make_response(200, json(*n));
        }
        if (method == "PUT") {
            if (!n) {
                return error_response(404, "no such note");
            }
            if (auto it = req.find("title"); it != req.end()) {
                if (!it->is_string()) {
                    return error_response(400, "title must be a string");
                }
                n->title = it->get<std::string>();
            }
            if (auto it = req.find("body"); it != req.end()) {
                if (!it->is_string()) {
                    return error_response(400, "body must be a string");
                }
                n->body = it->get<std::string>();
            }
            return make_response(200, json(*n));
        }
        if (method == "DELETE") {
            if (!db.erase(id)) {
                return error_response(404, "no such note");
            }
            return make_response(200, json{{"deleted", id}});
        }
        return error_response(405, "method not allowed on /notes/<id>");
    }

    if (path == "/stats" && method == "GET") {
        size_t total_bytes = 0;
        json recent = json::array();
        for (auto it = db.all().rbegin(); it != db.all().rend(); ++it) {
            total_bytes += it->second.title.size() + it->second.body.size();
            if (recent.size() < 5) {
                recent.push_back(it->second.id);
            }
        }
        return make_response(200, json{
            {"count", db.all().size()},
            {"total_text_bytes", total_bytes},
            {"recent_ids", std::move(recent)},
            {"server", {{"name", "udp-jsond"}, {"version", "0.1"}}}
        });
    }

    return error_response(404, "no such route");
}


// Convert a sockaddr to a printable "host:port" form for diagnostics.
static std::string sockaddr_to_string(const sockaddr* sa, socklen_t salen) {
    char host[NI_MAXHOST], port[NI_MAXSERV];
    if (getnameinfo(sa, salen, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        return std::format("{}:{}", host, port);
    }
    return "?";
}


// Decode one datagram payload, dispatch through `handle`, and return the
// response payload. On JSON parse error we still return a well-formed
// response object so the client always sees something it can decode.
static json process_datagram(std::string_view payload, notes_db& db) {
    json req;
    try {
        req = json::parse(payload);
    } catch (const json::parse_error& e) {
        return error_response(400, std::string("invalid JSON: ") + e.what());
    }
    if (!req.is_object()
        || !req.contains("path")
        || !req["path"].is_string()
        || (req.contains("method") && !req["method"].is_string())) {
        return error_response(400, "expected {\"method\": string, \"path\": string, ...}");
    }
    return handle(req.value("method", "GET"), req["path"].get<std::string>(), req, db);
}


// Receive datagrams forever. Each iteration:
//   1. recvfrom one datagram, capturing the sender's address.
//   2. Decode/dispatch synchronously.
//   3. sendto the reply back to the sender.
// Send errors and parse errors are logged but don't kill the loop.
cot::task<> serve(cot::fd sock, notes_db& db) {
    char buf[max_datagram];
    while (true) {
        sockaddr_storage src{};
        socklen_t srclen = sizeof(src);
        auto r = co_await cot::recvfrom(sock, buf, sizeof(buf),
                                        reinterpret_cast<sockaddr*>(&src),
                                        &srclen);
        if (!r) {
            std::print(std::cerr, "recvfrom: {}\n", r.error().message());
            continue;
        }

        std::string_view payload(buf, *r);
        json resp = process_datagram(payload, db);
        std::string out = resp.dump();

        if (verbose) {
            auto peer = sockaddr_to_string(reinterpret_cast<sockaddr*>(&src), srclen);
            std::print(std::cerr, "{} ← {}\n{} → {}\n", peer, payload, peer, out);
        }

        auto sr = co_await cot::sendto(sock, out.data(), out.size(),
                                       reinterpret_cast<sockaddr*>(&src), srclen);
        if (!sr) {
            std::print(std::cerr, "sendto: {}\n", sr.error().message());
        }
    }
}

cot::task<> start(std::string address, notes_db& db) {
    try {
        for (auto& sock : co_await cot::udp_listen_all(address)) {
            serve(std::move(sock), db).detach();
        }
    } catch (std::system_error err) {
        std::print(std::cerr, "{}\n", err.what());
        exit(1);
    }
}


void usage() {
    fprintf(stderr, "Usage: udp-jsond [-p PORT | -l ADDR:PORT] [-V]\n");
}

} // namespace


int main(int argc, char* argv[]) {
    int opt;
    std::string listen = "localhost:11113";
    while ((opt = getopt(argc, argv, "hp:l:V")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            exit(0);
        case 'p': {
            auto port = from_str_chars<unsigned short>(optarg);
            listen = std::format("localhost:{}", port);
            break;
        }
        case 'l':
            listen = optarg;
            break;
        case 'V':
            verbose = true;
            break;
        case '?':
        default:
            usage();
            exit(1);
        }
    }
    if (optind != argc) {
        usage();
        exit(1);
    }

    notes_db db;
    cot::set_clock(cot::clock::real_time);
    cot::task<> t = start(std::move(listen), db);
    cot::loop();
}
