#include "icmp_message.hh"

#include "util.hh"

#include <sstream>

using namespace std;

ParseResult ICMPMessage::parse(const Buffer buffer) {
    NetParser p{buffer};

    if (p.buffer().size() < ICMPMessage::MIN_LENGTH) {
        return ParseResult::PacketTooShort;
    }

    type = p.u8();
    code = p.u8();
    cksum = p.u16();

    // For Echo Request / Echo Reply, the next 4 bytes are identifier + sequence number.
    if (type == TYPE_ECHO_REQUEST || type == TYPE_ECHO_REPLY) {
        if (p.buffer().size() < ICMPMessage::ECHO_LENGTH - MIN_LENGTH) {
            return ParseResult::PacketTooShort;
        }
        identifier = p.u16();
        sequence_number = p.u16();
    } else {
        // For error messages (Destination Unreachable, Time Exceeded), the 4 bytes after
        // the checksum are unused (MUST be zero per RFC 792). Skip them if present.
        identifier = 0;
        sequence_number = 0;
        if (p.buffer().size() >= 4) {
            p.remove_prefix(4);
        }
    }

    // Remaining bytes are the payload.
    payload = std::string(p.buffer().str());

    return p.get_error();
}

uint16_t ICMPMessage::compute_checksum() const {
    // Build the serialized message with the checksum field zeroed, then compute the
    // Internet checksum over the whole thing.
    string s;
    NetUnparser::u8(s, type);
    NetUnparser::u8(s, code);
    NetUnparser::u16(s, 0);  // checksum field = 0 during computation

    if (type == TYPE_ECHO_REQUEST || type == TYPE_ECHO_REPLY) {
        NetUnparser::u16(s, identifier);
        NetUnparser::u16(s, sequence_number);
    } else {
        // 4 unused bytes for error messages
        NetUnparser::u32(s, 0);
    }

    s += payload;

    InternetChecksum check;
    check.add({s.data(), s.size()});
    return check.value();
}

string ICMPMessage::serialize() const {
    // Recompute the checksum so the serialized form is always valid.
    const uint16_t computed = compute_checksum();

    string ret;
    NetUnparser::u8(ret, type);
    NetUnparser::u8(ret, code);
    NetUnparser::u16(ret, computed);

    if (type == TYPE_ECHO_REQUEST || type == TYPE_ECHO_REPLY) {
        NetUnparser::u16(ret, identifier);
        NetUnparser::u16(ret, sequence_number);
    } else {
        NetUnparser::u32(ret, 0);  // 4 unused bytes for error messages
    }

    ret += payload;
    return ret;
}

string ICMPMessage::to_string() const {
    stringstream ss;
    ss << "ICMP type=" << +type << " code=" << +code << " cksum=" << +cksum;
    if (type == TYPE_ECHO_REQUEST || type == TYPE_ECHO_REPLY) {
        ss << " id=" << identifier << " seq=" << sequence_number;
    }
    ss << " payload_len=" << payload.size();
    return ss.str();
}

