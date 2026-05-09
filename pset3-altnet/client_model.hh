#pragma once
#include "pancy_msgs.hh"
#include "pancydb.hh"
#include "netsim.hh"
#include "cotamer/cotamer.hh"
#include <optional>
#include <vector>

// client_model
//
//    Models a set of Pancy clients.
//
//    The `client_model` abstract class knows how to connect to a set of
//    replicas. It encapsulates ports, channels, and a source of randomness,
//    and has convenience functions for sending and receiving messages.
//
//    Each simulated client has its own ephemeral address (`c{N}`) and its
//    own `channel<request>`s for sending requests. Responses
//    arrive on the client’s response port.
//
//    Derived classes are expected to implement specific test protocols.
//    `start()` starts the test protocol, and `stop()` stops it (as does
//    destroying the `client_model`).
//
//    We provide one derived test protocol in `lockseq_model.hh/cc`.

class client_model {
public:
    using network_type = netsim::network<pancy::request, pancy::response>;
    using channel_type = netsim::channel<pancy::request>;
    using response_port_type = netsim::port<pancy::response>;

    client_model(size_t nreplicas, network_type& net);
    virtual ~client_model() = default;
    client_model(const client_model&) = delete;
    client_model(client_model&&) = delete;
    client_model& operator=(const client_model&) = delete;
    client_model& operator=(client_model&&) = delete;

    // - Source of randomness
    random_source& randomness() { return randomness_; }

    // - Configured number of replicas
    size_t nreplicas() const noexcept { return nreplicas_; }
    // - Return a random replica index
    size_t random_replica() { return randomness_.uniform(size_t(0), nreplicas_ - 1); }

    // - Maximum number of simulated clients; must be a power of 2
    size_t max_clients() const noexcept { return 4096; }
    // - Current number of registered virtual clients
    size_t nclients() const noexcept { return clients_.size(); }
    // - Set number of clients
    void set_nclients(size_t n);
    // - Client mask: these bits of a request serial number equal client ID;
    //   a model supports at most `client_mask() + 1` simulated clients
    uint64_t client_mask() const noexcept { return max_clients() - 1; }
    // - Serial step: Each subsequent message from a simulated client should
    //   advance `serial` by this amount
    size_t serial_step() const noexcept { return 4096; }

    // - Send a request from client `serial & client_mask()` to replica
    //   `replicaid`
    template <pancy::request_type Req, typename... Args>
    cotamer::task<> send_request(
        size_t replicaid, uint64_t serial, Args... args
    );
    // - Receive a response, potentially by redirecting `replicaid`
    template <pancy::response_type Resp>
    cotamer::task<std::optional<Resp>> receive_response(
        size_t& replicaid, uint64_t serial
    );


    // - Start the model
    virtual void start() = 0;
    // - Stop the model
    virtual void stop();

    // - Test whether `db` could have been created by clients of this model.
    //   Returns `std::nullopt` on success, and a bad key on failure.
    virtual std::optional<std::string> check(const pancy::pancydb& db) = 0;

private:
    random_source& randomness_;
    network_type& net_;
    size_t nreplicas_;
    struct clientinfo {
        std::vector<channel_type*> channels;
        response_port_type* response_port;
    };
    std::vector<clientinfo> clients_;
};


template <pancy::request_type Req, typename... Args>
inline cotamer::task<> client_model::send_request(
    size_t replicaid, uint64_t serial, Args... args
) {
    size_t cid = serial & client_mask();
    return clients_[cid].channels[replicaid]->send(Req{{serial}, std::forward<Args>(args)...});
}

template <pancy::response_type Resp>
inline cotamer::task<std::optional<Resp>> client_model::receive_response(
    size_t& replicaid, uint64_t serial
) {
    size_t cid = serial & client_mask();
    while (true) {
        auto msg = co_await clients_[cid].response_port->receive();
        auto in_serial = pancy::message_serial(msg);
        if (in_serial != serial) {
            // stale response from earlier request; drop and keep waiting
            continue;
        }
        if (auto rp = std::get_if<pancy::redirection_response>(&msg)) {
            replicaid = rp->redirection;
            co_return std::nullopt;
        }
        if (auto mp = std::get_if<Resp>(&msg)) {
            co_return std::move(*mp);
        }
        // unexpected response type for this serial
        co_return std::nullopt;
    }
}
