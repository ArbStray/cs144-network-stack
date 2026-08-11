#ifndef SPONGE_LIBSPONGE_TCP_SENDER_HH
#define SPONGE_LIBSPONGE_TCP_SENDER_HH

#include "byte_stream.hh"
#include "tcp_config.hh"
#include "tcp_segment.hh"
#include "wrapping_integers.hh"

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

//! \brief TCPSender 的计时器，最长定时时间（ms）不能超过 uint32
class Timer {
private:
    uint32_t _time_count = 0;
    uint32_t _time_out = 0;
    bool _is_running = false;
public:
    Timer() = default;
    Timer(const uint32_t time_out) : _time_out(time_out) {}
    // void start() { _is_running = true; }
    void stop() { _is_running = false; }
    void set_time_out(const uint32_t time_out) { _time_out = time_out; }
    uint32_t get_time_out() const { return _time_out; }
    void restart() { _is_running = true, _time_count = 0; }
    void tick(const size_t ms_since_last_tick) {
        if (_is_running)
            _time_count += ms_since_last_tick; 
    }
    bool check_time_out() const { return _is_running && _time_count >= _time_out; }
    bool is_running() const { return _is_running; }
};

//! \brief The "sender" part of a TCP implementation.

//! Accepts a ByteStream, divides it up into segments and sends the
//! segments, keeps track of which segments are still in-flight,
//! maintains the Retransmission Timer, and retransmits in-flight
//! segments if the retransmission timer expires.
class TCPSender {
  private:
    //! our initial sequence number, the number for our SYN.
    WrappingInt32 _isn;

    //! outbound queue of segments that the TCPSender wants sent
    std::queue<TCPSegment> _segments_out{};

    //! retransmission timer for the connection
    unsigned int _initial_retransmission_timeout;

    //! outgoing stream of bytes that have not yet been sent
    ByteStream _stream;

    //! the (absolute) sequence number for the next byte to be sent
    uint64_t _next_seqno{0};

    //! 重传定时器
    Timer _timer;

    //! 已经发出但还未收到 ACK 确认的 TCPSegment 队列
    std::queue<std::pair<uint64_t, TCPSegment> > _outstanding_seg{};

    //! 连续重传次数
    uint32_t _consecutive_retransmissions_count = 0;

    //! 已经发送出去但还未收到 ACK 确认的字节数
    size_t _bytes_in_flight = 0;

    //! 窗口大小，根据文档初始值应为 1
    uint16_t _window_size = 1;

    //! 是否发送带 SYN/FIN 的包
    bool _set_syn_flag = false, _set_fin_flag = false;

    //! ==== TCP Reno 拥塞控制状态 ====

    //! 是否启用拥塞控制（来自 TCPConfig::congestion_control）。为 false 时发送窗口
    //! 仅由对端 advertised window 决定，即原始 CS144 行为。
    bool _congestion_control_enabled = false;

    //! 拥塞窗口 cwnd，单位字节。启用时初始为 1 MSS (MAX_PAYLOAD_SIZE)。
    size_t _congestion_window = TCPConfig::MAX_PAYLOAD_SIZE;

    //! 慢启动阈值 ssthresh，单位字节。初始为 size_t 最大值（即无限大），
    //! 保证在首次丢包前一直处于慢启动阶段。
    size_t _slow_start_threshold = std::numeric_limits<size_t>::max();

    //! 连续重复 ACK 计数（用于快重传）。重复 ACK 指未推进 abs_ackno 的 ACK。
    uint32_t _dup_ack_count = 0;

    //! 上次收到的 abs_ackno，用于判断当前 ACK 是否为重复 ACK。
    uint64_t _last_ackno_absolute = 0;

    //! 是否处于快恢复（fast recovery）状态。
    bool _in_fast_recovery = false;

    //! 返回有效发送窗口大小：min(advertised_window, cwnd)。
    //! 当对端 advertised window 为 0 时按 1 处理（零窗口探测语义）。
    //! 未启用拥塞控制时仅返回 advertised window（0 按 1）。
    size_t _effective_window_size() const;

    //! 收到推进窗口的新 ACK 时的处理：慢启动 / 拥塞避免的 cwnd 增长，退出快恢复。
    void _on_new_ack(size_t newly_acked_bytes);

    //! 收到重复 ACK 时的处理：计数 dupack，3 个触发快重传 + 进入快恢复。
    void _on_dup_ack();

    //! 超时拥塞事件处理：ssthresh = max(cwnd/2, 2*MSS)，cwnd 回到 1 MSS。
    void _on_congestion_timeout();


  public:
    //! Initialize a TCPSender
    //! \param[in] capacity the capacity of the outgoing byte stream
    //! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
    //! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
    //! \param[in] enable_cc whether to enable TCP Reno congestion control
    TCPSender(const size_t capacity = TCPConfig::DEFAULT_CAPACITY,
              const uint16_t retx_timeout = TCPConfig::TIMEOUT_DFLT,
              const std::optional<WrappingInt32> fixed_isn = {},
              const bool enable_cc = false);

    //! \name "Input" interface for the writer
    //!@{
    ByteStream &stream_in() { return _stream; }
    const ByteStream &stream_in() const { return _stream; }
    //!@}

    //! \name Methods that can cause the TCPSender to send a segment
    //!@{

    //! \brief A new acknowledgment was received
    void ack_received(const WrappingInt32 ackno, const uint16_t window_size);

    //! \brief Generate an empty-payload segment (useful for creating empty ACK segments)
    void send_empty_segment();

    //! \brief create and send segments to fill as much of the window as possible
    void fill_window();

    //! \brief Notifies the TCPSender of the passage of time
    void tick(const size_t ms_since_last_tick);
    //!@}

    //! \name Accessors
    //!@{

    //! \brief How many sequence numbers are occupied by segments sent but not yet acknowledged?
    //! \note count is in "sequence space," i.e. SYN and FIN each count for one byte
    //! (see TCPSegment::length_in_sequence_space())
    size_t bytes_in_flight() const;

    //! \brief Number of consecutive retransmissions that have occurred in a row
    unsigned int consecutive_retransmissions() const;

    //! \brief TCPSegments that the TCPSender has enqueued for transmission.
    //! \note These must be dequeued and sent by the TCPConnection,
    //! which will need to fill in the fields that are set by the TCPReceiver
    //! (ackno and window size) before sending.
    std::queue<TCPSegment> &segments_out() { return _segments_out; }
    //!@}

    //! \name What is the next sequence number? (used for testing)
    //!@{

    //! \brief absolute seqno for the next byte to be sent
    uint64_t next_seqno_absolute() const { return _next_seqno; }

    //! \brief relative seqno for the next byte to be sent
    WrappingInt32 next_seqno() const { return wrap(_next_seqno, _isn); }

    //! \brief current congestion window size in bytes (for testing/inspection)
    size_t congestion_window() const { return _congestion_window; }

    //! \brief current slow-start threshold in bytes (for testing/inspection)
    size_t slow_start_threshold() const { return _slow_start_threshold; }

    //! \brief number of consecutive duplicate ACKs seen (for testing/inspection)
    uint32_t dup_ack_count() const { return _dup_ack_count; }

    //! \brief whether the sender is currently in fast recovery (for testing/inspection)
    bool in_fast_recovery() const { return _in_fast_recovery; }
    //!@}
};

#endif  // SPONGE_LIBSPONGE_TCP_SENDER_HH
