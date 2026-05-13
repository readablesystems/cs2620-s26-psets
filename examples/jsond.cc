/*
  examples/jsond.cc
     A small in-memory “notes” REST API. Showcases JSON parsing of
     request bodies (POST/PUT) and JSON generation of responses
     (objects, arrays, nested values) using nlohmann::json.
     Companion clients are in `jsond-client.cc` and `jsond-curl-client.cc`.

     Architecture: `main` calls `start`, which listens on a TCP port
     and `co_await`s connections. Each accepted connection is handed
     to its own `run_one` coroutine, which parses HTTP requests via
     `cot::http_parser`, dispatches them through `handle()`, and
     writes JSON responses back. Connections stay open across
     requests via HTTP keepalive.

     Endpoints:
       GET    /notes        list all notes
       POST   /notes        create note; body = {"title": "...", "body": "..."}
       GET    /notes/<id>   fetch a note
       PUT    /notes/<id>   partial update of a note
       DELETE /notes/<id>   delete a note
       GET    /stats        aggregate counts and recent ids

     Try it with curl:
       curl -s -XPOST localhost:11112/notes \
            -H 'Content-Type: application/json' \
            -d '{"title":"hi","body":"first"}'
       curl -s localhost:11112/notes
       curl -s localhost:11112/stats

     Usage:
         ./jsond [-p PORT | -l ADDR:PORT]
*/

#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include <nlohmann/json.hpp>
#include <charconv>
#include <map>
#include <unistd.h>
#include "examples/utils.hh"

namespace cot = cotamer;
using json = nlohmann::json;


namespace {

bool verbose = false;

// A note in our toy database.
struct note {
    uint64_t id;
    std::string title;
    std::string body;
};

// `nlohmann::json` calls a free function named `to_json` to serialize a
// `note`. Defining this hook once lets the rest of the file build JSON from
// notes without repeating the field-by-field assignment.
[[maybe_unused]] void to_json(json& j, const note& n) {
    j = json{{"id", n.id}, {"title", n.title}, {"body", n.body}};
}


// In-memory note storage. Ids are assigned monotonically from 1 and
// never reused, even after deletion, so a deleted id will never later
// resolve to a different note.
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


// Build an HTTP response with the given status and JSON body. The body is
// pretty-printed with `dump(2)` (2-space indent).
//
// The compact form
//     return cot::http_message().status_code(status).body(body);
// would also work, as `http_message::body` has an overload that takes a
// `nlohmann::json`.
static cot::http_message make_response(unsigned status, const json& body) {
    cot::http_message res;
    std::string s = body.dump(2);
    s.push_back('\n');
    res.status_code(status)
        .header("Content-Type", "application/json")
        .body(std::move(s));
    return res;
}

// Build an HTTP error response
static cot::http_message error_response(unsigned status, std::string_view msg) {
    return make_response(status, json{{"error", msg}, {"status", status}});
}


// Given "/notes/<id>", parse `id` and assign `out = id`.
// Returns false if the path doesn't match the prefix or the id isn't
// a complete unsigned integer.
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


// Dispatch one parsed request to the right handler and return a
// response. The dispatch is layered:
//   1. Collection endpoints on `/notes`         (GET list, POST create)
//   2. Per-id endpoints on `/notes/<id>`        (GET, PUT, DELETE)
//   3. `/stats`                                 (aggregate JSON)
//   4. Anything else -> 404
static cot::http_message handle(const cot::http_message& req, notes_db& db) {
    auto method = req.method();
    auto path = req.path();

    // /notes — collection endpoints.
    if (path == "/notes") {
        if (method == HTTP_GET) {
            // List all notes as a JSON array. The `to_json` hook above
            // lets `arr.push_back(n)` serialize each note directly.
            json arr = json::array();
            for (auto& [_, n] : db.all()) {
                arr.push_back(n);
            }
            return make_response(200, json{{"notes", std::move(arr)}});
        }
        if (method == HTTP_POST) {
            // Parse the request body as JSON. `json::parse` throws on
            // malformed input; we catch and surface a 400.
            json body;
            try {
                body = json::parse(req.body());
            } catch (const json::parse_error& e) {
                return error_response(400, std::string("invalid JSON: ") + e.what());
            }
            // Both `title` and `body` are required and must be strings.
            if (!body.is_object()
                || !body.contains("title") || !body["title"].is_string()
                || !body.contains("body")  || !body["body"].is_string()) {
                return error_response(400, "expected {\"title\": string, \"body\": string}");
            }
            note& n = db.create(body["title"].get<std::string>(),
                                body["body"].get<std::string>());
            return make_response(201, json(n));    // 201 Created
        }
        return error_response(405, "method not allowed on /notes");
    }

    // /notes/<id> — per-note endpoints.
    uint64_t id;
    if (parse_note_id(path, id)) {
        note* n = db.find(id);
        if (method == HTTP_GET) {
            if (!n) {
                return error_response(404, "no such note");
            }
            return make_response(200, json(*n));
        }
        if (method == HTTP_PUT) {
            if (!n) {
                return error_response(404, "no such note");
            }
            json body;
            try {
                body = json::parse(req.body());
            } catch (const json::parse_error& e) {
                return error_response(400, std::string("invalid JSON: ") + e.what());
            }
            // Each field is optional on PUT; missing fields are left
            // alone. `body.find(key)` does a single lookup that tells
            // us both whether the key is present and gives us the
            // value if it is.
            if (auto it = body.find("title"); it != body.end()) {
                if (!it->is_string()) {
                    return error_response(400, "title must be a string");
                }
                n->title = it->get<std::string>();
            }
            if (auto it = body.find("body"); it != body.end()) {
                if (!it->is_string()) {
                    return error_response(400, "body must be a string");
                }
                n->body = it->get<std::string>();
            }
            return make_response(200, json(*n));
        }
        if (method == HTTP_DELETE) {
            if (!db.erase(id)) {
                return error_response(404, "no such note");
            }
            return make_response(200, json{{"deleted", id}});
        }
        return error_response(405, "method not allowed on /notes/<id>");
    }

    // /stats — aggregate stats, demonstrating nested JSON construction.
    if (path == "/stats" && method == HTTP_GET) {
        size_t total_bytes = 0;
        json recent = json::array();
        // Walk in reverse to gather the most recent ids first.
        for (auto it = db.all().rbegin(); it != db.all().rend(); ++it) {
            total_bytes += it->second.title.size() + it->second.body.size();
            if (recent.size() < 5) {
                recent.push_back(it->second.id);
            }
        }
        // Brace-initialization composes nested objects: `{"server",
        // {{"name", ...}, {"version", ...}}}` is an object inside an
        // object.
        return make_response(200, json{
            {"count", db.all().size()},
            {"total_text_bytes", total_bytes},
            {"recent_ids", std::move(recent)},
            {"server", {{"name", "jsond"}, {"version", "0.1"}}}
        });
    }

    return error_response(404, "no such route");
}


// Handle a client connection. Parse one request at a time; dispatch that
// request through `handle()` and write the corresponding response. writes the
// response, and loops until either the parser errors out or the peer asks to
// close (via `Connection: close` or HTTP/1.0 default).
//
// This task holds the only reference to `cfd`, so when it returns, the file
// descriptor will be closed.
cot::task<> handle_connection(cot::fd cfd, notes_db& db) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    cot::event sends{nullptr};
    while (true) {
        // receive request
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            break;
        }
        if (verbose) {
            std::print(std::cerr, "{} {}\n", req.method_name(), req.url());
        }

        // process request
        auto res = handle(req, db);

        // send response
        auto task = hp.send(std::move(res));
        sends = cot::all(sends, task.resolution());
        task.detach();

        if (!hp.should_keep_alive()) {
            break;
        }
    }
    co_await sends;
}

// For each accepted connection, spawn a per-connection coroutine via
// `run_one(...).detach()`.
cot::task<> run_listen(cot::fd lfd, notes_db& db) {
    while (true) {
        handle_connection(co_await cot::tcp_accept(lfd), db).detach();
    }
}

// Listen on all file descriptors indicated by `address`. (This might be more
// than one.)
cot::task<> start(std::string address, notes_db& db) {
    try {
        for (auto& lfd : co_await cot::tcp_listen_all(address)) {
            run_listen(std::move(lfd), db).detach();
        }
    } catch (std::system_error err) {
        std::print(std::cerr, "{}\n", err.what());
        exit(1);
    }
}

void usage() {
    fprintf(stderr, "Usage: jsond [-p PORT | -l ADDR:PORT] [-V]\n");
}

} // namespace

int main(int argc, char* argv[]) {
    int opt;
    std::string listen = "localhost:11112";
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
    // Keep the start task alive until cot::loop() exits.
    cot::task<> t = start(std::move(listen), db);
    cot::loop();
}
