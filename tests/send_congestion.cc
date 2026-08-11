#include "sender_harness.hh"
#include "tcp_config.hh"
#include "wrapping_integers.hh"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace std;

//! 期望 TCPSender 的拥塞窗口等于给定值
struct ExpectCwnd : public SenderExpectation {
    size_t _cwnd;
    explicit ExpectCwnd(size_t cwnd) : _cwnd(cwnd) {}
    string description() const { return "cwnd = " + to_string(_cwnd); }
    void execute(TCPSender &sender, queue<TCPSegment> &) const {
        if (sender.congestion_window() != _cwnd) {
            throw SenderExpectationViolation("cwnd was " + to_string(sender.congestion_window()) +
                                             ", expected " + to_string(_cwnd));
        }
    }
};

//! 期望 TCPSender 的 dupack 计数等于给定值
struct ExpectDupAck : public SenderExpectation {
    uint32_t _count;
    explicit ExpectDupAck(uint32_t count) : _count(count) {}
    string description() const { return "dup_ack_count = " + to_string(_count); }
    void execute(TCPSender &sender, queue<TCPSegment> &) const {
        if (sender.dup_ack_count() != _count) {
            throw SenderExpectationViolation("dup_ack_count was " + to_string(sender.dup_ack_count()) +
                                             ", expected " + to_string(_count));
        }
    }
};

//! 期望 TCPSender 处于/不处于快恢复
struct ExpectFastRecovery : public SenderExpectation {
    bool _in;
    explicit ExpectFastRecovery(bool in) : _in(in) {}
    string description() const { return string("in_fast_recovery = ") + (_in ? "true" : "false"); }
    void execute(TCPSender &sender, queue<TCPSegment> &) const {
        if (sender.in_fast_recovery() != _in) {
            throw SenderExpectationViolation("in_fast_recovery was "s +
                                             (sender.in_fast_recovery() ? "true" : "false") +
                                             ", expected " + (_in ? "true" : "false"));
        }
    }
};

int main() {
    try {
        const size_t MSS = TCPConfig::MAX_PAYLOAD_SIZE;  // 1000

        // === 测试 1：慢启动阶段 cwnd 随每个新 ACK 增长 ===
        {
            TCPConfig cfg;
            WrappingInt32 isn(1234);
            cfg.fixed_isn = isn;
            cfg.congestion_control = true;  // 启用拥塞控制

            TCPSenderTestHarness test{"Slow start: cwnd grows with each new ACK", cfg};
            // 初始 SYN：cwnd = MSS，发 SYN（1 字节 in flight）
            test.execute(ExpectSegment{}.with_no_flags().with_syn(true).with_payload_size(0).with_seqno(isn));
            test.execute(ExpectBytesInFlight{1});
            test.execute(ExpectCwnd{MSS});  // 初始 cwnd = 1 MSS

            // ACK SYN：cwnd += 1（SYN 被确认）→ cwnd = 2*MSS
            test.execute(AckReceived{WrappingInt32{isn + 1}}.with_win(10000));
            test.execute(ExpectCwnd{2 * MSS});

            // 写 2*MSS 字节，应一次性发完（cwnd = 2*MSS）
            test.execute(WriteBytes{string(2 * MSS, 'a')});
            test.execute(ExpectSegment{}.with_payload_size(MSS).with_data(string(MSS, 'a')));
            test.execute(ExpectSegment{}.with_payload_size(MSS).with_data(string(MSS, 'a')));
            test.execute(ExpectNoSegment{});

            // ACK 第一段（MSS 字节）：cwnd += MSS → 3*MSS
            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectCwnd{3 * MSS});

            // ACK 第二段（MSS 字节）：cwnd += MSS → 4*MSS
            test.execute(AckReceived{WrappingInt32{isn + 1 + 2 * MSS}}.with_win(10000));
            test.execute(ExpectCwnd{4 * MSS});
        }

        // === 测试 2：超时后 cwnd 回到 1 MSS，ssthresh 减半 ===
        {
            TCPConfig cfg;
            WrappingInt32 isn(5678);
            const uint16_t rto = 100;
            cfg.fixed_isn = isn;
            cfg.rt_timeout = rto;
            cfg.congestion_control = true;

            TCPSenderTestHarness test{"Timeout resets cwnd to 1 MSS", cfg};
            test.execute(ExpectSegment{}.with_no_flags().with_syn(true).with_payload_size(0).with_seqno(isn));
            test.execute(AckReceived{WrappingInt32{isn + 1}}.with_win(10000));
            // cwnd = 2*MSS after SYN ACK

            // 写并 ACK 一段，让 cwnd 增长到 3*MSS
            test.execute(WriteBytes{string(MSS, 'b')});
            test.execute(ExpectSegment{}.with_payload_size(MSS));
            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectCwnd{3 * MSS});

            // 再写一段但不 ACK，等超时
            test.execute(WriteBytes{string(MSS, 'c')});
            test.execute(ExpectSegment{}.with_payload_size(MSS));

            // 超时：cwnd 应回到 1 MSS，ssthresh = max(3*MSS/2, 2*MSS) = 2*MSS
            test.execute(Tick{rto}.with_max_retx_exceeded(false));
            test.execute(ExpectSegment{}.with_payload_size(MSS));  // 重传队首
            test.execute(ExpectCwnd{MSS});  // cwnd 回到 1 MSS
            test.execute(ExpectFastRecovery{false});
        }

        // === 测试 3：3 个 dupack 触发快重传 + 进入快恢复 ===
        {
            TCPConfig cfg;
            WrappingInt32 isn(9999);
            cfg.fixed_isn = isn;
            cfg.congestion_control = true;

            TCPSenderTestHarness test{"Fast retransmit on 3 dupacks", cfg};
            test.execute(ExpectSegment{}.with_no_flags().with_syn(true).with_payload_size(0).with_seqno(isn));
            test.execute(AckReceived{WrappingInt32{isn + 1}}.with_win(10000));
            // cwnd = 2*MSS

            // 写 2 段（各 MSS 字节），只 ACK 第一段
            test.execute(WriteBytes{string(MSS, 'd')});
            test.execute(ExpectSegment{}.with_payload_size(MSS));
            test.execute(WriteBytes{string(MSS, 'e')});
            test.execute(ExpectSegment{}.with_payload_size(MSS));
            test.execute(ExpectBytesInFlight{2 * MSS});

            // ACK 第一段（新 ACK）：cwnd += MSS → 3*MSS
            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectCwnd{3 * MSS});
            test.execute(ExpectDupAck{0});

            // 连续 3 个重复 ACK（ackno 不变 = isn+1+MSS）
            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectDupAck{1});
            test.execute(ExpectFastRecovery{false});

            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectDupAck{2});
            test.execute(ExpectFastRecovery{false});

            // 第 3 个 dupack：触发快重传（重发队首 'e' 段）+ 进入快恢复
            test.execute(AckReceived{WrappingInt32{isn + 1 + MSS}}.with_win(10000));
            test.execute(ExpectSegment{}.with_payload_size(MSS).with_data(string(MSS, 'e')));
            test.execute(ExpectFastRecovery{true});
            // ssthresh = max(cwnd/2, 2*MSS) = max(3*MSS/2, 2*MSS) = 2*MSS (整数除法 3000/2=1500 < 2000)
            // cwnd = ssthresh + 3*MSS = 5*MSS
            test.execute(ExpectCwnd{2 * MSS + 3 * MSS});

            // 收到新 ACK（ACK 'e' 段）：退出快恢复，cwnd = ssthresh = 2*MSS
            test.execute(AckReceived{WrappingInt32{isn + 1 + 2 * MSS}}.with_win(10000));
            test.execute(ExpectFastRecovery{false});
            test.execute(ExpectCwnd{2 * MSS});
        }

        // === 测试 4：未启用拥塞控制时 cwnd 不影响发送（回归保证）===
        {
            TCPConfig cfg;
            WrappingInt32 isn(4242);
            cfg.fixed_isn = isn;
            cfg.congestion_control = false;  // 默认：不启用

            TCPSenderTestHarness test{"CC disabled: large window fully filled", cfg};
            test.execute(ExpectSegment{}.with_no_flags().with_syn(true).with_payload_size(0).with_seqno(isn));
            test.execute(AckReceived{WrappingInt32{isn + 1}}.with_win(50000));
            // 写 50000 字节，应全部发出（不受 cwnd 限制）
            test.execute(WriteBytes{string(50000, 'x')});
            // 期望发出 50 个 MAX_PAYLOAD_SIZE 段
            for (unsigned i = 0; i < 50000 / MSS; ++i) {
                test.execute(ExpectSegment{}.with_payload_size(MSS).with_data(string(MSS, 'x')));
            }
            test.execute(ExpectNoSegment{});
        }

    } catch (const exception &e) {
        cerr << e.what() << endl;
        return 1;
    }

    return EXIT_SUCCESS;
}
