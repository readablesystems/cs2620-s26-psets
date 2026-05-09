#include "lockseq_model.hh"
#include "pancydb.hh"
#include "netsim.hh"

namespace cot = cotamer;
using namespace std::chrono_literals;

// testinfo
//    Holds configuration information about this test.

struct testinfo {
    random_source randomness;
    double loss = 0.0;
    bool verbose = false;
    bool print_db = false;
    size_t nreplicas = 3;
    size_t initial_leader = 0;

    template <typename Req, typename Resp>
    void configure_network(netsim::network<Req, Resp>& net) {
        net.set_default_verbose(verbose);
        net.set_default_loss(loss);
    }
};


// pt_paxos_replica, pt_paxos_instance
//    Manage a test of a Paxos-based Pancy service.
//    Initialization is more complicated than in the simpler settings;
//    we have a type, `pt_paxos_replica`, that represents a single replica,
//    and another, `pt_paxos_instance`, that constructs the replica set.

struct pt_paxos_instance;

// The type for inter-replica messages. You will change this!
using paxos_message = /* YOUR TYPE HERE */ int;

struct pt_paxos_replica {
    size_t index_;           // index of this replica in the replica set
    size_t nreplicas_;       // number of replicas
    size_t leader_index_;    // this replica’s idea of the current leader
    // bidirectional listening port for client requests
    netsim::port<pancy::request, pancy::response>& from_clients_;
    // one-way port for inter-replica messages
    netsim::port<paxos_message>& from_replicas_;
    // channels for inter-replica messages:
    std::vector<netsim::channel<paxos_message>*> to_replicas_;
    pancy::pancydb db_;      // our copy of the database
    // ...plus anything you want to add

    pt_paxos_replica(size_t index, pt_paxos_instance&,
        client_model::network_type& client_network,
        netsim::network<paxos_message>& interreplica_network);

    cot::task<> run();
};

struct pt_paxos_instance {
    testinfo& tester;
    client_model& clients;
    std::vector<std::unique_ptr<pt_paxos_replica>> replicas;
    // ...plus anything you want to add

    pt_paxos_instance(testinfo&, client_model&,
        client_model::network_type& client_network,
        netsim::network<paxos_message>& interreplica_network);
};


// Configuration and initialization

pt_paxos_replica::pt_paxos_replica(size_t index, pt_paxos_instance& inst,
        client_model::network_type& client_network,
        netsim::network<paxos_message>& interreplica_network)
    : index_(index),
      nreplicas_(inst.tester.nreplicas),
      leader_index_(inst.tester.initial_leader),
      from_clients_(*client_network.find_port(std::format("R{}", index_))),
      from_replicas_(*interreplica_network.find_port(std::format("R{}/r", index_))) {
    for (size_t s = 0UL; s != nreplicas_; ++s) {
        to_replicas_.push_back(interreplica_network.find_channel(
            std::format("R{}/r", index_),
            std::format("R{}/r", s)));
    }
}

pt_paxos_instance::pt_paxos_instance(testinfo& tester, client_model& clients,
        client_model::network_type& client_network,
        netsim::network<paxos_message>& interreplica_network)
    : tester(tester), clients(clients), replicas(tester.nreplicas) {
    for (size_t s = 0UL; s != tester.nreplicas; ++s) {
        replicas[s].reset(new pt_paxos_replica(
            s, *this, client_network, interreplica_network
        ));
    }
}



// ********** PANCY SERVICE CODE **********

cot::task<> pt_paxos_replica::run() {
    // Your code here! The handout code just implements a single primary.

    while (true) {
        // receive request and the reply channel for the requesting client
        auto [req, reply_chan] = co_await from_clients_.receive_with_channel();

        // if not leader, redirect
        if (index_ != leader_index_) {
            co_await reply_chan->send(pancy::redirection_response{
                pancy::response_header(req, pancy::errc::redirect), leader_index_
            });
            continue;
        }

        // if leader, process message and send response
        co_await reply_chan->send(db_.process_req(req));
    }
}

// ******** end Pancy service code ********



// Test functions

cot::task<> clear_after(cot::duration d) {
    co_await cot::after(d);
    cot::clear();
}

bool try_one_seed(testinfo& tester, unsigned long seed) {
    cot::reset();   // clear old events and coroutines
    tester.randomness.seed(seed);

    // Create networks: one bidirectional client/server network plus one-way
    // network for inter-replica messages.
    client_model::network_type client_network(tester.randomness);
    tester.configure_network(client_network);
    netsim::network<paxos_message> interreplica_network(tester.randomness);
    tester.configure_network(interreplica_network);
    for (size_t s = 0; s != tester.nreplicas; ++s) {
        client_network.port(std::format("R{}", s));
        interreplica_network.port(std::format("R{}/r", s));
    }

    // Create client generator and test instance
    lockseq_model clients(tester.nreplicas, client_network);
    pt_paxos_instance inst(tester, clients, client_network, interreplica_network);

    // Start coroutines
    clients.start();
    std::vector<cot::task<>> tasks;
    for (size_t s = 0UL; s != tester.nreplicas; ++s) {
        tasks.push_back(inst.replicas[s]->run());
    }
    cot::task<> timeout_task = clear_after(100s);

    // Wait for `timeout_task`
    cot::loop();

    // Check database
    std::print("{} lock, {} write, {} clear, {} unlock\n",
               clients.lock_complete, clients.write_complete,
               clients.clear_complete, clients.unlock_complete);
    pancy::pancydb& db = inst.replicas[tester.initial_leader]->db_;
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
    { "count", required_argument, nullptr, 'n' },
    { "seed", required_argument, nullptr, 'S' },
    { "random-seeds", required_argument, nullptr, 'R' },
    { "loss", required_argument, nullptr, 'l' },
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
        } else if (ch == 'n') {
            tester.nreplicas = from_str_chars<size_t>(optarg);
        } else if (ch == 'V') {
            tester.verbose = true;
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
