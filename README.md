# Order Execution Module

C++17로 작성된 저지연(low-latency) 암호화폐 주문 체결 모듈.  
알파 모델이 계산한 포지션을 받아 CEX/DEX 게이트웨이로 주문을 발송합니다.  
핫 패스(주문 수신 → 게이트웨이 디스패치)가 **마이크로초 단위**로 완료되어 알파 신호 감쇠를 방지합니다.

---

## 목차

1. [전체 시스템 연동 구조](#1-전체-시스템-연동-구조)
2. [공개 API 명세](#2-공개-api-명세)
3. [입력 계약 — OrderRequest](#3-입력-계약--orderrequest)
4. [반환값 및 부수효과](#4-반환값-및-부수효과)
5. [포트폴리오 상태 관리](#5-포트폴리오-상태-관리)
6. [모듈 내부 아키텍처](#6-모듈-내부-아키텍처)
7. [최적화 기법](#7-최적화-기법)
8. [테스트 케이스](#8-테스트-케이스)
9. [빌드 및 실행 방법](#9-빌드-및-실행-방법)
10. [성능 특성](#10-성능-특성)
11. [알려진 한계 및 프로덕션 갭](#11-알려진-한계-및-프로덕션-갭)

---

## 1. 전체 시스템 연동 구조

이 모듈은 시스템의 **실행 레이어(Execution Layer)** 에만 집중합니다.  
알파 생성, 리스크 관리, 포트폴리오 계산은 이 모듈의 상위/하위 시스템이 담당합니다.

```
┌─────────────────────────────────────────────────────────────────┐
│                         상위 시스템                              │
│                                                                 │
│  ┌─────────────┐    ┌──────────────────┐    ┌────────────────┐ │
│  │  Alpha Model │    │  Risk System     │    │  Portfolio     │ │
│  │  (신호 생성) │    │  (한도/드로우다운)│    │  Calculator    │ │
│  └──────┬──────┘    └────────┬─────────┘    └───────┬────────┘ │
│         │ OrderRequest       │ (리스크 통과 후        │          │
│         │ (입력 구조체)       │  receiveOrder 호출)   │          │
│         │                   │                       │          │
└─────────┼───────────────────┼───────────────────────┼──────────┘
          │                   │          updateUnrealizedProfit()│
          ▼                   ▼                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                   [이 모듈] OrderExecutor                        │
│                                                                 │
│   receiveOrder(OrderRequest)  →  검증 → 마진 차감 → 디스패치    │
│   onOrderExecuted(OrderRequest) →  포지션 업데이트 → 현금 정산  │
│                                                                 │
└──────────────────┬──────────────────┬───────────────────────────┘
                   │                  │
          ┌────────▼──────┐  ┌────────▼────────┐
          │ CEX Gateway   │  │ DEX Gateway     │
          │ (Binance REST)│  │ (Hyperliquid    │
          │               │  │  EIP-712 + RPC) │
          └───────────────┘  └─────────────────┘
```

### 연동 시 각 모듈의 역할

| 모듈 | 이 모듈에 해야 할 일 | 이 모듈이 해주는 일 |
|---|---|---|
| **Alpha Model** | `OrderRequest`를 채워서 `receiveOrder()` 호출 | 현금 검사, 스테일 신호 거부, 마진 차감 후 게이트웨이 발송 |
| **Risk System** | 리스크 한도 검사 후 통과한 주문만 넘길 것 | 별도 리스크 로직 없음 — 넘어온 주문을 그대로 신뢰 |
| **Portfolio Calculator** | 미실현 손익 변경 시 `updateUnrealizedProfit()` 호출 | 미실현 손익을 로그에 표시 (마진 계산에는 미사용) |
| **Exchange (체결 알림)** | 체결 완료 시 `onOrderExecuted()` 호출 | 포지션 장부 업데이트, SPOT SELL은 현금 반환 |
| **Frontend / Dashboard** | 현재 로그 스트림 또는 콜백 구독 (미구현) | `AsyncLogger`가 `stdout`으로 로그 출력 |

---

## 2. 공개 API 명세

### `OrderExecutor(int64_t initial_cash_ticks)`

```cpp
OrderExecutor executor(300000LL * SCALE); // 300,000 USDT 초기 현금
```

- **입력**: 초기 현금 (단위: ticks = 실제금액 × 10⁸)
- **동작**: 스레드풀(10개 워커), 비동기 로거, 포지션 해시맵 초기화
- **주의**: 생성 직후 게이트웨이 등록 전에 `receiveOrder` 호출 시 `[ERROR] Unknown Exchange ID` 처리

---

### `registerGateway(int exchange_id, G&& gw)`

```cpp
executor.registerGateway(0, BinanceGateway("API_KEY", "SECRET"));
executor.registerGateway(1, HyperliquidGateway("0xWALLET", "PRIV_KEY"));
```

- **입력**: `exchange_id` (0~7), 게이트웨이 객체 (`BinanceGateway` 또는 `HyperliquidGateway`)
- **반환값**: 없음 (void)
- **부수효과**: 내부 라우팅 테이블 등록, 로그 출력
- **주의**: `exchange_id`는 `OrderRequest.exchange_id`와 반드시 일치해야 함

---

### `receiveOrder(const OrderRequest& order)` ← 핵심 진입점

```cpp
executor.receiveOrder(order); // 알파 모델이 신호를 보낼 때 호출
```

- **입력**: `OrderRequest` 구조체 (하단 §3 참조)
- **반환값**: 없음 (void)
- **내부 처리 순서**:
  1. 스테일 신호 검사 (`now_ms - timestamp_ms > max_signal_age_ms` → `STALE`)
  2. 현금/포지션 검증 (`REJECT_CASH` 또는 `REJECT_OVERSELL`)
  3. 마진 차감 (BUY 전액, FUTURES/OPTIONS SELL 초기증거금)
  4. 스레드풀에 게이트웨이 디스패치 태스크 제출 후 **즉시 반환**
- **부수효과**:
  - 성공: `cash_ticks` 감소, 게이트웨이 비동기 발송
  - 실패: 로그 출력, `cash_ticks` 변경 없음
- **반환 타이밍**: SpinLock 해제 직후 (게이트웨이 I/O 완료를 기다리지 않음, ~수 µs)
- **스레드 안전**: ✅ 여러 스레드에서 동시 호출 가능

---

### `onOrderExecuted(const OrderRequest& order)` ← 체결 콜백

```cpp
executor.onOrderExecuted(filled_order); // 거래소에서 체결 확인이 왔을 때 호출
```

- **입력**: 체결된 `OrderRequest` (원본 주문과 동일 구조체 재사용)
- **반환값**: 없음 (void)
- **부수효과**:
  - BUY 체결: 포지션 `qty_ticks` 증가
  - SPOT SELL 체결: 포지션 감소 + `cash_ticks` 복원 (전체 매도 대금)
  - FUTURES/OPTIONS SELL 체결: 포지션만 감소 (현금 변동 없음, P&L은 외부 시스템 담당)
- **스레드 안전**: ✅
- **주의**: 현재 버전은 100% 체결을 가정. 부분 체결 시 `qty_ticks`를 실제 체결량으로 수정 후 호출할 것

---

### `updateUnrealizedProfit(int64_t profit_ticks)`

```cpp
executor.updateUnrealizedProfit(20000LL * SCALE); // 미실현 손익 2만 USDT
```

- **입력**: 현재 미실현 손익 (단위: ticks)
- **반환값**: 없음 (void)
- **부수효과**: 내부 `unrealized_profit_ticks` 갱신 (마진 계산에는 미사용, 로그 표시 전용)
- **호출 주체**: Portfolio Calculator
- **스레드 안전**: ✅

---

## 3. 입력 계약 — OrderRequest

알파 모델이 이 구조체를 채워서 `receiveOrder()`에 전달합니다.  
**이 모듈은 입력된 모든 값을 신뢰합니다. 비즈니스 로직 검증은 호출 측 책임입니다.**

```cpp
struct OrderRequest {
    int     order_id;            // 주문 고유 ID (중복 방지는 호출 측 책임)
    int     exchange_id;         // 게이트웨이 라우팅 키 (registerGateway와 동일 ID)
    int     inst_type;           // 0=SPOT, 1=FUTURES, 2=OPTIONS
    int     leverage;            // 레버리지 배율 (SPOT=1, FUTURES=알파모델이 결정)
    char    symbol[16];          // 심볼 문자열 (예: "BTCUSDT", "ETH-PERP")
    int     side;                // 0=BUY, 1=SELL
    int     max_signal_age_ms;   // 0=검사 안 함, >0=이 시간(ms) 초과 시 STALE 폐기
    int64_t price_ticks;         // 가격 × SCALE (SCALE = 100,000,000)
    int64_t qty_ticks;           // 수량 × SCALE
    int64_t timestamp_ms;        // 알파 신호가 생성된 Unix 타임스탬프 (밀리초)
};
// sizeof(OrderRequest) = 64 bytes
```

**SCALE 변환 예시:**
```
// 가격 70,000 USDT, 수량 1 BTC
order.price_ticks = 70000LL * 100000000LL;  // = 7,000,000,000,000
order.qty_ticks   = 1LL    * 100000000LL;  // = 100,000,000
```

**마진 계산 공식:**
```
notional = (price_ticks / SCALE) × qty_ticks
margin   = notional / leverage
```

| inst_type | leverage | 마진 |
|---|---|---|
| SPOT (0) | 반드시 1 | 전액 notional |
| FUTURES (1) | 알파모델 결정 (예: 10) | notional / 10 |
| OPTIONS (2) | 알파모델 결정 | notional / leverage |

---

## 4. 반환값 및 부수효과

`receiveOrder()`와 `onOrderExecuted()`는 모두 `void`를 반환합니다.  
처리 결과는 `AsyncLogger`를 통해 `stdout`으로 출력됩니다.

### 내부 Verdict (로그로만 노출)

| Verdict | 로그 접두어 | 의미 | 현금 변화 |
|---|---|---|---|
| `OK` | `[CEX-Binance]` 또는 `[DEX-Hyperliquid]` | 정상 발송 | 마진만큼 차감 |
| `REJECT_CASH` | `[Reject] REJECT_CASH.` | 가용 현금 부족 | 변화 없음 |
| `REJECT_OVERSELL` | `[CRITICAL REJECT] SPOT SELL` | SPOT 보유 포지션 없음 | 변화 없음 |
| `STALE` | `[Stale] Signal expired.` | 신호 유효시간 초과 | 변화 없음 |

> ⚠️ **현재 버전은 Verdict를 호출 측으로 반환하지 않습니다.**  
> 알파 모델이 거부 여부를 알려면 현재 로그 파싱이 필요합니다.  
> 프로덕션에서는 `receiveOrder()`의 반환 타입을 `Verdict`로 변경하거나  
> 콜백(`std::function<void(int order_id, Verdict)>`)을 추가하는 것을 권장합니다.

### 로그 출력 예시

```
[System] ThreadPool ready. 10 spinning workers, lock-free queue (cap=64).
[System] Executor Ready. Cash: 30000000000000 ticks
[System] Gateway registered for Exchange ID: 0
[System] Gateway registered for Exchange ID: 1
[CEX-Binance] Order Dispatched. ID: 101 (Latency: ~50ms)
[DEX-Hyperliquid] Order Confirmed on-chain. ID: 601 (Latency: ~1050ms)
[Reject] REJECT_CASH. Order 401 | required=150000 | cash=100000 | unrealized=20000
[CRITICAL REJECT] SPOT SELL with no position. Order: 302, Symbol: ETHUSDT
[Stale] Signal expired. Order 501 | age=2003ms > limit=500ms
[System] BUY Filled.  +10 [0_0_ETHUSDT]
[WARN] FlatHashMap load > 50% (256 / 512 slots). Consider increasing CAP.
```

---

## 5. 포트폴리오 상태 관리

```
전체 포트폴리오 = cash + unrealized_profit + realized_profit
```

이 모듈이 관리하는 상태는 **cash**와 **포지션 장부**뿐입니다.

| 상태 | 관리 주체 | 변경 시점 |
|---|---|---|
| `cash_ticks` | 이 모듈 | BUY 발송 시 차감, SPOT SELL 체결 시 복원 |
| 포지션 장부 (`FlatHashMap`) | 이 모듈 | `onOrderExecuted()` 호출 시 |
| `unrealized_profit_ticks` | 외부 Portfolio Calculator가 주입 | `updateUnrealizedProfit()` 호출 시 |
| realized_profit | **이 모듈 범위 밖** | 외부 리스크/포트폴리오 시스템 담당 |

**중요 원칙: 미실현 손익은 마진으로 사용되지 않습니다.**  
시장 변동으로 즉시 증발할 수 있기 때문에, `cash_ticks`만이 주문 가능 재원입니다.

**포지션 키 형식:**
```
"{exchange_id}_{inst_type}_{symbol}"
예: "0_0_ETHUSDT"   (Binance, SPOT, ETH)
    "1_1_BTCUSDT"   (Hyperliquid, FUTURES, BTC)
```

---

## 6. 모듈 내부 아키텍처

```
receiveOrder(OrderRequest)
       │
       │  ① get_now_ms() — SpinLock 진입 전 시간 캡처
       ▼
┌──────────────────────────────────┐
│   SpinLock  (~30ns 유지)         │
│   ① 스테일 신호 검사              │
│   ② validateOrder()              │
│      - SPOT SELL: 포지션 장부 확인│
│      - 그 외: 현금 잔고 확인      │
│   ③ 마진 차감 (OK인 경우만)       │
│   ④ gateway ID 조회              │
└────────────┬─────────────────────┘
             │ SpinLock 해제 (I/O 시작 전)
             ▼
   std::visit(AnyGateway)  ← 가상함수 없음, 컴파일타임 디스패치
             │
             ▼
   ThreadPool::submit(Task)  ← 힙 할당 없는 고정 크기 Task
             │
             ▼
   MPMCQueue<Task, 64>  ← 락프리 링버퍼
             │
             ▼
   ThreadPool 워커 (10개 스피닝)  → 실제 게이트웨이 I/O

onOrderExecuted(OrderRequest)
       │
       ▼
   SpinLock → FlatHashMap 업데이트 → SPOT SELL이면 cash 복원
```

---

## 7. 최적화 기법

### 1. 스레드풀 — 스레드 생성 비용 제거
**문제:** 주문마다 `std::thread` 생성 → 수 ms 오버헤드  
**해결:** 시작 시 10개 워커 스레드를 미리 생성. 주문은 큐에 태스크만 밀어 넣음

### 2. SpinLock — OS 슬립 없는 잠금
**문제:** `std::mutex`는 컨텍스트 스위칭으로 1~10µs 지연  
**해결:** `std::atomic_flag` 기반 SpinLock. ~30ns 임계구역에서는 스피닝이 더 빠름.  
SpinLock 범위를 의도적으로 좁혀서 **게이트웨이 호출 전에 해제**

### 3. 락프리 MPMC 큐 — 스레드풀 병목 제거
**문제:** 뮤텍스 보호 `std::queue`는 공유 락 병목  
**해결:** Dmitry Vyukov MPMC 링버퍼. 생산자/소비자가 CAS로 독립적으로 커서 전진

### 4. 힙프리 Task — 핫패스 malloc 제거
**문제:** `std::function`이 캡처 크기 초과 시 힙 할당 (`OrderRequest`=64B → 항상 초과)  
**해결:** `alignas(64)` 128바이트 인라인 스토리지를 가진 커스텀 `Task` 구조체. 플레이스먼트-new 사용

### 5. 비동기 로거 — I/O가 핫패스 차단 없음
**문제:** `std::cout`은 내부 뮤텍스 + 시스템 콜  
**해결:** MPSC 링버퍼 (`AsyncLogger`). `pushf()`는 atomic fetch-add + memcpy만 수행.  
전용 I/O 스레드가 비동기로 `fwrite()` 처리

### 6. FlatHashMap — 캐시 친화적 포지션 장부
**문제:** `std::unordered_map`은 체이닝으로 포인터 체이싱 발생  
**해결:** 선형 프로빙 오픈 어드레싱. 512슬롯 전체가 L2 캐시에 적재.  
컴파일타임 고정 용량 — 재해싱, 힙 할당 없음

### 7. std::variant 정적 디스패치 — vtable 오버헤드 없음
**문제:** `virtual sendOrderAsync()` → 간접 분기 1회  
**해결:** `std::variant<BinanceGateway, HyperliquidGateway>` + `std::visit`.  
컴파일러가 컴파일타임에 점프 테이블 생성

### 8. 스택 할당 키 — 문자열 힙 할당 없음
**문제:** `"0_1_ETHUSDT"` 같은 포지션 키를 `std::string`으로 만들면 매 조회마다 malloc  
**해결:** `char[24]` 스택 버퍼 `PosKey`. `makePosKey()`는 `std::to_chars`로 ~5ns

### 9. 오버플로우 안전 Notional 계산
**문제:** `price_ticks × qty_ticks`는 `int64_t` 최대값(9.2×10¹⁸) 초과  
**해결:** `(price_ticks / SCALE) × qty_ticks` — 나눗셈 먼저, 오버플로우 없음

### 10. Instrument별 SELL 검증
**문제:** 모든 SELL에 재고 확인을 적용하면 FUTURES 공매도가 잘못 거부됨  
**해결:**
- **SPOT**: 보유 포지션 확인 → 없으면 `REJECT_OVERSELL`
- **FUTURES/OPTIONS**: 현금 마진 확인만 → 공매도 허용

### 11. 제로할당 페이로드 직렬화 (프로덕션 대비)
게이트웨이를 실제 REST/WebSocket으로 교체 시, `std::string` 대신 `char[512]` 스택 버퍼에  
`std::to_chars` + 수동 문자열 복사로 JSON 페이로드를 직렬화해야 힙 할당 없는 엔드투엔드가 유지됩니다.

---

## 8. 테스트 케이스

### TC1. 마이크로초 단위 연속 주문 (Zero-Blocking)
**상황:** 심볼 A 포지션 계산 직후, 다음 틱에 심볼 B 매수 주문 수신  
**검증:** `receiveOrder()` 2회 호출 각각의 wall-clock 시간이 수 µs 이내  
**의미:** 게이트웨이 I/O(50ms)를 기다리지 않고 즉시 반환

### TC2. 3심볼 동시 주문 (SpinLock 경합)
**상황:** BTC, ETH, ADA에 동시에 매수 신호  
**검증:** 3개 스레드 동시 `receiveOrder()` 호출 후 주문 3개 모두 정상 발송  
**의미:** SpinLock이 장부 업데이트를 직렬화하여 데이터 오염 없음

### TC3. 미체결 상태 SPOT 매도 방어 (REJECT_OVERSELL)
**상황:** SPOT ETH 매수 발송 → 체결 미확인 → 즉시 ETH 매도 신호 도착  
**검증:** `REJECT_OVERSELL` 반환, `onOrderExecuted()` 후에는 정상 매도 가능  
**의미:** 포지션 없이 SPOT 공매도 불가 방어

### TC4. 미실현 손익 포함 현금 검증 (REJECT_CASH)
**상황:** cash=100,000, unrealized=20,000, 주문금액=150,000  
**검증:** `REJECT_CASH` (120,000 포트폴리오도 cash 100,000 초과 주문은 거부)  
**의미:** 미실현 손익은 마진 재원으로 인정하지 않음

### TC5. 스테일 신호 폐기 (STALE)
**상황:** 알파 신호 생성 후 2,000ms 지연 수신 (허용 한도: 500ms)  
**검증:** `STALE` 처리, 게이트웨이 미발송  
**의미:** 시장 가격이 이미 변한 신호는 자동 폐기

### TC6. CEX/DEX 차익거래 (양방향 동시 발송)
**상황:** Hyperliquid BTC-PERP 70,000 / Binance BTC-PERP 70,050 (스프레드 50 USDT)  
**검증:** DEX BUY + CEX SELL 두 레그가 수 µs 내 동시 디스패치  
```
[TC6-Timing] Both legs submitted in Xus total.
[CEX-Binance]      Order Dispatched. ID: 602 (Latency: ~50ms)
[DEX-Hyperliquid]  Order Confirmed on-chain. ID: 601 (Latency: ~1050ms)
```
**의미:** DEX의 1초+ 블록 컨펌이 CEX 레그를 블로킹하지 않음 (완전 비동기)

---

## 9. 빌드 및 실행 방법

### 사전 요구사항

| 도구 | 최소 버전 |
|---|---|
| CMake | 3.16 |
| C++ 컴파일러 | GCC 9 / Clang 10 / MSVC 2019 (C++17) |

---

### Linux / macOS

```bash
cd order_execute_module

cmake -S . -B out -DCMAKE_BUILD_TYPE=Release
cmake --build out -j$(nproc)

./out/oe_runner
```

**AddressSanitizer (메모리 오류 검사):**
```bash
cmake -S . -B out_asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build out_asan -j$(nproc)
./out_asan/oe_runner
```

**ThreadSanitizer (데이터 레이스 검사):**
```bash
cmake -S . -B out_tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON
cmake --build out_tsan -j$(nproc)
./out_tsan/oe_runner
```

---

### Windows — Git Bash + MSYS2 (권장)

MSYS2(`C:\msys64`)가 설치되어 있어야 합니다.  
Git Bash를 새로 열면 `~/.bashrc`의 MSYS2 PATH가 자동으로 적용됩니다.

```bash
cd /c/kfac/order_execute_module

cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/c/msys64/mingw64/bin/g++.exe \
  -DCMAKE_MAKE_PROGRAM=/c/msys64/mingw64/bin/ninja.exe

cmake --build out

./out/oe_runner.exe
```

**MSYS2 GCC 초기 설치 (한 번만):**
```bash
# MSYS2 터미널에서
pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-ninja mingw-w64-x86_64-cmake
```

---

### Windows — MSVC

```powershell
cmake -S . -B out -G "Visual Studio 17 2022" -A x64
cmake --build out --config Release
.\out\Release\oe_runner.exe
```

---

## 10. 성능 특성

| 경로 | 예상 지연시간 |
|---|---|
| `receiveOrder()` 핫패스 (검증 + 디스패치) | < 1 µs (I/O 없음, 힙 할당 없음) |
| SpinLock 유지 시간 (임계구역) | ~30 ns |
| `AsyncLogger::pushf()` | ~20 ns (atomic fetch-add + memcpy) |
| `FlatHashMap::find()` / `operator[]` | ~5–15 ns (L1/L2 캐시 히트) |
| `makePosKey()` | ~5 ns (`std::to_chars` + 수동 복사) |
| 스레드풀 태스크 제출 (`MPMCQueue::enqueue`) | ~10 ns (CAS 1회) |
| 게이트웨이 I/O — Binance (CEX) | ~50 ms (비동기, 핫패스 외부) |
| 게이트웨이 I/O — Hyperliquid (DEX) | ~1,050 ms (비동기, 핫패스 외부) |

핫패스에서 배제된 것들: `malloc`, `free`, `mutex`, `std::string`, 가상 디스패치, 동기 I/O

---

## 11. 알려진 한계 및 프로덕션 갭

현재 구현과 프로덕션 모듈의 차이입니다. 버그가 아닌 의도된 범위 제한입니다.

### 🔴 Critical — 실거래 전 반드시 해결

| 항목 | 현재 상태 | 프로덕션에서 필요한 것 |
|---|---|---|
| **실제 네트워크 코드 없음** | `sleep_for`로 레이턴시 시뮬레이션 | Binance: HMAC-SHA256 서명 REST. Hyperliquid: EIP-712 서명 + JSON-RPC |
| **Verdict 반환 없음** | `receiveOrder()`가 `void` 반환 | 알파 모델이 거부 여부를 알 수 있도록 `Verdict` 반환 또는 콜백 추가 |
| **order_type 필드 없음** | 모든 주문을 지정가(LIMIT) 처리 | `LIMIT`, `MARKET`, `IOC`, `FOK` 구분 필요. HFT에서는 IOC가 표준 |
| **Time-in-Force 없음** | 주문이 취소될 때까지 유효 | `GTC`, `IOC`, `FOK`. 미체결 주문 쌓임 방지를 위해 IOC 권장 |
| **부분 체결 미지원** | `onOrderExecuted()`가 100% 체결 가정 | 실제 체결량(`fill_qty`)과 체결 가격(`fill_price`)을 인수로 받아야 함 |
| **주문 취소 기능 없음** | `cancelOrder()` 미구현 | 시장 상황 변화 시 미체결 주문 취소 API 필요 |

### 🟡 Significant — 파생상품 거래 시 필요

| 항목 | 현재 상태 | 프로덕션에서 필요한 것 |
|---|---|---|
| **FUTURES P&L 정산 없음** | 공매도 진입 후 마진이 영구 차감 | 포지션 청산 시 마진 + 실현 손익 반환 로직 |
| **OPTIONS 증거금 모델 없음** | FUTURES와 동일하게 처리 | 비선형 손익 구조 반영한 SPAN 증거금 또는 델타 기반 모델 |
| **reduce_only 플래그 없음** | 포지션 청산도 신규 진입과 동일 마진 차감 | 청산 주문은 마진 재차감 없이 처리 |

### 🟢 Minor — 안정성

| 항목 | 현재 상태 | 프로덕션에서 필요한 것 |
|---|---|---|
| **중복 주문 ID 방어 없음** | 동일 `order_id` 재처리 시 마진 이중 차감 | atomic 처리된 seen-ID 비트셋 또는 세대 카운터 |
| **포지션 한도 없음** | 심볼당 포지션 크기 무제한 | `validateOrder()`에서 최대 notional 또는 포지션 크기 검사 |
| **게이트웨이 최대 8개 고정** | `MAX_EXCHANGES = 8` 컴파일타임 상수 | 설정 파일 기반 런타임 라우팅 테이블 |
| **재연결/페일오버 없음** | 거래소 ID당 게이트웨이 1개 | 하트비트 모니터링, 자동 재연결, 이중화 게이트웨이 |

### ⚪ 설계 범위 밖 (의도적 제외)

아래 항목은 이 모듈의 범위가 아니며 상위/하위 시스템에 속합니다:

- **알파 신호 생성** — 이 모듈은 순수 실행자. 신호 로직은 알파 모델 담당
- **실현 손익 추적** — 포트폴리오/리스크 레이어에서 계산 후 `updateUnrealizedProfit()`으로 주입
- **리스크 한도 (드로우다운, VaR)** — 이 모듈에 신호를 전달하기 전에 리스크 시스템이 처리
- **오더북/시장 데이터** — 이 모듈에서 소비하지 않음. 가격 정보는 알파 모델이 `OrderRequest`에 담아 전달

---

## 파일 구조

```
order_execute_module/
├── CMakeLists.txt          # 빌드 시스템
├── README.md               # 이 파일
├── OrderExecutor.h         # 모든 핵심 자료구조 및 클래스 선언
├── OrderExecutor.cpp       # OrderExecutor, 게이트웨이 구현체
├── DummyOrderGenerator.h   # 테스트 픽스처 팩토리 (TC1~TC6)
└── main.cpp                # 테스트 진입점
```
