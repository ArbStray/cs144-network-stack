#include "arp_message.hh"
#include "lua_config.hh"
#include "router.hh"
#include "util.hh"

#include <iostream>
#include <list>
#include <unordered_map>
#include <vector>

using namespace std;

auto rd = get_random_generator();

EthernetAddress random_host_ethernet_address() {
    EthernetAddress addr;
    for (auto &byte : addr) {
        byte = rd();  // use a random local Ethernet address
    }
    addr.at(0) |= 0x02;  // "10" in last two binary digits marks a private Ethernet address
    addr.at(0) &= 0xfe;

    return addr;
}

EthernetAddress random_router_ethernet_address() {
    EthernetAddress addr;
    for (auto &byte : addr) {
        byte = rd();  // use a random local Ethernet address
    }
    addr.at(0) = 0x02;  // "10" in last two binary digits marks a private Ethernet address
    addr.at(1) = 0;
    addr.at(2) = 0;

    return addr;
}

uint32_t ip(const string &str) { return Address{str}.ipv4_numeric(); }

template <typename T>
void clear(T &queue1, T &queue2) {
    while (not queue1.empty()) {
        queue1.pop();
        queue2.pop();
    }
}

string summary(const EthernetFrame &frame) {
    string ret;
    ret += frame.header().to_string();
    switch (frame.header().type) {
        case EthernetHeader::TYPE_IPv4: {
            InternetDatagram dgram;
            if (dgram.parse(frame.payload()) == ParseResult::NoError) {
                ret += " " + dgram.header().summary();
                ret += " payload=\"" + string(dgram.payload().concatenate()) + "\"";
            } else {
                ret += " (bad IPv4)";
            }
        } break;
        case EthernetHeader::TYPE_ARP: {
            ARPMessage arp;
            if (arp.parse(frame.payload()) == ParseResult::NoError) {
                ret += " " + arp.to_string();
            } else {
                ret += " (bad ARP)";
            }
        }
        default:
            break;
    }
    return ret;
}

class Host {
    string _name;
    Address _my_address;
    AsyncNetworkInterface _interface;
    Address _next_hop;

    std::list<InternetDatagram> _expecting_to_receive{};

    bool expecting(const InternetDatagram &expected) const {
        for (const auto &x : _expecting_to_receive) {
            if (x.serialize().concatenate() == expected.serialize().concatenate()) {
                return true;
            }
        }
        return false;
    }

    void remove_expectation(const InternetDatagram &expected) {
        for (auto it = _expecting_to_receive.begin(); it != _expecting_to_receive.end(); ++it) {
            if (it->serialize().concatenate() == expected.serialize().concatenate()) {
                _expecting_to_receive.erase(it);
                return;
            }
        }
    }

  public:
    Host(const string &name, const Address &my_address, const Address &next_hop)
        : _name(name)
        , _my_address(my_address)
        , _interface(random_host_ethernet_address(), _my_address)
        , _next_hop(next_hop) {}

    InternetDatagram send_to(const Address &destination, const uint8_t ttl = 64) {
        InternetDatagram dgram;
        dgram.header().src = _my_address.ipv4_numeric();
        dgram.header().dst = destination.ipv4_numeric();
        dgram.payload() = "random payload: {" + to_string(rd()) + "}";
        dgram.header().len = dgram.header().hlen * 4 + dgram.payload().size();
        dgram.header().ttl = ttl;

        _interface.send_datagram(dgram, _next_hop);

        cerr << "Host " << _name << " trying to send datagram (with next hop = " << _next_hop.ip()
             << "): " << dgram.header().summary() << " payload=\"" << dgram.payload().concatenate() << "\"\n";

        return dgram;
    }

    const Address &address() { return _my_address; }

    AsyncNetworkInterface &interface() { return _interface; }

    void expect(const InternetDatagram &expected) { _expecting_to_receive.push_back(expected); }

    const string &name() { return _name; }

    void check() {
        while (not _interface.datagrams_out().empty()) {
            const auto &dgram_received = _interface.datagrams_out().front();
            if (not expecting(dgram_received)) {
                throw runtime_error("Host " + _name +
                                    " received unexpected Internet datagram: " + dgram_received.header().summary() +
                                    " payload=\"" + dgram_received.payload().concatenate() + "\"");
            }
            remove_expectation(dgram_received);
            _interface.datagrams_out().pop();
        }

        if (not _expecting_to_receive.empty()) {
            auto &expected = _expecting_to_receive.front();
            throw runtime_error("Host " + _name + " did NOT receive an expected Internet datagram: " +
                                expected.header().summary() + " payload=\"" + expected.payload().concatenate() + "\"");
        }
    }
};

class Network {
  private:
    Router _router{};

    // For the hardcoded constructor: named interface indices.
    size_t default_id{0}, eth0_id{0}, eth1_id{0}, eth2_id{0}, uun3_id{0}, hs4_id{0}, mit5_id{0};

    // For the Lua-driven constructor: interface name → index.
    std::unordered_map<std::string, size_t> _interface_ids{};

    // Physical connections. Each entry is a pair of (router_interface_name, host_name).
    // The Lua constructor populates this so simulate_physical_connections() works
    // without knowing the topology ahead of time.
    struct Connection {
        std::string iface_name;
        std::string host_name;
    };
    std::vector<Connection> _connections{};

    std::unordered_map<string, Host> _hosts{};

    void exchange_frames(const string &x_name,
                         AsyncNetworkInterface &x,
                         const string &y_name,
                         AsyncNetworkInterface &y) {
        auto x_frames = x.frames_out(), y_frames = y.frames_out();

        deliver(x_name, x_frames, y_name, y);
        deliver(y_name, y_frames, x_name, x);

        clear(x_frames, x.frames_out());
        clear(y_frames, y.frames_out());
    }

    void exchange_frames(const string &x_name,
                         AsyncNetworkInterface &x,
                         const string &y_name,
                         AsyncNetworkInterface &y,
                         const string &z_name,
                         AsyncNetworkInterface &z) {
        auto x_frames = x.frames_out(), y_frames = y.frames_out(), z_frames = z.frames_out();

        deliver(x_name, x_frames, y_name, y);
        deliver(x_name, x_frames, z_name, z);

        deliver(y_name, y_frames, x_name, x);
        deliver(y_name, y_frames, z_name, z);

        deliver(z_name, z_frames, x_name, x);
        deliver(z_name, z_frames, y_name, y);

        clear(x_frames, x.frames_out());
        clear(y_frames, y.frames_out());
        clear(z_frames, z.frames_out());
    }

    void deliver(const string &src_name,
                 const queue<EthernetFrame> &src,
                 const string &dst_name,
                 AsyncNetworkInterface &dst) {
        queue<EthernetFrame> to_send = src;
        while (not to_send.empty()) {
            to_send.front().payload() = to_send.front().payload().concatenate();
            cerr << "Transferring frame from " << src_name << " to " << dst_name << ": " << summary(to_send.front())
                 << "\n";
            dst.recv_frame(move(to_send.front()));
            to_send.pop();
        }
    }

  public:
    Network()
        : default_id(_router.add_interface({random_router_ethernet_address(), {"171.67.76.46"}}))
        , eth0_id(_router.add_interface({random_router_ethernet_address(), {"10.0.0.1"}}))
        , eth1_id(_router.add_interface({random_router_ethernet_address(), {"172.16.0.1"}}))
        , eth2_id(_router.add_interface({random_router_ethernet_address(), {"192.168.0.1"}}))
        , uun3_id(_router.add_interface({random_router_ethernet_address(), {"198.178.229.1"}}))
        , hs4_id(_router.add_interface({random_router_ethernet_address(), {"143.195.0.2"}}))
        , mit5_id(_router.add_interface({random_router_ethernet_address(), {"128.30.76.255"}})) {
        _hosts.insert({"applesauce", {"applesauce", {"10.0.0.2"}, {"10.0.0.1"}}});
        _hosts.insert({"default_router", {"default_router", {"171.67.76.1"}, {"0"}}});
        ;
        _hosts.insert({"cherrypie", {"cherrypie", {"192.168.0.2"}, {"192.168.0.1"}}});
        _hosts.insert({"hs_router", {"hs_router", {"143.195.0.1"}, {"0"}}});
        _hosts.insert({"dm42", {"dm42", {"198.178.229.42"}, {"198.178.229.1"}}});
        _hosts.insert({"dm43", {"dm43", {"198.178.229.43"}, {"198.178.229.1"}}});

        _router.add_route(ip("0.0.0.0"), 0, host("default_router").address(), default_id);
        _router.add_route(ip("10.0.0.0"), 8, {}, eth0_id);
        _router.add_route(ip("172.16.0.0"), 16, {}, eth1_id);
        _router.add_route(ip("192.168.0.0"), 24, {}, eth2_id);
        _router.add_route(ip("198.178.229.0"), 24, {}, uun3_id);
        _router.add_route(ip("143.195.0.0"), 17, host("hs_router").address(), hs4_id);
        _router.add_route(ip("143.195.128.0"), 18, host("hs_router").address(), hs4_id);
        _router.add_route(ip("143.195.192.0"), 19, host("hs_router").address(), hs4_id);
        _router.add_route(ip("128.30.76.255"), 16, Address{"128.30.0.1"}, mit5_id);
    }

    void simulate_physical_connections() {
        if (_connections.empty()) {
            // Hardcoded topology: use the original per-interface exchange logic.
            exchange_frames(
                "router.default", _router.interface(default_id), "default_router", host("default_router").interface());
            exchange_frames("router.eth0", _router.interface(eth0_id), "applesauce", host("applesauce").interface());
            exchange_frames("router.eth2", _router.interface(eth2_id), "cherrypie", host("cherrypie").interface());
            exchange_frames("router.hs4", _router.interface(hs4_id), "hs_router", host("hs_router").interface());
            exchange_frames("router.uun3",
                            _router.interface(uun3_id),
                            "dm42",
                            host("dm42").interface(),
                            "dm43",
                            host("dm43").interface());
        } else {
            // Lua-driven topology: iterate the connection list.
            for (auto &conn : _connections) {
                const size_t iface_id = _interface_ids.at(conn.iface_name);
                const string label = "router." + conn.iface_name;
                exchange_frames(label, _router.interface(iface_id),
                                conn.host_name, host(conn.host_name).interface());
            }
        }
    }

    void simulate() {
        for (unsigned int i = 0; i < 256; i++) {
            _router.route();
            simulate_physical_connections();
        }

        for (auto &host : _hosts) {
            host.second.check();
        }
    }

    Host &host(const string &name) {
        auto it = _hosts.find(name);
        if (it == _hosts.end()) {
            throw runtime_error("unknown host: " + name);
        }
        if (it->second.name() != name) {
            throw runtime_error("invalid host: " + name);
        }
        return it->second;
    }

    //! Construct a Network from a Lua configuration script.
    explicit Network(LuaConfig &lua) {
        auto *L = lua.state();
        // The script has already been run; the result table is on top of the stack.

        // ---- 1. Interfaces ----
        if (LuaConfig::push_field(L, "interfaces")) {
            LuaConfig::for_each(L, [&](int) {
                auto name = LuaConfig::get_string(L, "name");
                auto ip_s = LuaConfig::get_string(L, "ip");
                if (name && ip_s) {
                    const size_t id = _router.add_interface({random_router_ethernet_address(), {ip_s.value(), 0}});
                    _interface_ids[name.value()] = id;
                    cerr << "  Lua: interface " << name.value() << " (" << ip_s.value() << ") = id " << id << "\n";
                }
            });
            lua_pop(L, 1);  // pop interfaces table
        }

        // ---- 2. Hosts ----
        if (LuaConfig::push_field(L, "hosts")) {
            LuaConfig::for_each_pair(L, [&](const char *host_name) {
                auto ip_s = LuaConfig::get_string(L, "ip");
                auto hop_s = LuaConfig::get_string(L, "next_hop");
                if (ip_s && hop_s) {
                    _hosts.emplace(host_name, Host{host_name, {ip_s.value(), 0}, {hop_s.value(), 0}});
                    cerr << "  Lua: host " << host_name << " ip=" << ip_s.value() << "\n";
                }
            });
            lua_pop(L, 1);  // pop hosts table
        }

        // ---- 3. Routes ----
        if (LuaConfig::push_field(L, "routes")) {
            LuaConfig::for_each(L, [&](int) {
                auto prefix_s = LuaConfig::get_string(L, "prefix");
                auto len_i = LuaConfig::get_int(L, "len");
                auto hop_s = LuaConfig::get_string(L, "next_hop");
                auto iface_s = LuaConfig::get_string(L, "interface");
                if (prefix_s && len_i && iface_s) {
                    const uint32_t prefix = Address{prefix_s.value(), 0}.ipv4_numeric();
                    const uint8_t len = static_cast<uint8_t>(len_i.value());
                    const size_t iface_id = _interface_ids.at(iface_s.value());
                    std::optional<Address> next_hop;
                    if (hop_s && !hop_s.value().empty() && host_exists(hop_s.value())) {
                        next_hop = host(hop_s.value()).address();
                    } else if (hop_s) {
                        next_hop = Address{hop_s.value(), 0};
                    }
                    _router.add_route(prefix, len, next_hop, iface_id);
                }
            });
            lua_pop(L, 1);  // pop routes table
        }

        // ---- 4. Connections ----
        if (LuaConfig::push_field(L, "connections")) {
            LuaConfig::for_each(L, [&](int) {
                auto iface_s = LuaConfig::get_string(L, "router");
                auto host_s = LuaConfig::get_string(L, "host");
                if (iface_s && host_s) {
                    _connections.push_back({iface_s.value(), host_s.value()});
                    cerr << "  Lua: connection " << iface_s.value() << " <-> " << host_s.value() << "\n";
                } else if (iface_s) {
                    // Check for multi-host list: key = "hosts" (array)
                    bool has_multi = LuaConfig::push_field(L, "hosts");
                    if (has_multi) {
                        LuaConfig::for_each(L, [&](int) {
                            auto h = LuaConfig::peek_string(L, -1);
                            if (!h.empty()) {
                                _connections.push_back({iface_s.value(), h});
                            }
                        });
                        lua_pop(L, 1);  // pop hosts array
                    }
                }
            });
            lua_pop(L, 1);  // pop connections table
        }
    }

    //! Check whether a host name exists (used during route construction).
    bool host_exists(const string &name) const {
        return _hosts.find(name) != _hosts.end();
    }
};

void network_simulator() {
    const string green = "\033[32;1m", normal = "\033[m";

    cerr << green << "Constructing network." << normal << "\n";

    Network network;

    cout << green << "\n\nTesting traffic between two ordinary hosts (applesauce to cherrypie)..." << normal << "\n\n";
    {
        auto dgram_sent = network.host("applesauce").send_to(network.host("cherrypie").address());
        dgram_sent.header().ttl--;
        network.host("cherrypie").expect(dgram_sent);
        network.simulate();
    }

    cout << green << "\n\nTesting traffic between two ordinary hosts (cherrypie to applesauce)..." << normal << "\n\n";
    {
        auto dgram_sent = network.host("cherrypie").send_to(network.host("applesauce").address());
        dgram_sent.header().ttl--;
        network.host("applesauce").expect(dgram_sent);
        network.simulate();
    }

    cout << green << "\n\nSuccess! Testing applesauce sending to the Internet." << normal << "\n\n";
    {
        auto dgram_sent = network.host("applesauce").send_to({"1.2.3.4"});
        dgram_sent.header().ttl--;
        network.host("default_router").expect(dgram_sent);
        network.simulate();
    }

    cout << green << "\n\nSuccess! Testing sending to the HS network and Internet." << normal << "\n\n";
    {
        auto dgram_sent = network.host("applesauce").send_to({"143.195.131.17"});
        dgram_sent.header().ttl--;
        network.host("hs_router").expect(dgram_sent);
        network.simulate();

        dgram_sent = network.host("cherrypie").send_to({"143.195.193.52"});
        dgram_sent.header().ttl--;
        network.host("hs_router").expect(dgram_sent);
        network.simulate();

        dgram_sent = network.host("cherrypie").send_to({"143.195.223.255"});
        dgram_sent.header().ttl--;
        network.host("hs_router").expect(dgram_sent);
        network.simulate();

        dgram_sent = network.host("cherrypie").send_to({"143.195.224.0"});
        dgram_sent.header().ttl--;
        network.host("default_router").expect(dgram_sent);
        network.simulate();
    }

    cout << green << "\n\nSuccess! Testing two hosts on the same network (dm42 to dm43)..." << normal << "\n\n";
    {
        auto dgram_sent = network.host("dm42").send_to(network.host("dm43").address());
        dgram_sent.header().ttl--;
        network.host("dm43").expect(dgram_sent);
        network.simulate();
    }

    cout << green << "\n\nSuccess! Testing TTL expiration..." << normal << "\n\n";
    {
        auto dgram_sent = network.host("applesauce").send_to({"1.2.3.4"}, 1);
        network.simulate();

        dgram_sent = network.host("applesauce").send_to({"1.2.3.4"}, 0);
        network.simulate();
    }

    cout << "\n\n\033[32;1mCongratulations! All datagrams were routed successfully.\033[m\n";
}

//! Run the network simulator driven by a Lua topology file.
int network_simulator_lua(const string &filename) {
    const string green = "\033[32;1m", normal = "\033[m";

    cerr << green << "Loading network from " << filename << "..." << normal << "\n";
    LuaConfig lua{filename};

    cerr << green << "Constructing network from Lua config." << normal << "\n";
    Network network{lua};

    // Re-read the tests table from the Lua config and run each test.
    auto *L = lua.state();
    if (!LuaConfig::push_field(L, "tests")) {
        cerr << "\033[33;1mWarning: no tests defined in Lua config.\033[m\n";
        return EXIT_SUCCESS;
    }

    LuaConfig::for_each(L, [&](int) {
        auto from_s = LuaConfig::get_string(L, "from");
        auto to_s = LuaConfig::get_string(L, "to");
        auto desc_s = LuaConfig::get_string(L, "desc");
        auto ttl_i = LuaConfig::get_int(L, "ttl");

        if (!from_s || !to_s) {
            return;  // skip malformed test entry
        }

        const string desc = desc_s.value_or(from_s.value() + " -> " + to_s.value());
        cout << green << "\n\nTesting " << desc << " (" << from_s.value() << " -> " << to_s.value()
             << ")..." << normal << "\n\n";

        const uint8_t ttl = ttl_i.has_value() ? static_cast<uint8_t>(ttl_i.value()) : 64;
        auto dgram_sent = network.host(from_s.value()).send_to({to_s.value()}, ttl);

        if (ttl_i.has_value() && ttl_i.value() <= 1) {
            // TTL expiration test: don't expect delivery, just simulate.
            network.simulate();
        } else {
            // Normal test: expect delivery to the destination.
            dgram_sent.header().ttl--;
            // If `to` is a host name, expect it there; otherwise it's an
            // external IP routed via the default gateway.
            if (network.host_exists(to_s.value())) {
                network.host(to_s.value()).expect(dgram_sent);
            }
            network.simulate();
        }
    });
    lua_pop(L, 1);  // pop tests table

    cout << "\n\n\033[32;1mCongratulations! All Lua-defined tests passed.\033[m\n";
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    try {
        if (argc > 1) {
            return network_simulator_lua(argv[1]);
        }
        network_simulator();
    } catch (const exception &e) {
        cerr << "\n\n\n";
        cerr << "\033[31;1mError: " << e.what() << "\033[m\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
