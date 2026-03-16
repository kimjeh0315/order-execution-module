#include "OrderExecutor.h"
#include "dummyordergenerator.h"
#include <cstdio>
#include <thread>
#include <chrono>

using namespace oem;
using Clock = std::chrono::high_resolution_clock;
using us    = std::chrono::microseconds;
using ms    = std::chrono::milliseconds;

// Current Unix timestamp in milliseconds (for order timestamps).
static int64_t now_unix_ms() {
    using namespace std::chrono;
    return (int64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

int main() {
    static constexpr char BANNER[] =
        "========== [Order Execution Module: Core Test Cases] ==========\n\n";
    fwrite(BANNER, 1, sizeof(BANNER) - 1, stdout);
    fflush(stdout);

    const int64_t SCALE = DummyOrderGenerator::SCALE;

    // TC1 / TC2 / TC3 / TC5 share one executor (300k cash — enough for all).
    OrderExecutor executor(300000LL * SCALE);
    executor.registerGateway(0, BinanceGateway("API_KEY", "SECRET_KEY"));
    executor.registerGateway(1, HyperliquidGateway("0xWALLET", "PRIVATE_KEY"));
    std::this_thread::sleep_for(ms(30)); // let logger flush startup messages

    const int64_t ts = now_unix_ms(); // shared base timestamp for all TCs

    // =========================================================================
    // TC1: Next-Tick Sequential Orders — Zero-Blocking Proof
    // =========================================================================
    {
        static constexpr char HDR[] =
            "\n[TC1] Next-Tick Sequential Orders (Zero-Blocking)\n";
        fwrite(HDR, 1, sizeof(HDR) - 1, stdout);
        fflush(stdout);

        auto tc1 = DummyOrderGenerator::tc1_next_tick(ts);

        // Measure wall-clock cost of each receiveOrder call.
        // If non-blocking: each call returns in ~microseconds, NOT 50ms.
        auto t0 = Clock::now();
        executor.receiveOrder(tc1[0]); // BTCUSDT BUY  → dispatched to thread pool
        auto t1 = Clock::now();
        executor.receiveOrder(tc1[1]); // SOLUSDT BUY  → dispatched immediately
        auto t2 = Clock::now();

        char buf[128];
        int n = snprintf(buf, sizeof(buf),
            "[TC1-Timing] BTCUSDT dispatch: %lldus | SOLUSDT dispatch: %lldus"
            " (gateway I/O is async — not blocking)\n",
            (long long)std::chrono::duration_cast<us>(t1 - t0).count(),
            (long long)std::chrono::duration_cast<us>(t2 - t1).count());
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
    }
    std::this_thread::sleep_for(ms(100));

    // =========================================================================
    // TC2: Concurrent Multi-Symbol Orders — SpinLock Contention Test
    // =========================================================================
    {
        static constexpr char HDR[] =
            "\n[TC2] Concurrent Multi-Symbol Orders (3 threads, same millisecond)\n";
        fwrite(HDR, 1, sizeof(HDR) - 1, stdout);
        fflush(stdout);

        auto tc2 = DummyOrderGenerator::tc2_concurrent_3symbols(ts);

        // All three threads fire at the same instant.
        // SpinLock serializes the book updates; no order should be lost.
        std::thread t1([&](){ executor.receiveOrder(tc2[0]); }); // ETHUSDT
        std::thread t2([&](){ executor.receiveOrder(tc2[1]); }); // XRPUSDT
        std::thread t3([&](){ executor.receiveOrder(tc2[2]); }); // ADAUSDT
        t1.join(); t2.join(); t3.join();
    }
    std::this_thread::sleep_for(ms(100));

    // =========================================================================
    // TC3: SPOT Buy-Pending → Sell Conflict — Oversell Guard
    //   SPOT has no short selling; FUTURES/OPTIONS would accept a short here.
    // =========================================================================
    {
        static constexpr char HDR[] =
            "\n[TC3] SPOT Buy Pending → Sell Conflict (REJECT_OVERSELL expected)\n";
        fwrite(HDR, 1, sizeof(HDR) - 1, stdout);
        fflush(stdout);

        auto tc3 = DummyOrderGenerator::tc3_buy_pending_sell(ts);

        executor.receiveOrder(tc3[0]); // SPOT BUY dispatched (not yet confirmed)

        fwrite("[Alpha] SELL signal fires before BUY is confirmed.\n", 1, 50, stdout);
        fflush(stdout);
        executor.receiveOrder(tc3[1]); // SPOT SELL → REJECT_OVERSELL (no position yet)

        fwrite("[Network] Late BUY confirmation arrives for Order 301.\n", 1, 54, stdout);
        fflush(stdout);
        executor.onOrderExecuted(tc3[0]); // position book now updated
    }
    std::this_thread::sleep_for(ms(100));

    // =========================================================================
    // TC4: Real-Time Cash Validation — Unrealized Profit Must NOT Cover Margin
    //   Dedicated executor: cash=100,000 | unrealized=20,000 | order=150,000
    // =========================================================================
    {
        static constexpr char HDR[] =
            "\n[TC4] Cash Validation with Unrealized Profit (REJECT_CASH expected)\n";
        fwrite(HDR, 1, sizeof(HDR) - 1, stdout);
        fflush(stdout);

        OrderExecutor exec4(100000LL * SCALE);
        exec4.registerGateway(0, BinanceGateway("API_KEY_4", "SECRET_KEY_4"));
        std::this_thread::sleep_for(ms(20));

        // Inject unrealized profit from the position calculator.
        exec4.updateUnrealizedProfit(20000LL * SCALE);

        auto tc4 = DummyOrderGenerator::tc4_cash_overflow(ts);
        exec4.receiveOrder(tc4); // 150,000 > cash(100,000) → REJECT_CASH
                                 // Log will show: required=150000 | cash=100000 | unrealized=20000

        std::this_thread::sleep_for(ms(50)); // flush TC4 logs
    }

    // =========================================================================
    // TC5: Stale Signal Rejection — Age 2,000ms > Limit 500ms
    // =========================================================================
    {
        static constexpr char HDR[] =
            "\n[TC5] Stale Signal Rejection (STALE expected, age=~2000ms > 500ms)\n";
        fwrite(HDR, 1, sizeof(HDR) - 1, stdout);
        fflush(stdout);

        // Signal was "generated" 2 seconds ago; the alpha model allows max 500ms.
        // The executor sees it now and computes age ≈ 2000ms → STALE.
        auto tc5 = DummyOrderGenerator::tc5_stale_signal(ts);
        executor.receiveOrder(tc5); // must never reach the gateway
    }
    std::this_thread::sleep_for(ms(50));

    fwrite("\n========== [All Test Cases Completed] ==========\n", 1, 51, stdout);
    fflush(stdout);

    return 0;
}
