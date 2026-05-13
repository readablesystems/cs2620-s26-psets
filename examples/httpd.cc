#include "cotamer/cotamer.hh"
#include "cotamer/http.hh"
#include "examples/utils.hh"

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

cot::task<> run_listen(cot::fd lfd, double delay) {
    while (true) {
        run_one(co_await cot::tcp_accept(lfd), delay).detach();
    }
}

cot::task<> start(std::string address, double delay) {
    try {
        for (auto& lfd : co_await cot::tcp_listen_all(address)) {
            run_listen(std::move(lfd), delay).detach();
        }
    } catch (std::system_error err) {
        std::print(std::cerr, "{}\n", err.what());
        exit(1);
    }
}

static void usage() {
    fprintf(stderr, "Usage: httpd [-p PORT | -l ADDR:PORT] [-d DELAY]\n");
}

int main(int argc, char* argv[]) {
    int opt;
    std::string listen = "localhost:11111";
    double delay = 0;
    while ((opt = getopt(argc, argv, "hp:l:t:")) != -1) {
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
    cot::task<> t = start(std::move(listen), delay);
    cot::loop();
}
