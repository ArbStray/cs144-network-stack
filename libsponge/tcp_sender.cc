#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <algorithm>
#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
//! \param[in] enable_cc whether to enable TCP Reno congestion control
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn,
                     const bool enable_cc)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity)
    , _timer(retx_timeout)
    , _congestion_control_enabled(enable_cc) {}

size_t TCPSender::bytes_in_flight() const { return _bytes_in_flight; }

void TCPSender::fill_window() {
    // 有效发送窗口 = min(对端 advertised window, 拥塞窗口 cwnd)。
    // 未启用拥塞控制时仅取 advertised window（0 按 1 处理，保持零窗口探测语义）。
    const size_t window_size = _effective_window_size();
    while (_bytes_in_flight < window_size) {
        TCPSegment seg;
        // 首先发 SYN 包，不含 payload（因为初始时 window_size 为 1）
        if (!_set_syn_flag) {
            seg.header().syn = true;
            _set_syn_flag = true;
        }

        // MAX_PAYLOAD_SIZE 只限制字符串长度并不包括 SYN 和 FIN，但是 window_size 包括 SYN 和 FIN
        auto payload_size = min(TCPConfig::MAX_PAYLOAD_SIZE, \
                            min(window_size - _bytes_in_flight - seg.header().syn, _stream.buffer_size()));
        auto payload = _stream.read(payload_size);
        seg.payload() = Buffer(std::move(payload));

        // 如果读到 EOF 了且 window_size 还有空位
        if (!_set_fin_flag && _stream.eof() && _bytes_in_flight + seg.length_in_sequence_space() < window_size) {
            seg.header().fin = true;
            _set_fin_flag = true;
        }

        // 空数据报就不发送了
        uint64_t length;
        if ((length = seg.length_in_sequence_space()) == 0) break;

        // 发送
        seg.header().seqno = next_seqno(); // next_seqno() 是 TCP seqno
        _segments_out.push(seg);

        // 如果定时器关闭，则启动定时器
        if (!_timer.is_running()) _timer.restart();

        // 保存备份，重发时可能会用
        _outstanding_seg.emplace(_next_seqno, std::move(seg));
        
        // 更新序列号和发出但未 ACK 的字节数
        _next_seqno += length; // _next_seqno 是 absolute seqno
        _bytes_in_flight += length;
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    auto abs_ackno = unwrap(ackno, _isn, next_seqno_absolute());
    if (abs_ackno > next_seqno_absolute()) return;  // 传入的 ACK 是不可靠的，直接丢弃

    // 记录本次 ACK 推进了多少字节（用于拥塞控制判断）
    size_t newly_acked_bytes = 0;

    // 处理已经收到的包（序列号空间要小于 ACK）
    while (!_outstanding_seg.empty()) {
        auto &[abs_seq, seg] = _outstanding_seg.front();
        if (abs_seq + seg.length_in_sequence_space() - 1 < abs_ackno) {
            newly_acked_bytes += seg.length_in_sequence_space();
            _bytes_in_flight -= seg.length_in_sequence_space();
            _outstanding_seg.pop();
        } else {
            break;
        }
    }

    // ==== 拥塞控制状态更新 ====
    if (_congestion_control_enabled) {
        if (newly_acked_bytes > 0) {
            // 新 ACK：推进窗口的 ACK
            _on_new_ack(newly_acked_bytes);
        } else if (abs_ackno == _last_ackno_absolute) {
            // 重复 ACK：abs_ackno 没有推进
            // 注意：只有当 _outstanding_seg 非空时才计数（否则所有段已被确认，dupack 无意义）
            _on_dup_ack();
        }
        // 如果是 abs_ackno < _last_ackno_absolute（旧 ACK），忽略，不做任何 cc 调整
        _last_ackno_absolute = abs_ackno;
    }

    // 有成功 ACK 的包，则重置定时器，清零连续重传次数
    if (newly_acked_bytes > 0) {
        _consecutive_retransmissions_count = 0;
        _timer.set_time_out(_initial_retransmission_timeout);
        _timer.restart();
    }

    // 没有等待 ACK 的包了，则关闭定时器
    if (_bytes_in_flight == 0) {
        _timer.stop();
    }

    // 更新 window_size，并尝试填满窗口
    _window_size = window_size;
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    _timer.tick(ms_since_last_tick);

    // 定时器超时（已经确保定时器已经打开），如果定时器关闭不会超时检查不会返回 true
    // 理论上不用检测 _outstanding_seg 非空，但为了鲁棒性就检测下吧
    if (_timer.check_time_out() && !_outstanding_seg.empty()) {
        // ==== 拥塞控制：超时事件 ====
        if (_congestion_control_enabled) {
            _on_congestion_timeout();
        }

        // 重传最早的报文
        _segments_out.push(_outstanding_seg.front().second);

        // window_size 非 0 对应的操作
        if (_window_size > 0) {
            ++_consecutive_retransmissions_count;
            _timer.set_time_out(_timer.get_time_out() * 2);
        }

        // 重启定时器
        _timer.restart();
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions_count; }

void TCPSender::send_empty_segment() {
    // 发送空数据报，可以用于仅仅 ACK
    TCPSegment seg;
    seg.header().seqno = next_seqno();
    _segments_out.emplace(std::move(seg));
}

//! 返回有效发送窗口大小：min(advertised_window, cwnd)。
//! 未启用拥塞控制时仅返回 advertised window（0 按 1 处理）。
size_t TCPSender::_effective_window_size() const {
    // 对端 advertised window 为 0 时按 1 处理（零窗口探测语义）
    const size_t adv = max(static_cast<size_t>(_window_size), size_t(1));
    if (!_congestion_control_enabled) {
        return adv;
    }
    return min(adv, _congestion_window);
}

//! 收到推进窗口的新 ACK 时的处理：慢启动 / 拥塞避免的 cwnd 增长，退出快恢复。
void TCPSender::_on_new_ack(const size_t newly_acked_bytes) {
    // 如果处于快恢复中，收到新 ACK 表示恢复完成：cwnd = ssthresh，退出快恢复
    if (_in_fast_recovery) {
        _congestion_window = _slow_start_threshold;
        _in_fast_recovery = false;
    }

    if (_congestion_window < _slow_start_threshold) {
        // 慢启动：每收到一个新 ACK，cwnd += newly_acked_bytes
        // （按字节实现，等价于每段被 ACK 时 cwnd 增加该段长度）
        _congestion_window += newly_acked_bytes;
    } else {
        // 拥塞避免：每 RTT cwnd 增长约 1 MSS
        // 近似为每 ACK：cwnd += MSS * newly_acked_bytes / cwnd
        // （避免整数除法为 0，cwnd 至少为 1 MSS）
        _congestion_window += TCPConfig::MAX_PAYLOAD_SIZE * newly_acked_bytes / _congestion_window;
    }

    // 收到新 ACK 重置 dupack 计数
    _dup_ack_count = 0;
}

//! 收到重复 ACK 时的处理：计数 dupack，3 个触发快重传 + 进入快恢复。
void TCPSender::_on_dup_ack() {
    // 如果所有段已被确认（_outstanding_seg 为空），重复 ACK 无意义，不计数
    if (_outstanding_seg.empty()) {
        return;
    }

    ++_dup_ack_count;

    if (_dup_ack_count == 3 && !_in_fast_recovery) {
        // 快重传：ssthresh = max(cwnd/2, 2*MSS)，cwnd = ssthresh + 3*MSS
        _slow_start_threshold = max(_congestion_window / 2,
                                    2 * TCPConfig::MAX_PAYLOAD_SIZE);
        _congestion_window = _slow_start_threshold + 3 * TCPConfig::MAX_PAYLOAD_SIZE;
        _in_fast_recovery = true;
        // 立即重传队首段（不等超时）
        _segments_out.push(_outstanding_seg.front().second);
        // 注意：快重传不重置 RTO 定时器（标准 Reno 行为）
    } else if (_in_fast_recovery && _dup_ack_count > 3) {
        // 快恢复期间每多一个 dupack，cwnd += 1 MSS（RFC 5681 窗口膨胀）
        _congestion_window += TCPConfig::MAX_PAYLOAD_SIZE;
    }
}

//! 超时拥塞事件处理：ssthresh = max(cwnd/2, 2*MSS)，cwnd 回到 1 MSS。
void TCPSender::_on_congestion_timeout() {
    _slow_start_threshold = max(_congestion_window / 2,
                                2 * TCPConfig::MAX_PAYLOAD_SIZE);
    _congestion_window = TCPConfig::MAX_PAYLOAD_SIZE;  // 回到 1 MSS，重新慢启动
    _dup_ack_count = 0;
    _in_fast_recovery = false;
}
