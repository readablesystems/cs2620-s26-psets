#include "client_model.hh"

// client_model.cc
//
//    Method bodies for `client_model`.

namespace cot = cotamer;
using namespace std::chrono_literals;

client_model::client_model(size_t nreplicas, network_type& net)
    : randomness_(net.randomness()), net_(net), nreplicas_(nreplicas) {
    if (nreplicas == 0) {
        throw std::out_of_range("nreplicas must not be zero");
    }
}

void client_model::set_nclients(size_t n) {
    if (n > max_clients()) {
        throw std::out_of_range("nclients out of range");
    }
    while (clients_.size() < n) {
        size_t cid = clients_.size();
        std::string client_addr = std::format("c{}", cid);
        clients_.push_back(clientinfo{});
        clients_[cid].response_port = &net_.response_port(client_addr);
        clients_[cid].response_port->set_verbose(false);
        for (size_t r = 0; r != nreplicas_; ++r) {
            std::string replica_addr = std::format("R{}", r);
            clients_[cid].channels.push_back(net_.find_channel(client_addr, replica_addr));
            clients_[cid].channels[r]->set_verbose(false);
        }
    }
    clients_.resize(n);
}

void client_model::stop() {
}
