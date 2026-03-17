#include "OrderExecutor.h"
#include <cstdio>
#include <chrono>
#include <mutex>

static constexpr int64_t SCALE = 100000000LL;

// Current wall-clock time in milliseconds (used for stale-signal detection).
static inline int64_t get_now_ms() noexcept {
    using namespace std::chrono;
    return (int64_t)duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// Gateway implementations (async via ThreadPool).
BinanceGateway::BinanceGateway(const char* key, const char* secret) {
    snprintf(api_key,    sizeof(api_key),    "%s", key);
    snprintf(secret_key, sizeof(secret_key), "%s", secret);
}

void BinanceGateway::sendOrderAsync(const OrderRequest& order, ThreadPool& pool) {
    pool.submit([order]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        char buf[80];
        int  n = snprintf(buf, sizeof(buf),
            "[CEX-Binance] Order Dispatched. ID: %d (Latency: ~50ms)\n", order.order_id);
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
    });
}

HyperliquidGateway::HyperliquidGateway(const char* wallet, const char* pkey) {
    snprintf(wallet_address, sizeof(wallet_address), "%s", wallet);
    snprintf(private_key,    sizeof(private_key),    "%s", pkey);
}

void HyperliquidGateway::sendOrderAsync(const OrderRequest& order, ThreadPool& pool) {
    pool.submit([order]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));    // EIP-712 sign
        std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // block confirm
        char buf[96];
        int  n = snprintf(buf, sizeof(buf),
            "[DEX-Hyperliquid] Order Confirmed on-chain. ID: %d (Latency: ~1050ms)\n",
            order.order_id);
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
    });
}

OrderExecutor::OrderExecutor(int64_t initial_cash_ticks) : cash_ticks(initial_cash_ticks) {
    logger.pushf("[System] ThreadPool ready. %d spinning workers, lock-free queue (cap=%d).\n",
                 ThreadPool::POOL_SIZE, 64);
    logger.pushf("[System] Executor Ready. Cash: %lld ticks\n", (long long)cash_ticks);
}

Verdict OrderExecutor::validateOrder(const OrderRequest& order, int64_t now_ms) noexcept {
    // Stale-signal guard: reject if the signal is older than the alpha model allows.
    if (order.max_signal_age_ms > 0 &&
        now_ms - order.timestamp_ms > order.max_signal_age_ms)
        return Verdict::STALE;

    // Pre-compute notional and required margin (used by both BUY and FUTURES/OPTIONS SELL).
    // Divide first to avoid int64 overflow (price*qty*SCALE^2 can exceed 9.2e18).
    const int64_t notional = (order.price_ticks / SCALE) * order.qty_ticks;
    const int64_t required = notional / order.leverage;

    if (order.side == 0) {  // BUY (all instruments: cash margin required)
        if (cash_ticks < required) return Verdict::REJECT_CASH;

    } else if (order.side == 1) {  // SELL
        if (order.inst_type == 0) {
            // SPOT: short selling is impossible — must own the asset.
            const PosKey    key = makePosKey(order.exchange_id, order.inst_type, order.symbol);
            const Position* pos = positions.find(key);
            if (!pos || pos->qty_ticks < order.qty_ticks)
                return Verdict::REJECT_OVERSELL;
        } else {
            // FUTURES / OPTIONS: short selling is allowed.
            // Treat like a BUY — require initial margin from cash.
            if (cash_ticks < required) return Verdict::REJECT_CASH;
        }
    }
    return Verdict::OK;
}

void OrderExecutor::updateUnrealizedProfit(int64_t profit_ticks) noexcept {
    std::lock_guard<SpinLock> lock(spin);
    unrealized_profit_ticks = profit_ticks;
}

// receiveOrder: validate, deduct margin, dispatch to gateway.
void OrderExecutor::receiveOrder(const OrderRequest& order) {
    // Capture time before spinlock — stale check needs wall clock, not protected state.
    const int64_t now_ms = get_now_ms();

    Verdict v;
    int     gw_id          = -1;
    int64_t snap_cash       = 0; // captured inside lock for REJECT_CASH log
    int64_t snap_unrealized = 0;

    {   // critical section (~30ns target)
        std::lock_guard<SpinLock> lock(spin);
        v = validateOrder(order, now_ms);
        if (v == Verdict::OK) {
            // BUY always deducts margin.
            // FUTURES/OPTIONS short-sell (side=1, inst!=SPOT) also deducts initial margin.
            if (order.side == 0 || (order.side == 1 && order.inst_type != 0)) {
                const int64_t notional = (order.price_ticks / SCALE) * order.qty_ticks;
                cash_ticks -= notional / order.leverage;
            }
            if (order.exchange_id >= 0 &&
                order.exchange_id < MAX_EXCHANGES &&
                gateways[order.exchange_id].has_value())
                gw_id = order.exchange_id;
        } else if (v == Verdict::REJECT_CASH) {
            snap_cash       = cash_ticks;
            snap_unrealized = unrealized_profit_ticks;
        }
    }

    if (v != Verdict::OK) {
        if (v == Verdict::REJECT_CASH) {
            const int64_t required = ((order.price_ticks / SCALE) * order.qty_ticks)
                                     / order.leverage;
            logger.pushf("[Reject] REJECT_CASH. Order %d | required=%lld"
                         " | cash=%lld | unrealized=%lld\n",
                         order.order_id,
                         (long long)(required          / SCALE),
                         (long long)(snap_cash         / SCALE),
                         (long long)(snap_unrealized   / SCALE));
        } else if (v == Verdict::REJECT_OVERSELL) {
            // SPOT-only: short selling is not supported on spot markets.
            logger.pushf("\n[CRITICAL REJECT] SPOT SELL with no position."
                         " Order: %d, Symbol: %s\n",
                         order.order_id, order.symbol);
        } else if (v == Verdict::STALE) {
            logger.pushf("[Stale] Signal expired. Order %d | age=%lldms > limit=%dms\n",
                         order.order_id,
                         (long long)(now_ms - order.timestamp_ms),
                         order.max_signal_age_ms);
        }
        return;
    }

    if (gw_id >= 0) {
        std::visit([&](auto& gw){ gw.sendOrderAsync(order, pool); },
                   *gateways[gw_id]);
    } else {
        logger.pushf("[ERROR] Unknown Exchange ID: %d\n", order.exchange_id);
        // Refund whatever margin was deducted above.
        if (order.side == 0 || (order.side == 1 && order.inst_type != 0)) {
            std::lock_guard<SpinLock> lock(spin);
            const int64_t notional = (order.price_ticks / SCALE) * order.qty_ticks;
            cash_ticks += notional / order.leverage;
        }
    }
}

// onOrderExecuted: update position book; settle cash for SPOT fills.
void OrderExecutor::onOrderExecuted(const OrderRequest& order) {
    char    log_key[24] = {};
    int64_t log_qty     = 0;
    bool    check_load  = false;
    int     pos_size    = 0;

    {   // critical section
        std::lock_guard<SpinLock> lock(spin);
        const PosKey key = makePosKey(order.exchange_id, order.inst_type, order.symbol);
        memcpy(log_key, key.buf, sizeof(key.buf));

        if (order.side == 0) {  // BUY fill: credit position
            positions[key].qty_ticks += order.qty_ticks;
            log_qty    = order.qty_ticks;
            check_load = positions.overloaded();
            pos_size   = positions.size();
        } else if (order.side == 1) {  // SELL fill: debit position
            positions[key].qty_ticks -= order.qty_ticks;
            log_qty = -order.qty_ticks;

            if (order.inst_type == 0) {
                // SPOT: immediate proceeds settlement — return full sale price to cash.
                const int64_t notional = (order.price_ticks / SCALE) * order.qty_ticks;
                cash_ticks += notional; // SPOT leverage must always be 1
            }
            // FUTURES / OPTIONS: no cash change on fill.
            // Margin is held until the position is closed (open short → negative qty_ticks).
            // P&L and margin return are managed externally via updateUnrealizedProfit().
        }
    }

    if (check_load && !load_warn_fired.exchange(true, std::memory_order_relaxed)) {
        logger.pushf("[WARN] FlatHashMap load > 50%% (%d / 512 slots). "
                     "Consider increasing CAP to prevent overflow.\n", pos_size);
    }

    if (order.side == 0)
        logger.pushf("[System] BUY Filled.  +%lld [%s]\n",
                     (long long)(log_qty / SCALE), log_key);
    else if (order.side == 1)
        logger.pushf("[System] SELL Filled.  %lld [%s]\n",
                     (long long)(log_qty / SCALE), log_key);
}
