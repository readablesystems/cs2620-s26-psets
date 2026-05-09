#pragma once
#include "cotamer/cotamer.hh"
#include "random_source.hh"
#include <print>
#include <string_view>

// netsim.hh
//    Network simulator.
//
//    The `network` type is a registry for ports (receiving endpoints) and
//    channels (point-to-point connections). `network<Req, Resp>` supports
//    client-server connections with requests of type `Req` and responses of
//    type `Resp`; `network<T>` supports unidirectional connections with
//    messages of type `T`.
//
//    The `port` type receives messages. `port<Req, Resp>` is a server port; it
//    receives requests of type `Req`, and provides a pointer to a response
//    channel delivering responses of type `Resp`. `port<T>` receives
//    responses or unidirectional messages.
//
//    The `channel<T>` type delivers messages of type `T` to a receiving port.


namespace netsim {
namespace cot = cotamer;
using namespace std::chrono_literals;
class channel_base;
template <typename Req, typename Resp = void> class network;
template <typename Req, typename Resp = void> class port;
template <typename T> class channel;
template <typename T> struct message_traits;
extern const std::string disconnected_id;

template <typename T, typename U>
concept smart_pointer_to =
    std::same_as<T, std::unique_ptr<U>> || std::same_as<T, std::shared_ptr<U>>;


// channel_base
//    Common base for every `channel<T>` type

class channel_base {
public:
    using address_type = std::string_view;
    using address_storage_type = std::string;

    address_type source_address() const noexcept { return source_address_; }

protected:
    explicit channel_base(address_type source) : source_address_(source) {}
    ~channel_base() = default;
    channel_base(const channel_base&) = delete;
    channel_base(channel_base&&) = delete;
    channel_base& operator=(const channel_base&) = delete;
    channel_base& operator=(channel_base&&) = delete;

    address_storage_type source_address_;
};


// receiver<T>
//    The receive side of a port. Owns a message queue, the event used to
//    wake blocked receivers, and the receive delay.
//
//    `port<...>` types inherit publicly from `receiver`.

template <typename T>
class receiver {
public:
    using address_type = std::string_view;
    using address_storage_type = std::string;
    using message_type = T;
    using message_from_type = std::pair<message_type, address_type>;
    using message_traits_type = message_traits<message_type>;

    receiver(address_type address)
        : address_(std::move(address)) {
    }
    ~receiver() {
        // wake up any waiters so the driver cleanup code frees their memory
        receivable_.trigger();
    }
    receiver(const receiver&) = delete;
    receiver(receiver&&) = delete;
    receiver& operator=(const receiver&) = delete;
    receiver& operator=(receiver&&) = delete;

    address_type address() const noexcept { return address_; }

    bool verbose() const noexcept { return verbose_; }
    void set_verbose(bool v) noexcept { verbose_ = v; }

    // enqueue an incoming message and wake any blocked receiver
    void deliver(channel_base* from, message_type m) {
        messageq_.emplace_back(std::move(m), from->source_address());
        receivable_.trigger();
    }

    // suspend until a message is available, then dequeue and return it
    cot::task<message_from_type> receive_from() {
        do {
            while (messageq_.empty()) {
                cot::driver_guard guard;
                // Register an event that senders will trigger on delivery.
                // Need a new one every time because events are one-shot.
                co_await receivable_.arm();
                // Suspend for receive delay
                co_await cot::after(receive_delay_);
            }

            // Suspend to ensure our return value will be used
            co_await cot::resolve{};
        } while (messageq_.empty());

        auto m = std::move(messageq_.front());
        messageq_.pop_front();
        if (this->verbose()) {
            std::print("{}: {} ← {} {}\n", cot::now(), address(), m.second,
                       message_traits_type::print_transform(m.first));
        }
        co_return std::move(m);
    }

    cot::task<message_type> receive() {
        auto [msg, source_addr] = co_await receive_from();
        co_return std::move(msg);
    }

private:
    std::deque<message_from_type> messageq_;
    cot::event receivable_;
    cot::duration receive_delay_ = 1ms;

    address_storage_type address_;
    bool verbose_ = false;
};


// channel<T>
//    One-way send-only link from a source to a `receiver<T>`. Used both
//    for plain inter-server messaging and as the *reply channel* for a
//    server `port<Req, Resp>` (with `T == Resp`).

template <typename T>
class channel : public channel_base {
public:
    using message_type = T;
    using message_traits_type = message_traits<T>;

    inline channel(random_source&, address_type source,
                   receiver<T>& destination,
                   address_type destination_addr);

    address_type destination_address() const noexcept { return destination_address_; }

    bool verbose() const noexcept { return verbose_; }
    void set_verbose(bool v) noexcept { verbose_ = v; }

    double loss() const noexcept { return loss_; }
    void set_loss(double l) noexcept { loss_ = l; }

    // send a message on this channel
    cot::task<> send(T m);

private:
    receiver<T>* destination_ = nullptr;
    cot::duration link_delay_ = 5ms;
    cot::duration send_delay_ = 1ms;
    double loss_ = 0.0;

    random_source& randomness_;
    address_storage_type destination_address_;
    bool verbose_ = false;

    cot::task<> send_after(cot::duration, T);
};


// port<T, void>
//    One-way receive-only port. Used both for inter-server messaging
//    (in `network<T, void>`) and as a response port on the client side
//    of a bidirectional `network<Req', Resp>` where `Resp == Req`.

template <typename T>
class port<T, void> : public receiver<T> {
public:
    using message_type = T;
    using message_traits_type = message_traits<T>;
    using address_type = std::string_view;
    using address_storage_type = std::string;

    inline port(network<T>&, address_type);
    ~port() = default;
    port(const port&) = delete;
    port(port&&) = delete;
    port& operator=(const port&) = delete;
    port& operator=(port&&) = delete;

    // inherit `receive_from` and `receive` from `receiver<Req>`

    // obtain channel from `addr` to this port; may return nullptr
    channel<T>* find_channel_from(address_type addr);

private:
    friend class network<T, void>;

    network<T>& network_;
    std::map<address_storage_type, channel<T>> channels_;
};


// network<T, void>
//    Registry of one-way ports. Used for inter-server messaging.

template <typename T>
class network<T, void> {
public:
    using message_type = T;
    using message_traits_type = message_traits<T>;
    using port_type = ::netsim::port<T, void>;
    using address_type = std::string_view;
    using address_storage_type = std::string;

    inline network(random_source&);
    network(const network&) = delete;
    network(network&&) = delete;
    network& operator=(const network&) = delete;
    network& operator=(network&&) = delete;

    random_source& randomness() noexcept { return randomness_; }

    bool default_verbose() const noexcept { return default_verbose_; }
    void set_default_verbose(bool v) noexcept { default_verbose_ = v; }
    double default_loss() const noexcept { return default_loss_; }
    void set_default_loss(double l) noexcept { default_loss_ = l; }

    // register-or-find a port at `addr`
    inline port_type& port(address_type addr);
    // find an existing port; return nullptr if not registered
    inline port_type* find_port(address_type addr);
    // find-or-create channel from `source_addr` to `destination_addr`;
    // returns nullptr if there is no port at `source_addr`
    inline channel<T>* find_channel(address_type source_addr,
                                    address_type destination_addr);

private:
    random_source& randomness_;
    std::map<address_storage_type, port_type> ports_;
    bool default_verbose_ = false;
    double default_loss_ = 0.0;
};


// port<Req, Resp>  (Resp != void)
//    Server-side listening port. `receive_from()` yields each `Req`
//    paired with a one-way reply channel pointing at the sender's
//    response port. Reply channels are stable across multiple requests
//    from the same peer.

template <typename Req, typename Resp>
class port : public receiver<Req> {
public:
    using request_type = Req;
    using response_type = Resp;
    using request_traits_type = message_traits<Req>;
    using response_traits_type = message_traits<Resp>;
    using address_type = std::string_view;
    using address_storage_type = std::string;

    inline port(network<Req, Resp>&, address_type);
    ~port() = default;
    port(const port&) = delete;
    port(port&&) = delete;
    port& operator=(const port&) = delete;
    port& operator=(port&&) = delete;

    // receive a request paired with the reply channel for the sender
    cot::task<std::pair<request_type, channel<Resp>*>> receive_with_channel();

private:
    friend class network<Req, Resp>;

    // Channels owned by this server port, keyed by peer (client) address.
    // Each entry is a pair: first = the inbound `channel<Req>` from the
    // peer; second = the outbound `channel<Resp>` reply channel back to
    // the peer. Both are created together by `network::find_channel`.
    using channel_pair = std::pair<channel<Req>, channel<Resp>>;
    std::map<address_storage_type, channel_pair> channels_;

    network<Req, Resp>& network_;
};


// network<Req, Resp>  (Resp != void)
//    Registry of bidirectional endpoints. Hands out:
//      * `port(addr)`            — server `port<Req, Resp>`.
//      * `response_port(addr)`   — client-side `port<Resp>` that receives
//                                  responses.
//      * `find_channel(src, dst)`— client→server `channel<Req>` link.

template <typename Req, typename Resp>
class network {
public:
    using request_type = Req;
    using response_type = Resp;
    using request_traits_type = message_traits<Req>;
    using response_traits_type = message_traits<Resp>;
    using port_type = ::netsim::port<Req, Resp>;
    using response_port_type = ::netsim::port<Resp>;
    using address_type = std::string_view;
    using address_storage_type = std::string;

    inline network(random_source&);
    network(const network&) = delete;
    network(network&&) = delete;
    network& operator=(const network&) = delete;
    network& operator=(network&&) = delete;

    random_source& randomness() noexcept { return randomness_; }

    bool default_verbose() const noexcept { return default_verbose_; }
    void set_default_verbose(bool v) noexcept {
        default_verbose_ = v;
        response_network_.set_default_verbose(v);
    }
    double default_loss() const noexcept { return default_loss_; }
    void set_default_loss(double l) noexcept {
        default_loss_ = l;
        response_network_.set_default_loss(l);
    }

    // register-or-find a server port at `addr`
    inline port_type& port(address_type addr);
    // register-or-find a response (client) port at `addr`
    inline response_port_type& response_port(address_type addr);

    // find an existing port; return nullptr if not registered
    inline port_type* find_port(address_type addr);
    inline response_port_type* find_response_port(address_type addr);

    // find the client→server response channel from server `src` to client
    // `dst`; returns nullptr if either port does not exist
    inline channel<Req>* find_channel(address_type src, address_type dst);

    // find the server→client response channel from server `src` to client
    // `dst`; returns nullptr if either port does not exist
    inline channel<Resp>* find_response_channel(address_type src, address_type dst);

private:
    random_source& randomness_;
    std::map<address_storage_type, port_type> ports_;
    network<Resp, void> response_network_;   // owns response ports
    bool default_verbose_ = false;
    double default_loss_ = 0.0;
};


// =====================================================================
// channel<T> — implementation

template <typename T>
inline channel<T>::channel(random_source& r, address_type source,
                           receiver<T>& destination,
                           address_type destination_addr)
    : channel_base(source), destination_(&destination), randomness_(r),
      destination_address_(destination_addr) {
}

template <typename T>
cot::task<> channel<T>::send(T m) {
    if (destination_ && !randomness_.coin_flip(loss_)) {
        if (verbose_) {
            std::print("{}: {} → {} {}\n", cot::now(),
                       source_address(), destination_address(),
                       message_traits_type::print_transform(m));
        }
        // after `link_delay_`, place the message in the receiver’s queue
        send_after(link_delay_, std::move(m)).detach();
    }
    // sending a message takes time
    co_await cot::after(send_delay_);
}

template <typename T>
cot::task<> channel<T>::send_after(cot::duration delay, T m) {
    co_await cot::after(delay);
    if (!destination_) {
        co_return;
    }
    destination_->deliver(this, std::move(m));
}


template <typename T>
inline port<T, void>::port(network<T>& net, address_type addr)
    : receiver<T>(addr), network_(net) {
}

template <typename T>
channel<T>* port<T, void>::find_channel_from(address_type src) {
    auto it = channels_.find(address_storage_type(src));
    if (it != channels_.end()) {
        return &it->second;
    }
    auto p = network_.find_port(src);
    if (!p) {
        return nullptr;
    }
    auto [xit, inserted] = channels_.try_emplace(
        address_storage_type(src),
        network_.randomness(), p->address(), *this, this->address()
    );
    xit->second.set_verbose(network_.default_verbose());
    xit->second.set_loss(network_.default_loss());
    return &xit->second;
}


// =====================================================================
// network<T, void> — implementation

template <typename T>
inline network<T, void>::network(random_source& r) : randomness_(r) {
}

template <typename T>
inline auto network<T, void>::port(address_type addr) -> port_type& {
    auto [it, inserted] = ports_.try_emplace(address_storage_type(addr), *this, addr);
    it->second.set_verbose(default_verbose_);
    return it->second;
}

template <typename T>
inline auto network<T, void>::find_port(address_type addr) -> port_type* {
    auto it = ports_.find(address_storage_type(addr));
    return it == ports_.end() ? nullptr : &it->second;
}

template <typename T>
inline channel<T>* network<T, void>::find_channel(address_type src, address_type dst) {
    auto it = ports_.find(address_storage_type(dst));
    return it == ports_.end() ? nullptr : it->second.find_channel_from(src);
}


// =====================================================================
// port<Req, Resp> — implementation

template <typename Req, typename Resp>
inline port<Req, Resp>::port(network<Req, Resp>& net, address_type addr)
    : receiver<Req>(addr), network_(net) {
}

template <typename Req, typename Resp>
auto port<Req, Resp>::receive_with_channel()
    -> cot::task<std::pair<Req, channel<Resp>*>> {
    auto [msg, source_addr] = co_await this->receive_from();
    auto it = channels_.find(address_storage_type(source_addr));
    co_return {std::move(msg), &it->second.second};
}


// =====================================================================
// network<Req, Resp> — implementation

template <typename Req, typename Resp>
inline network<Req, Resp>::network(random_source& r)
    : randomness_(r), response_network_(r) {
}

template <typename Req, typename Resp>
inline auto network<Req, Resp>::port(address_type addr) -> port_type& {
    auto [it, inserted] = ports_.try_emplace(address_storage_type(addr), *this, addr);
    it->second.set_verbose(default_verbose_);
    return it->second;
}

template <typename Req, typename Resp>
inline auto network<Req, Resp>::response_port(address_type addr) -> response_port_type& {
    return response_network_.port(addr);
}

template <typename Req, typename Resp>
inline auto network<Req, Resp>::find_port(address_type addr) -> port_type* {
    auto it = ports_.find(address_storage_type(addr));
    return it == ports_.end() ? nullptr : &it->second;
}

template <typename Req, typename Resp>
inline auto network<Req, Resp>::find_response_port(address_type addr) -> response_port_type* {
    return response_network_.find_port(addr);
}

template <typename Req, typename Resp>
inline channel<Resp>* network<Req, Resp>::find_response_channel(address_type src, address_type dst) {
    auto* server = find_port(src);
    if (!server) {
        return nullptr;
    }
    auto it = server->channels_.find(address_storage_type(dst));
    return it == server->channels_.end() ? nullptr : &it->second.second;
}

template <typename Req, typename Resp>
inline channel<Req>* network<Req, Resp>::find_channel(address_type src, address_type dst) {
    auto* dest = find_port(dst);
    if (!dest) {
        return nullptr;
    }
    auto& m = dest->channels_;
    auto it = m.find(address_storage_type(src));
    if (it != m.end()) {
        return &it->second.first;
    }
    // Need the source's response port so we can pre-create the matching
    // reply channel.
    auto* src_resp = find_response_port(src);
    if (!src_resp) {
        return nullptr;
    }
    auto [xit, inserted] = m.try_emplace(
        address_storage_type(src),
        std::piecewise_construct,
        std::forward_as_tuple(randomness_, src, *dest, dest->address()),
        std::forward_as_tuple(randomness_, dst, *src_resp, src)
    );
    auto& [req_chan, resp_chan] = xit->second;
    req_chan.set_verbose(default_verbose_);
    req_chan.set_loss(default_loss_);
    resp_chan.set_verbose(default_verbose_);
    resp_chan.set_loss(default_loss_);
    return &req_chan;
}


// message_traits<T>
//    This template lets us change the behavior of network functions based on
//    message type. We provide specializations that allow you to print
//    messages that are stored as pointers.

template <typename T>
struct message_traits {
    static decltype(auto) print_transform(const T& x) {
        if constexpr (std::formattable<T, char>) {
            return x;
        } else {
            return "<message>";
        }
    }
};

template <typename T>
struct message_traits<T*> {
    static inline const T& print_transform(T* x) {
        return message_traits<T>::print_transform(*x);
    }
};

template <typename T>
struct message_traits<std::unique_ptr<T>> {
    static inline const T& print_transform(const std::unique_ptr<T>& x) {
        return message_traits<T>::print_transform(*x);
    }
};

template <typename T>
struct message_traits<std::shared_ptr<T>> {
    static inline const T& print_transform(const std::shared_ptr<T>& x) {
        return message_traits<T>::print_transform(*x);
    }
};

}
