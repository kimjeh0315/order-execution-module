#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <charconv>
#include <atomic>
#include <thread>
#include <optional>
#include <variant>
#include <type_traits>

// Lightweight spin lock for very short critical sections.
class SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
    void lock()     noexcept { while (flag.test_and_set(std::memory_order_acquire)) {} }
    void unlock()   noexcept { flag.clear(std::memory_order_release); }
    bool try_lock() noexcept { return !flag.test_and_set(std::memory_order_acquire); }
};

// Fixed-size, heap-free callable used by the thread pool.
struct alignas(64) Task {
    static constexpr size_t STORAGE = 128; // fits OrderRequest(64B) with headroom
    using Fn = void (*)(void*);

    Fn   invoke_fn  = nullptr;
    Fn   destroy_fn = nullptr;
    alignas(8) char storage[STORAGE];

    template<typename F>
    static Task make(F&& fn) noexcept {
        using FD = std::decay_t<F>;
        static_assert(sizeof(FD) <= STORAGE,
            "Lambda capture exceeds Task::STORAGE(64B). Increase STORAGE or reduce captures.");
        static_assert(std::is_trivially_copyable_v<FD>,
            "Task requires trivially copyable captures (no std::string, unique_ptr, etc.)");
        Task t{};
        new (t.storage) FD(std::forward<F>(fn));
        t.invoke_fn  = [](void* p){ (*static_cast<FD*>(p))(); };
        t.destroy_fn = [](void* p){ static_cast<FD*>(p)->~FD(); };
        return t;
    }

    void operator()() noexcept { if (invoke_fn) invoke_fn(storage); }
    ~Task()                    { if (destroy_fn) destroy_fn(storage); }

    Task() = default;
    Task(const Task&)            = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& o) noexcept
        : invoke_fn(o.invoke_fn), destroy_fn(o.destroy_fn) {
        memcpy(storage, o.storage, STORAGE);
        o.invoke_fn = o.destroy_fn = nullptr;
    }
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (destroy_fn) destroy_fn(storage);
            invoke_fn = o.invoke_fn; destroy_fn = o.destroy_fn;
            memcpy(storage, o.storage, STORAGE);
            o.invoke_fn = o.destroy_fn = nullptr;
        }
        return *this;
    }
};

// Lock-free MPMC ring buffer (D. Vyukov algorithm).
template<typename T, int N>
class MPMCQueue {
    static_assert((N & (N - 1)) == 0 && N >= 2, "N must be power of 2 >= 2");

    alignas(64) std::atomic<size_t> seqs[N]; // sequence per slot
    alignas(64) T                   data[N]; // task payloads (cold)
    alignas(64) std::atomic<size_t> enq{0};  // producer cursor
    alignas(64) std::atomic<size_t> deq{0};  // consumer cursor

public:
    MPMCQueue() noexcept {
        for (int i = 0; i < N; ++i)
            seqs[i].store(i, std::memory_order_relaxed);
    }
    MPMCQueue(const MPMCQueue&)            = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;

    // enqueue: false = queue full
    bool enqueue(T&& val) noexcept {
        size_t pos = enq.load(std::memory_order_relaxed);
        for (;;) {
            size_t   seq  = seqs[pos & (N-1)].load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)pos;
            if (diff == 0) {
                if (enq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;          // full
            } else {
                pos = enq.load(std::memory_order_relaxed);
            }
        }
        data[pos & (N-1)] = std::move(val);
        seqs[pos & (N-1)].store(pos + 1, std::memory_order_release);
        return true;
    }

    // dequeue: false = queue empty
    bool dequeue(T& val) noexcept {
        size_t pos = deq.load(std::memory_order_relaxed);
        for (;;) {
            size_t   seq  = seqs[pos & (N-1)].load(std::memory_order_acquire);
            intptr_t diff = (intptr_t)seq - (intptr_t)(pos + 1);
            if (diff == 0) {
                if (deq.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    break;
            } else if (diff < 0) {
                return false;          // empty
            } else {
                pos = deq.load(std::memory_order_relaxed);
            }
        }
        val = std::move(data[pos & (N-1)]);
        seqs[pos & (N-1)].store(pos + N, std::memory_order_release);
        return true;
    }
};

// Async logger: MPSC ring buffer + dedicated I/O thread.
class AsyncLogger {
    static constexpr int N = 256;

    struct alignas(64) Slot {
        std::atomic<uint8_t> ready{0};
        char    buf[118];
        uint8_t len = 0;
    };

    alignas(64) std::atomic<uint32_t> push_pos{0};
    Slot             ring[N];

    std::thread      io_thread;
    std::atomic<bool> running{true};
    uint32_t         pop_pos{0};

public:
    AsyncLogger() {
        io_thread = std::thread([this]() {
            while (true) {
                Slot& s = ring[pop_pos & (N - 1)];
                if (s.ready.load(std::memory_order_acquire)) {
                    fwrite(s.buf, 1, s.len, stdout);
                    s.ready.store(0, std::memory_order_release);
                    ++pop_pos;
                } else {
                    if (!running.load(std::memory_order_relaxed)) break;
                    std::this_thread::yield();
                }
            }
            while (ring[pop_pos & (N-1)].ready.load(std::memory_order_acquire)) {
                Slot& s = ring[pop_pos & (N-1)];
                fwrite(s.buf, 1, s.len, stdout);
                s.ready.store(0, std::memory_order_release);
                ++pop_pos;
            }
            fflush(stdout);
        });
    }

    ~AsyncLogger() {
        running.store(false, std::memory_order_relaxed);
        io_thread.join();
    }

    void push(const char* msg, int len) noexcept {
        uint32_t p = push_pos.fetch_add(1, std::memory_order_acq_rel);
        Slot& s = ring[p & (N - 1)];
        while (s.ready.load(std::memory_order_acquire)) std::this_thread::yield();
        int copy = (len > 117) ? 117 : len;
        memcpy(s.buf, msg, copy);
        s.len = static_cast<uint8_t>(copy);
        s.ready.store(1, std::memory_order_release);
    }

    template<typename... Args>
    void pushf(const char* fmt, Args... args) noexcept {
        char buf[128];
        int  n = snprintf(buf, sizeof(buf), fmt, args...);
        if (n > 0) push(buf, n);
    }

    AsyncLogger(const AsyncLogger&)            = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
};

// Thread pool built on top of MPMCQueue + spinning workers.
class ThreadPool {
    static constexpr int QUEUE_CAP  = 64;   // power of 2
    static constexpr int SPIN_LIMIT = 2000;

    using Queue = MPMCQueue<Task, QUEUE_CAP>;

    Queue             queue;
    std::thread       workers[10];
    std::atomic<bool> stop{false};

    void worker_fn() {
        Task task;
        int  spin = 0;
        for (;;) {
            if (queue.dequeue(task)) {
                task();
                spin = 0;
                continue;
            }
            if (stop.load(std::memory_order_acquire)) {
                while (queue.dequeue(task)) task(); // drain before exit
                return;
            }
            if (++spin > SPIN_LIMIT) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                spin = 0;
            }
        }
    }

public:
    static constexpr int POOL_SIZE = 10;

    ThreadPool() {
        for (int i = 0; i < POOL_SIZE; ++i)
            workers[i] = std::thread(&ThreadPool::worker_fn, this);
    }
    ~ThreadPool() {
        stop.store(true, std::memory_order_release);
        for (auto& w : workers) w.join();
    }

    template<typename F>
    void submit(F&& fn) noexcept {
        Task t = Task::make(std::forward<F>(fn));
        while (!queue.enqueue(std::move(t))) std::this_thread::yield();
    }

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
};

// Flat open-addressing hash map with fixed capacity.
template<typename K, typename V, typename H, int CAP = 512>
class FlatHashMap {
    static_assert((CAP & (CAP - 1)) == 0 && CAP >= 2, "CAP must be power of 2 >= 2");

    struct Entry {
        K    key   = {};
        V    val   = {};
        bool used  = false;
    };

    Entry entries[CAP] = {};
    Entry overflow_slot = {};  // isolated fallback; returned on full-capacity insert
    int   used_count    = 0;   // number of distinct keys stored

public:
    int  size()       const noexcept { return used_count; }
    bool overloaded() const noexcept { return used_count * 2 >= CAP; } // load > 50%

    // find: 없으면 nullptr
    V* find(const K& k) noexcept {
        size_t h = H{}(k) & (CAP - 1);
        for (int i = 0; i < CAP; ++i) {
            size_t idx = (h + i) & (CAP - 1);
            if (!entries[idx].used)       return nullptr;
            if (entries[idx].key == k)    return &entries[idx].val;
        }
        return nullptr;
    }

    // operator[]: 없으면 기본값으로 삽입 후 반환
    V& operator[](const K& k) noexcept {
        size_t h = H{}(k) & (CAP - 1);
        for (int i = 0; i < CAP; ++i) {
            size_t idx = (h + i) & (CAP - 1);
            if (!entries[idx].used) {
                entries[idx].key  = k;
                entries[idx].used = true;
                ++used_count;
                return entries[idx].val;
            }
            if (entries[idx].key == k) return entries[idx].val;
        }
        // Map full: return isolated overflow_slot to avoid corrupting entries[0].
        overflow_slot = {};
        return overflow_slot.val;
    }
};

struct PosKey {
    char buf[24] = {};
    bool operator==(const PosKey& o) const noexcept {
        return memcmp(buf, o.buf, sizeof(buf)) == 0;
    }
};

struct PosKeyHash {
    size_t operator()(const PosKey& k) const noexcept {
        size_t h = 14695981039346656037ULL; // FNV-1a
        for (const char c : k.buf) {
            if (!c) break;
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        return h;
    }
};

// Key format: "exchange_id_inst_type_symbol", e.g. "0_1_ETH-PERP"
inline PosKey makePosKey(int exchange_id, int inst_type, const char* symbol) noexcept {
    PosKey key{};
    char*  p   = key.buf;
    char*  end = key.buf + sizeof(key.buf) - 1;
    auto [p1, ec1] = std::to_chars(p, end, exchange_id);
    p = p1; *p++ = '_';
    auto [p2, ec2] = std::to_chars(p, end, inst_type);
    p = p2; *p++ = '_';
    const char* s = symbol;
    while (*s && p < end) *p++ = *s++;
    return key;
}

struct OrderRequest {
    int     order_id;
    int     exchange_id;
    int     inst_type;          // 0=SPOT, 1=FUTURES, 2=OPTIONS
    int     leverage;           // margin multiplier supplied by alpha model
    char    symbol[16];
    int     side;               // 0=BUY, 1=SELL
    int     max_signal_age_ms;  // 0=skip; >0=reject if (now - timestamp_ms) > this
    int64_t price_ticks;
    int64_t qty_ticks;
    int64_t timestamp_ms;
    // sizeof = 64B (max_signal_age_ms fills former 4-byte padding gap)
};

struct Position { int64_t qty_ticks = 0; };

enum class Verdict : uint8_t { OK, REJECT_CASH, REJECT_OVERSELL, STALE };

// Gateways (no virtual dispatch; std::variant based).
class BinanceGateway {
    char api_key[64];
    char secret_key[64];
public:
    BinanceGateway(const char* key, const char* secret);
    void sendOrderAsync(const OrderRequest& order, ThreadPool& pool);
};

class HyperliquidGateway {
    char wallet_address[64];
    char private_key[128];
public:
    HyperliquidGateway(const char* wallet, const char* pkey);
    void sendOrderAsync(const OrderRequest& order, ThreadPool& pool);
};

using AnyGateway = std::variant<BinanceGateway, HyperliquidGateway>;

class OrderExecutor {
    static constexpr int64_t SCALE        = 100000000LL;
    static constexpr int     MAX_EXCHANGES = 8;

    // hot path members
    ThreadPool pool;
    SpinLock   spin;
    int64_t    cash_ticks;
    int64_t    unrealized_profit_ticks{0}; // set by position calculator; not used for margin

    // position book (fixed-capacity hash map)
    FlatHashMap<PosKey, Position, PosKeyHash, 512> positions;

    std::atomic<bool> load_warn_fired{false};

    // exchange_id-based routing table
    std::optional<AnyGateway> gateways[MAX_EXCHANGES];

    AsyncLogger logger;

    // now_ms passed in from receiveOrder (captured before spinlock to minimize hold time)
    Verdict validateOrder(const OrderRequest& order, int64_t now_ms) noexcept;

public:
    explicit OrderExecutor(int64_t initial_cash_ticks);

    template<typename G>
    void registerGateway(int exchange_id, G&& gw) {
        if (exchange_id < 0 || exchange_id >= MAX_EXCHANGES) return;
        gateways[exchange_id].emplace(std::forward<G>(gw));
        logger.pushf("[System] Gateway registered for Exchange ID: %d\n", exchange_id);
    }

    // Called by the position calculator when unrealized P&L changes.
    void updateUnrealizedProfit(int64_t profit_ticks) noexcept;

    void receiveOrder(const OrderRequest& order);
    void onOrderExecuted(const OrderRequest& order);
};
