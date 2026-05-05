#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"

namespace cot = cotamer;

cot::task<> run_one(cot::fd cfd, double delay) {
    cot::http_parser hp(std::move(cfd), cot::http_parser::server);
    cot::http_message req, res;

    do {
        auto req = co_await hp.receive();
        if (!hp.ok()) {
            break;
        }
        std::cerr << req.url() << '\n';
        co_await cot::after(std::chrono::duration<double>(delay));
        res.clear();
        std::string s = std::format("URL: {}\n", req.url());
        for (auto it = req.header_begin(); it != req.header_end(); ++it) {
            s += std::format("Header: {}: {}\n", it.name(), it.value());
        }
        for (auto it = req.search_param_begin(); it != req.search_param_end(); ++it) {
            s += std::format("Param: {}: {}\n", it.name(), it.value());
        }
        res.status_code(200)
            .date_header("Date", time(NULL))
            .header("Content-Type", "text/plain")
            .body(s);
        co_await hp.send(std::move(res));
    } while (hp.should_keep_alive());
}

cot::task<> start(std::string address, double delay) {
    auto lfd = co_await cot::tcp_listen(address);
    while (true) {
        run_one(co_await cot::tcp_accept(lfd), delay).detach();
    }
}

static void usage() {
    fprintf(stderr, "Usage: tamer-httpd [-p PORT] [-d DELAY]\n");
}

int main(int argc, char* argv[]) {
    int opt;
    int port = 11111;
    double delay = 0;
    while ((opt = getopt(argc, argv, "hp:t:")) != -1) {
        switch (opt) {
        case 'h':
            usage();
            exit(0);
        case 'p':
            port = strtol(optarg, 0, 0);
            break;
        case 't':
            delay = strtod(optarg, 0);
            break;
        case '?':
        usage:
            usage();
            exit(1);
        }
    }

    if (optind != argc) {
        goto usage;
    }

    cot::set_clock(cot::clock::real_time);
    cot::task<> t = start(std::format("localhost:{}", port), delay);
    cot::loop();
}
