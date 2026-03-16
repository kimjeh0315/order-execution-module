#pragma once
#include "OrderExecutor.h"
#include <array>
#include <cstring>

namespace oem {

class DummyOrderGenerator {
public:
    static constexpr int64_t SCALE = 100000000LL;

    // Builds an OrderRequest; price and qty are unscaled human values (e.g. 70000, 1).
    // max_age: signal expiry in ms (0 = no stale check).
    static OrderRequest make_order(int id, int ex_id, int inst, int lev,
                                   const char* sym, int side,
                                   int64_t price, int64_t qty, int64_t ts,
                                   int max_age = 0) {
        OrderRequest req{};
        req.order_id          = id;
        req.exchange_id       = ex_id;
        req.inst_type         = inst;
        req.leverage          = lev;
        std::strncpy(req.symbol, sym, sizeof(req.symbol) - 1); // safe; req{} zeroes last byte
        req.side              = side;
        req.max_signal_age_ms = max_age;
        req.price_ticks       = price * SCALE;
        req.qty_ticks         = qty   * SCALE;
        req.timestamp_ms      = ts;
        return req;
    }

    // -------------------------------------------------------------------------
    // TC1: Next-tick sequential orders on two different symbols.
    //   Demonstrates Zero-Blocking: both receiveOrder calls return in microseconds
    //   regardless of the 50ms network I/O behind each gateway dispatch.
    // -------------------------------------------------------------------------
    static std::array<OrderRequest, 2> tc1_next_tick(int64_t now) {
        return {
            make_order(101, 0, 0, 1, "BTCUSDT", 0, 70000, 1, now),
            make_order(102, 0, 0, 1, "SOLUSDT", 0,   150, 5, now + 1) // ~next tick
        };
    }

    // -------------------------------------------------------------------------
    // TC2: Three symbols at the exact same millisecond.
    //   SpinLock must serialize book updates; no order may be silently lost.
    // -------------------------------------------------------------------------
    static std::array<OrderRequest, 3> tc2_concurrent_3symbols(int64_t now) {
        return {
            make_order(201, 0, 0, 1, "ETHUSDT", 0, 3500, 2,    now),
            make_order(202, 0, 0, 1, "XRPUSDT", 0,    1, 1000, now),
            make_order(203, 0, 0, 1, "ADAUSDT", 0,    1, 500,  now)
        };
    }

    // -------------------------------------------------------------------------
    // TC3: SPOT BUY dispatched but not yet confirmed; SPOT SELL arrives immediately.
    //   SPOT has no short selling — must own the asset first.
    //   Expected: SELL → REJECT_OVERSELL (position book still empty).
    //   After onOrderExecuted(), position is recorded and a subsequent SELL would pass.
    //
    //   Note: For FUTURES/OPTIONS, the same scenario would NOT reject because
    //   short selling is allowed (validateOrder checks margin, not inventory).
    // -------------------------------------------------------------------------
    static std::array<OrderRequest, 2> tc3_buy_pending_sell(int64_t now) {
        return {
            make_order(301, 0, 0, 1, "ETHUSDT", 0, 3500, 10, now),     // SPOT BUY
            make_order(302, 0, 0, 1, "ETHUSDT", 1, 3500, 10, now + 5)  // SPOT SELL → REJECT_OVERSELL
        };
    }

    // -------------------------------------------------------------------------
    // TC4: Cash-only margin validation (unrealized profit must NOT extend margin).
    //   Executor: cash=100,000 | unrealized=20,000 → total portfolio=120,000
    //   Order: 150,000 → required=150,000 > cash=100,000 → REJECT_CASH
    // -------------------------------------------------------------------------
    static OrderRequest tc4_cash_overflow(int64_t now) {
        return make_order(401, 0, 0, 1, "BTCUSDT", 0, 150000, 1, now);
    }

    // -------------------------------------------------------------------------
    // TC5: Stale signal — generated 2,000ms ago, but alpha model allows max 500ms.
    //   Expected: STALE rejection before hitting the SpinLock or gateway.
    // -------------------------------------------------------------------------
    static OrderRequest tc5_stale_signal(int64_t now) {
        return make_order(501, 0, 0, 1, "SOLUSDT", 0, 200, 3, now - 2000, /*max_age=*/500);
    }
};

} // namespace oem
