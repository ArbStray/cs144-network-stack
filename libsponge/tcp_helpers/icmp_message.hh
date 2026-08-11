#ifndef SPONGE_LIBSPONGE_ICMP_MESSAGE_HH
#define SPONGE_LIBSPONGE_ICMP_MESSAGE_HH

#include "parser.hh"

#include <cstdint>
#include <string>

//! \brief [ICMP](\ref rfc::rfc792) message
//!
//! Implements a subset of ICMP sufficient for:
//!  - Echo Request / Echo Reply (type 8 / 0) -- used by ping
//!  - Destination Unreachable (type 3) -- sent when no route matches
//!  - Time Exceeded (type 11) -- sent when TTL reaches 0
//!
//! ~~~{.txt}
//!   0                   1                   2                   3
//!   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//!  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//!  |     Type      |     Code      |          Checksum             |
//!  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//!  |           Identifier          |        Sequence Number        |  (Echo only)
//!  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//!  |                          Payload ...                          |
//!  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//! ~~~
struct ICMPMessage {
    static constexpr size_t MIN_LENGTH = 4;   //!< ICMP header minimum length (type+code+checksum)
    static constexpr size_t ECHO_LENGTH = 8;  //!< ICMP echo header length (with id+seq)

    //! ICMP type codes
    enum Type : uint8_t {
        TYPE_ECHO_REPLY = 0,            //!< Echo Reply
        TYPE_DESTINATION_UNREACHABLE = 3,  //!< Destination Unreachable
        TYPE_ECHO_REQUEST = 8,          //!< Echo Request
        TYPE_TIME_EXCEEDED = 11,        //!< Time Exceeded
    };

    //! ICMP type field
    uint8_t type{};
    //! ICMP code field (meaning depends on type)
    uint8_t code{};
    //! ICMP checksum field (stored as-is on parse; recomputed on serialize)
    uint16_t cksum{};

    //! Echo identifier (only meaningful for Echo Request/Reply)
    uint16_t identifier{};
    //! Echo sequence number (only meaningful for Echo Request/Reply)
    uint16_t sequence_number{};

    //! Payload (echo data, or original datagram for error messages)
    std::string payload{};

    //! Parse an ICMP message from a buffer. Returns NoError on success.
    ParseResult parse(const Buffer buffer);

    //! Serialize the ICMP message to a string, recomputing the checksum.
    std::string serialize() const;

    //! Compute the Internet checksum over the ICMP message (with cksum field = 0).
    uint16_t compute_checksum() const;

    //! Return a human-readable string.
    std::string to_string() const;
};

#endif  // SPONGE_LIBSPONGE_ICMP_MESSAGE_HH
