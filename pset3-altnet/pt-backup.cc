#include "lockseq_model.hh"
#include "pancydb.hh"
#include "netsim.hh"

namespace cot = cotamer;
using namespace std::chrono_literals;

// testinfo
//    Holds configuration information about this test.

struct testinfo {
    random_source randomness;
    double loss = 0.0;           // channel loss rate
    bool verbose = false;        // whether to print messages
    bool print_db = false;       // print database on success
    bool bogus = false;          // implement erroneous protocol

    template <typename Req, typename Resp>
    void configure_network(netsim::network<Req, Resp>& net) {
        net.set_default_verbose(verbose);
        net.set_default_loss(loss);
    }
};


// pt_backup_instance
//    Manage a test of a primary-backup Pancy service.

struct pt_backup_instance {
    // Primary↔backup messages: primary sends a (serial, request) pair to
    // the backup, which acknowledges with the serial it has applied.
    using pb_request_type = std::pair<uint64_t, pancy::request>;
    using pb_net_type = netsim::network<pb_request_type, uint64_t>;

    testinfo& tester_;
    client_model& clients_;

    // client-server network (kept for failure simulation, which iterates
    // over reply channels via `find_response_channel`)
    client_model::network_type& client_network_;

    // primary↔backup: primary sends requests on `p2b_channel_`, receives acks
    // on `p2b_response_port_`; backup receives requests on `p2b_port_`
    // and replies on the channel that arrives with each request.
    netsim::channel<pb_request_type>& p2b_channel_;
    netsim::port<uint64_t>& p2b_response_port_;
    netsim::port<pb_request_type, uint64_t>& p2b_port_;

    // client-server communication (bidirectional listening ports)
    netsim::port<pancy::request, pancy::response>& c2p_port_;
    netsim::port<pancy::request, pancy::response>& c2b_port_;

    // primary and backup databases
    pancy::pancydb primarydb_;
    pancy::pancydb backupdb_;
    bool primarydb_error_ = false;

    pt_backup_instance(testinfo&, client_model&,
        client_model::network_type& client_network,
        pb_net_type& pb_network);
    cot::task<> primary();
    cot::task<> backup();
};

// Configuration and initialization

pt_backup_instance::pt_backup_instance(testinfo& tester, client_model& clients,
        client_model::network_type& client_network,
        pb_net_type& pb_network)
    : tester_(tester),
      clients_(clients),
      client_network_(client_network),
      p2b_channel_(*pb_network.find_channel("R0/r", "R1/r")),
      p2b_response_port_(*pb_network.find_response_port("R0/r")),
      p2b_port_(*pb_network.find_port("R1/r")),
      c2p_port_(*client_network.find_port("R0")),
      c2b_port_(*client_network.find_port("R1")) {
}



// ********** PANCY SERVICE CODE **********

cot::task<> pt_backup_instance::primary() {
    uint64_t serial = 0;
    unsigned long loops = 0;
    while (true) {
        // Obtain request and the reply channel for the requesting client
        auto [req, reply_chan] = co_await c2p_port_.receive_with_channel();

        // Pass `-B/--bogus` to trigger this ERRONEOUS code.
        if (tester_.bogus) {
            // Process request (ERRONEOUSLY!)
            co_await reply_chan->send(primarydb_.process_req(req));
        }

        // Forward request to backup, wait for ack
        ++serial;
        while (true) {
            co_await p2b_channel_.send(std::make_pair(serial, req));
            auto ret = co_await cot::attempt(
                p2b_response_port_.receive(),
                cot::after(1s)
            );
            if (ret && *ret == serial) {
                break;
            }
        }

        if (!tester_.bogus) {
            // Apply request to database and respond on the reply channel
            co_await reply_chan->send(primarydb_.process_req(std::move(req)));
        }

        // Periodically check database and exit if erroneous
        if ((++loops % (1UL << 20)) == 0 && clients_.check(primarydb_)) {
            primarydb_error_ = true;
            co_return;
        }
    }
}

cot::task<> pt_backup_instance::backup() {
    // Operate in backup mode until timeout
    uint64_t expected_serial = 0;
    while (true) {
        // receive request from primary, client, or timeout
        auto ret = co_await cot::first(
            p2b_port_.receive_with_channel(),
            c2b_port_.receive_with_channel(),
            cot::after(5s)
        );
        if (ret.index() == 2) {
            // timeout
            break;
        }
        if (ret.index() == 1) {
            // redirect client to leader
            auto& [cmsg, reply_chan] = std::get<1>(ret);
            co_await reply_chan->send(pancy::redirection_response{
                pancy::response_header(cmsg, pancy::errc::redirect), 0
            });
            continue;
        }

        // apply request to database
        auto& [pmsg, reply_chan] = std::get<0>(ret);
        if (pmsg.first == expected_serial + 1) {
            ++expected_serial;
            backupdb_.process_req(pmsg.second);
        } else {
            assert(pmsg.first == expected_serial);
        }

        // acknowledge to primary on the same connection
        co_await reply_chan->send(expected_serial);
    }

    // If we get here, the primary has failed; it's our turn to process
    // requests.

    unsigned long loops = 0;
    while (true) {
        // Receive request, process and respond
        auto [req, reply_chan] = co_await c2b_port_.receive_with_channel();
        co_await reply_chan->send(backupdb_.process_req(req));

        if ((++loops % (1UL << 20)) == 0 && clients_.check(backupdb_)) {
            co_return;
        }
    }
}

// ******** end Pancy service code ********



// Test functions

cot::task<> clear_after(cot::duration d) {
    co_await cot::after(d);
    cot::clear();
}

cot::task<> fail_primary_after(pt_backup_instance& inst, cot::duration d) {
    co_await cot::after(d);
    // Drop all primary→backup messages on the bidirectional channel.
    inst.p2b_channel_.set_loss(1.0);
    // Drop the primary's reply channel to each existing client.
    for (size_t cid = 0; cid != inst.clients_.nclients(); ++cid) {
        if (auto* rc = inst.client_network_.find_response_channel("R0", std::format("c{}", cid))) {
            rc->set_loss(1.0);
        }
    }
}

bool try_one_seed(testinfo& tester, unsigned long seed) {
    cot::reset();   // clear old events and coroutines
    tester.randomness.seed(seed);

    // Create networks: one bidirectional client/server network and one
    // bidirectional primary↔backup network.
    client_model::network_type client_network(tester.randomness);
    tester.configure_network(client_network);
    pt_backup_instance::pb_net_type pb_network(tester.randomness);
    tester.configure_network(pb_network);
    for (size_t s = 0; s != 2; ++s) {
        client_network.port(std::format("R{}", s));
    }
    pb_network.response_port("R0/r");
    pb_network.port("R1/r");

    // Create client generator and test instance
    lockseq_model clients(2, client_network);
    pt_backup_instance inst(tester, clients, client_network, pb_network);

    // Start coroutines
    clients.start();
    cot::task<> primary_task = inst.primary();
    cot::task<> backup_task = inst.backup();
    cot::task<> failure_task = fail_primary_after(inst, 60s);
    cot::task<> timeout_task = clear_after(100s);

    // Wait for `timeout_task`
    cot::loop();

    // Check database
    std::print("{} lock, {} write, {} clear, {} unlock\n",
               clients.lock_complete, clients.write_complete,
               clients.clear_complete, clients.unlock_complete);
    pancy::pancydb& db = inst.primarydb_error_ ? inst.primarydb_ : inst.backupdb_;
    if (auto problem = clients.check(db)) {
        std::print(std::clog, "*** FAILURE on seed {} at key {}\n", seed, *problem);
        db.print_near(*problem, std::clog);
        return false;
    } else if (tester.print_db) {
        db.print(std::cout);
    }
    return true;
}


// Argument parsing

static struct option options[] = {
    { "seed", required_argument, nullptr, 'S' },
    { "random-seeds", required_argument, nullptr, 'R' },
    { "loss", required_argument, nullptr, 'l' },
    { "bogus", no_argument, nullptr, 'B' },
    { "verbose", no_argument, nullptr, 'V' },
    { "print-db", no_argument, nullptr, 'p' },
    { "quiet", no_argument, nullptr, 'q' },
    { nullptr, 0, nullptr, 0 }
};

int main(int argc, char* argv[]) {
    testinfo tester;

    std::optional<unsigned long> first_seed;
    unsigned long seed_count = 1;

    auto shortopts = short_options_for(options);
    int ch;
    while ((ch = getopt_long(argc, argv, shortopts.c_str(), options, nullptr)) != -1) {
        if (ch == 'S') {
            first_seed = from_str_chars<unsigned long>(optarg);
        } else if (ch == 'R') {
            seed_count = from_str_chars<unsigned long>(optarg);
        } else if (ch == 'l') {
            tester.loss = from_str_chars<double>(optarg);
        } else if (ch == 'V') {
            tester.verbose = true;
        } else if (ch == 'B') {
            tester.bogus = true;
        } else if (ch == 'p') {
            tester.print_db = true;
        } else {
            std::print(std::cerr, "Unknown option\n");
            return 1;
        }
    }

    bool ok;
    if (first_seed) {
        ok = try_one_seed(tester, *first_seed);
    } else {
        std::mt19937_64 seed_generator = randomly_seeded<std::mt19937_64>();
        for (unsigned long i = 0; i != seed_count; ++i) {
            if (i > 0 && i % 1000 == 0) {
                std::print(std::cerr, ".");
            }
            unsigned long seed = seed_generator();
            ok = try_one_seed(tester, seed);
            if (!ok) {
                break;
            }
        }
        if (ok && seed_count >= 1000) {
            std::print(std::cerr, "\n");
        }
    }
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
