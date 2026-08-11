#ifndef SPONGE_LIBSPONGE_ROUTER_HH
#define SPONGE_LIBSPONGE_ROUTER_HH

#include "icmp_message.hh"
#include "network_interface.hh"
#include "address.hh"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <queue>
#include <vector>

//! \brief A wrapper for NetworkInterface that makes the host-side
//! interface asynchronous: instead of returning received datagrams
//! immediately (from the `recv_frame` method), it stores them for
//! later retrieval. Otherwise, behaves identically to the underlying
//! implementation of NetworkInterface.
class AsyncNetworkInterface : public NetworkInterface {
    std::queue<InternetDatagram> _datagrams_out{};

  public:
    using NetworkInterface::NetworkInterface;

    //! Construct from a NetworkInterface
    AsyncNetworkInterface(NetworkInterface &&interface) : NetworkInterface(interface) {}

    //! \brief Receives and Ethernet frame and responds appropriately.

    //! - If type is IPv4, pushes to the `datagrams_out` queue for later retrieval by the owner.
    //! - If type is ARP request, learn a mapping from the "sender" fields, and send an ARP reply.
    //! - If type is ARP reply, learn a mapping from the "target" fields.
    //!
    //! \param[in] frame the incoming Ethernet frame
    void recv_frame(const EthernetFrame &frame) {
        auto optional_dgram = NetworkInterface::recv_frame(frame);
        if (optional_dgram.has_value()) {
            _datagrams_out.push(std::move(optional_dgram.value()));
        }
    };

    //! Access queue of Internet datagrams that have been received
    std::queue<InternetDatagram> &datagrams_out() { return _datagrams_out; }
};

//! \brief A single routing table entry (forwarding rule).
struct RouteEntry {
    uint32_t route_prefix{};
    uint8_t prefix_length{};
    std::optional<Address> next_hop{};
    size_t interface_num{};

    RouteEntry() = default;
    RouteEntry(const uint32_t route_prefix_,
               const uint8_t prefix_length_,
               const std::optional<Address> &next_hop_,
               const size_t interface_num_)
        : route_prefix(route_prefix_)
        , prefix_length(prefix_length_)
        , next_hop(next_hop_)
        , interface_num(interface_num_) {}
};

//! \brief A binary Trie (radix tree) that stores IPv4 routes and supports
//! longest-prefix-match lookup in O(prefix_length) = O(32) time.
//!
//! Each node has up to two children (bit 0 / bit 1, indexed from the most
//! significant bit of the IPv4 address). A route is stored at the node
//! reached by walking down the tree along the bits of its prefix. Lookup
//! walks the tree following the destination IP's bits and remembers the
//! last node that carries a route -- that is the longest matching prefix.
class RoutingTrie {
  private:
    //! A node in the binary trie.
    struct TrieNode {
        std::array<std::unique_ptr<TrieNode>, 2> children{};
        //! Route stored at this node (empty if this node is only an internal node).
        std::optional<RouteEntry> route{};
    };

    std::unique_ptr<TrieNode> _root{std::make_unique<TrieNode>()};

    //! Return the node reached by walking down `prefix`/`prefix_length` bits,
    //! creating intermediate nodes as needed.
    TrieNode &_navigate(const uint32_t route_prefix, const uint8_t prefix_length) {
        TrieNode *node = _root.get();
        for (uint8_t bit = 0; bit < prefix_length; ++bit) {
            // Extract bit `bit` (0 = MSB) of route_prefix.
            const uint8_t dir = (route_prefix >> (31 - bit)) & 1;
            if (!node->children[dir]) {
                node->children[dir] = std::make_unique<TrieNode>();
            }
            node = node->children[dir].get();
        }
        return *node;
    }

  public:
    //! Insert (or overwrite) a route entry into the trie.
    void insert(const RouteEntry &entry) {
        _navigate(entry.route_prefix, entry.prefix_length).route = entry;
    }

    //! Longest-prefix-match lookup. Returns the longest matching RouteEntry,
    //! or nullopt if no route matches.
    std::optional<RouteEntry> longest_prefix_match(const uint32_t dst) const {
        const TrieNode *node = _root.get();
        std::optional<RouteEntry> best_match = node->route;  // default route (prefix_length == 0)
        for (uint8_t bit = 0; bit < 32 && node; ++bit) {
            const uint8_t dir = (dst >> (31 - bit)) & 1;
            node = node->children[dir].get();
            if (!node) {
                break;
            }
            if (node->route.has_value()) {
                best_match = node->route;  // this match is longer than the previous
            }
        }
        return best_match;
    }
};

//! \brief A router that has multiple network interfaces and
//! performs longest-prefix-match routing between them.
class Router {
    //! The router's collection of network interfaces
    std::vector<AsyncNetworkInterface> _interfaces{};

    //! 用二进制 Trie 组织的路由表，支持 O(32) 最长前缀匹配
    RoutingTrie _route_table{};

    //! 是否启用 ICMP 错误报告（Destination Unreachable / Time Exceeded）。
    //! 默认为 false，确保现有 network_simulator 测试不受影响。
    //! 设为 true 后，路由器遇到不可达目的地或 TTL 过期时会发送 ICMP 错误消息。
    bool _icmp_enabled{false};

    //! Send a single datagram from the appropriate outbound interface to the next hop,
    //! as specified by the route with the longest prefix_length that matches the
    //! datagram's destination address.
    void route_one_datagram(InternetDatagram &dgram);

    //! 构造并发送一条 ICMP 消息。
    //! \param[in] type ICMP 类型（如 TYPE_ECHO_REPLY, TYPE_DESTINATION_UNREACHABLE, TYPE_TIME_EXCEEDED）
    //! \param[in] code ICMP 代码
    //! \param[in] dgram_original 触发该 ICMP 的原始 IP 数据报（用于错误消息时回填 payload 和地址）
    //! \param[in] interface_num 从哪个接口发出（用于确定源 IP）
    //! \param[in] next_hop 下一跳地址（通常是原始数据报的源 IP）
    void send_icmp(const uint8_t type, const uint8_t code,
                   const InternetDatagram &dgram_original,
                   const size_t interface_num,
                   const Address &next_hop);

  public:
    //! Add an interface to the router
    //! \param[in] interface an already-constructed network interface
    //! \returns The index of the interface after it has been added to the router
    size_t add_interface(AsyncNetworkInterface &&interface) {
        _interfaces.push_back(std::move(interface));
        return _interfaces.size() - 1;
    }

    //! Access an interface by index
    AsyncNetworkInterface &interface(const size_t N) { return _interfaces.at(N); }

    //! Add a route (a forwarding rule)
    void add_route(const uint32_t route_prefix,
                   const uint8_t prefix_length,
                   const std::optional<Address> next_hop,
                   const size_t interface_num);

    //! \brief Enable or disable ICMP error reporting (Destination Unreachable / Time Exceeded).
    //! Echo Reply is always enabled when a datagram is addressed to one of the router's
    //! own interfaces. Default is off to preserve original CS144 behavior.
    void set_icmp_enabled(const bool enabled) { _icmp_enabled = enabled; }

    //! Route packets between the interfaces
    void route();
};

#endif  // SPONGE_LIBSPONGE_ROUTER_HH
