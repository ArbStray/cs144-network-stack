#include "router.hh"

#include <algorithm>
#include <iostream>

using namespace std;

// Dummy implementation of an IP router

// Given an incoming Internet datagram, the router decides
// (1) which interface to send it out on, and
// (2) what next hop address to send it to.

// For Lab 6, please replace with a real implementation that passes the
// automated checks run by `make check_lab6`.

// You will need to add private members to the class declaration in `router.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

//! \param[in] route_prefix The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
//! \param[in] prefix_length For this route to be applicable, how many high-order (most-significant) bits of the route_prefix will need to match the corresponding bits of the datagram's destination address?
//! \param[in] next_hop The IP address of the next hop. Will be empty if the network is directly attached to the router (in which case, the next hop address should be the datagram's final destination).
//! \param[in] interface_num The index of the interface to send the datagram out on.
void Router::add_route(const uint32_t route_prefix,
                       const uint8_t prefix_length,
                       const std::optional<Address> &next_hop,
                       const size_t interface_num) {
    cerr << "DEBUG: adding route " << Address::from_ipv4_numeric(route_prefix).ip() << "/" << int(prefix_length)
         << " => " << (next_hop.has_value() ? next_hop->ip() : "(direct)") << " on interface " << interface_num << "\n";

    // 将路由条目插入 Trie（O(prefix_length)）
    _route_table.insert({route_prefix, prefix_length, next_hop, interface_num});
}

//! \param[in] dgram The datagram to be routed
void Router::route_one_datagram(InternetDatagram &dgram) {
    const auto ip_dst = dgram.header().dst;

    // ==== 第一层：检查是否是发往路由器自身的 ICMP Echo Request ====
    // 遍历所有接口，检查目的 IP 是否匹配某个接口的 IP。
    // 如果匹配且 protocol = ICMP(1) type = 8(Echo Request)，回复 Echo Reply。
    if (dgram.header().proto == IPv4Header::PROTO_ICMP) {
        for (size_t i = 0; i < _interfaces.size(); ++i) {
            if (ip_dst == _interfaces.at(i).ip_address().ipv4_numeric()) {
                // 解析 ICMP payload（需要连续内存，用 concatenate）
                ICMPMessage icmp;
                if (icmp.parse(Buffer(dgram.payload().concatenate())) != ParseResult::NoError) {
                    return;  // 解析失败，丢弃
                }
                if (icmp.type == ICMPMessage::TYPE_ECHO_REQUEST) {
                    // 构造 Echo Reply
                    ICMPMessage reply;
                    reply.type = ICMPMessage::TYPE_ECHO_REPLY;
                    reply.code = 0;
                    reply.identifier = icmp.identifier;
                    reply.sequence_number = icmp.sequence_number;
                    reply.payload = std::move(icmp.payload);

                    const std::string reply_serialized = reply.serialize();
                    InternetDatagram reply_dgram;
                    reply_dgram.header().src = ip_dst;               // 路由器接口 IP
                    reply_dgram.header().dst = dgram.header().src;  // 原始发送方
                    reply_dgram.header().proto = IPv4Header::PROTO_ICMP;
                    reply_dgram.header().ttl = IPv4Header::DEFAULT_TTL;
                    // len 必须等于 IP 头 + payload，否则 IPv4Datagram::serialize 会抛异常
                    reply_dgram.header().len = IPv4Header::LENGTH + reply_serialized.size();
                    reply_dgram.payload() = BufferList(Buffer(std::string(reply_serialized)));

                    cerr << "  ICMP: replying Echo Request from "
                         << Address::from_ipv4_numeric(dgram.header().src).ip() << " to "
                         << Address::from_ipv4_numeric(ip_dst).ip() << "\n";
                    // 从匹配的接口发送回复，下一跳是原始发送方
                    _interfaces.at(i).send_datagram(reply_dgram,
                                                    Address::from_ipv4_numeric(dgram.header().src));
                }
                // 其他发往路由器自身的 ICMP（非 Echo Request），静默丢弃
                return;
            }
        }
    }

    // ==== 第二层：在路由表中进行最长前缀匹配 ====
    const auto best_match = _route_table.longest_prefix_match(ip_dst);

    if (!best_match.has_value()) {
        // 没有匹配的路由条目 → 发送 ICMP Destination Unreachable (code 0 = net unreachable)
        if (_icmp_enabled) {
            cerr << "  ICMP: Destination Unreachable for " << Address::from_ipv4_numeric(ip_dst).ip() << "\n";
            send_icmp(ICMPMessage::TYPE_DESTINATION_UNREACHABLE, 0, dgram,
                      0,  // 从接口 0 发出（通常是 default/uplink 接口）
                      Address::from_ipv4_numeric(dgram.header().src));
        }
        return;
    }

    // ==== 第三层：检查 TTL ====
    if (dgram.header().ttl <= 1) {
        // TTL 已过期 → 发送 ICMP Time Exceeded (code 0 = ttl exceeded in transit)
        if (_icmp_enabled) {
            cerr << "  ICMP: Time Exceeded for " << Address::from_ipv4_numeric(ip_dst).ip() << "\n";
            send_icmp(ICMPMessage::TYPE_TIME_EXCEEDED, 0, dgram,
                      best_match->interface_num,
                      Address::from_ipv4_numeric(dgram.header().src));
        }
        return;
    }

    // ==== 第四层：正常转发 ====
    --dgram.header().ttl;
    auto &next_interface = interface(best_match->interface_num);
    // 如果路由器直接连接到相关网络，则下一跳就是目的 IP 地址，否则为下一跳路由器的 IP 地址
    if (best_match->next_hop.has_value()) {
        next_interface.send_datagram(dgram, best_match->next_hop.value());
    } else {
        next_interface.send_datagram(dgram, Address::from_ipv4_numeric(ip_dst));
    }
}

void Router::send_icmp(const uint8_t type, const uint8_t code,
                       const InternetDatagram &dgram_original,
                       const size_t interface_num,
                       const Address &next_hop) {
    ICMPMessage icmp;
    icmp.type = type;
    icmp.code = code;
    // 错误消息的 payload 是原始 IP 数据报的头部 + 前 8 字节 payload（RFC 792）
    // 截取 IP 头（20 字节）+ 前 8 字节 = 28 字节
    const std::string original_serialized = dgram_original.serialize().concatenate();
    icmp.payload = original_serialized.substr(0, std::min(original_serialized.size(),
                                                           size_t(IPv4Header::LENGTH + 8)));

    InternetDatagram icmp_dgram;
    // 用发送接口的 IP 作为源地址；如果 interface_num 不可达则用 0（鲁棒处理）
    icmp_dgram.header().src = (_interfaces.size() > interface_num)
                                  ? _interfaces.at(interface_num).ip_address().ipv4_numeric()
                                  : 0;
    icmp_dgram.header().dst = dgram_original.header().src;  // 回复给原始发送方
    icmp_dgram.header().proto = IPv4Header::PROTO_ICMP;
    icmp_dgram.header().ttl = IPv4Header::DEFAULT_TTL;
    const std::string icmp_serialized = icmp.serialize();
    icmp_dgram.header().len = IPv4Header::LENGTH + icmp_serialized.size();
    icmp_dgram.payload() = BufferList(Buffer(std::string(icmp_serialized)));

    // 从指定接口发出 ICMP 消息
    if (_interfaces.size() > interface_num) {
        _interfaces.at(interface_num).send_datagram(icmp_dgram, next_hop);
    }
}

void Router::route() {
    // Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
    for (auto &interface : _interfaces) {
        auto &queue = interface.datagrams_out();
        while (not queue.empty()) {
            route_one_datagram(queue.front());
            queue.pop();
        }
    }
}
