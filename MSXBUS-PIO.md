# MSXBUS-PIO: RP2350 기반 실물 MSX 카트리지 버스 인터페이스 구현 가이드

이 문서는 **Raspberry Pi Pico 2 (RP2350)** 및 **Waveshare RP2350-PiZero** 보드에서 실물 MSX 카트리지 슬롯을 고속으로 직접 제어하기 위해 구현된 `msxbus` 드라이버와 **PIO(Programmable I/O)** 및 **SIO(Single-cycle I/O)** 가속 구조에 대한 기술 문서입니다.

기존 Raspberry Pi Zero(BCM2835) 베어메탈(Circle) 기반 `blueberry-msx`의 버스 인터페이스를 RP2350의 마이크로컨트롤러 하드웨어 아키텍처에 맞추어 재설계하였습니다.

---

## 1. 아키텍처 및 설계 목적

### 1.1 배경 및 과제
- MSX 슬롯은 Z80 CPU 클럭(3.58MHz, 1사이클 약 279ns)을 기준으로 작동하며, 메모리/IO 읽기·쓰기 시 엄격한 셋업 타임(Address Setup), 홀드 타임(Hold Time), `/WAIT` 신호 동기화가 요구됩니다.
- RP2350은 최대 252MHz~300MHz의 고속 클럭으로 동작하므로, 지연 없이 단 몇십 나노초(ns) 단위로 주소 래치와 제어 신호를 생성해야 실제 카트리지(ROM 팩, 사운드 팩, 확장 램 등)와 안정적으로 통신할 수 있습니다.
- Pico2MSX에서는 코어 0(Z80 에뮬레이터 및 MSX 메인보드 로직)과 코어 1(DVI 60Hz HDMI 비디오 출력)이 병렬로 실행되므로 버스 충돌 방지와 스핀락 동기화가 필수적입니다.

### 1.2 핵심 구현 요소
1. **PIO (Programmable I/O) 엔진 (`msxbus.pio`)**: 하드웨어 스테이트 머신을 활용하여 주소 래치(MODE1/MODE2) 및 버스 읽기 사이클을 CPU 사이클 낭비 없이 정밀한 타이밍으로 처리.
2. **RP2350 SIO (Single-cycle I/O) 고속 제어**: ARM Cortex-M33 코어 전용 1사이클 하드웨어 I/O 레지스터(`sio_hw->gpio_set/clr/oe`)를 사용하여 나노초 단위의 버스 트랜잭션 수행.
3. **하드웨어 스핀락 동기화**: 코어 간 버스 경합을 하드웨어 락(`hardware/sync.h`)으로 완벽 차단.
4. **2가지 하드웨어 프로파일 지원**: 표준 Blueberry GPIO 40핀 2슬롯 확장 보드 및 Zemmix Mini 래치 보드 지원.

---

## 2. 하드웨어 프로파일 및 핀 매핑 (Pinout)

### 2.1 Profile 1: Blueberry GPIO MODE 보드 (`MsxBusHwGpio`, 기본값)
라즈베리파이 40핀 GPIO 헤더를 통해 74HC574 / 74LS373 래치 IC를 구동하여 2개의 MSX 카트리지 슬롯을 제어합니다.
`MODE0`(GPIO 8, Low bit)과 `MODE1`(GPIO 9, High bit)의 2비트 Side-Set 조합으로 버스 모드를 전환합니다:

| 2비트 값 (`MODE1`, `MODE0`) | GPIO 9, 8 핀 상태 | 동작 설명 |
| :---: | :---: | :--- |
| **`01`** | `GPIO 9 = 0, GPIO 8 = 1` | **하위 주소 (A0 ~ A7)** 래치 |
| **`11`** | `GPIO 9 = 1, GPIO 8 = 1` | **상위 주소 (A8 ~ A15)** 래치 |
| **`10`** | `GPIO 9 = 1, GPIO 8 = 0` | **데이터 버스 (D0 ~ D7)** 활성화 |
| **`00`** | `GPIO 9 = 0, GPIO 8 = 0` | **유휴 상태 (Idle)** |

| GPIO 번호 | 방향 | 핀 이름 | 설명 |
|---|---|---|---|
| **GPIO 0 ~ 7** | 입/출력 | `D0 ~ D7` / `A0 ~ A7` / `A8 ~ A15` | 데이터 및 주소 멀티플렉싱 8비트 버스 (PIO OUT/IN) |
| **GPIO 8** | 출력 | `MODE0` | 모드 제어 하위 비트 (PIO Side-Set bit 0) |
| **GPIO 9** | 출력 | `MODE1` | 모드 제어 상위 비트 (PIO Side-Set bit 1) |
| **GPIO 10** | 출력 | `/MREQ` | 메모리 요청 신호 (Active LOW) |
| **GPIO 11** | 출력 | `/IORQ` | I/O 요청 신호 (Active LOW) |
| **GPIO 12** | 출력 | `/RD` | 읽기 스트로브 신호 (Active LOW) |
| **GPIO 13** | 출력 | `/WR` | 쓰기 스트로브 신호 (Active LOW) |
| **GPIO 14** | 출력 | `/SLTSL1` | 슬롯 1 선택 신호 (Active LOW) |
| **GPIO 15** | 출력 | `/SLTSL2` | 슬롯 2 선택 신호 (Active LOW) |
| **GPIO 16** | 입력 (Pull-Up) | `/WAIT` | 카트리지 대기 요청 신호 (Active LOW) |
| **GPIO 17** | 입력 (Pull-Up) | `/INT` | 카트리지 인터럽트 신호 (Active LOW) |
| **GPIO 24** | 출력 | `/RESET` | MSX 슬롯 리셋 신호 (Active LOW) |

---

## 3. PIO Side-Set 하드웨어 가속 구조 (`msxbus.pio`)

RP2350의 PIO2 스테이트 머신(SM1: Read, SM2: Write)을 사용하여, CPU 사이클 낭비 없이 2비트 Side-Set(`MODE0`, `MODE1`)과 8비트 데이터 버스 입출력을 단일 명령어로 원자적(Atomic) 처리합니다.

```pasm
.define public SIDE_LOW_ADDR   0b01  ; MODE1=0, MODE0=1 (Low Address A0-A7)
.define public SIDE_HIGH_ADDR  0b11  ; MODE1=1, MODE0=1 (High Address A8-A15)
.define public SIDE_DATA       0b10  ; MODE1=1, MODE0=0 (Data D0-D7)
.define public SIDE_IDLE       0b00  ; MODE1=0, MODE0=0 (Idle state)

.program msxbus_read
.side_set 2

.wrap_target
    pull block                  side SIDE_IDLE
    ; 1. Low Address (A0-A7) 출력 및 MODE = 01
    out pins, 8                 side SIDE_LOW_ADDR [4]
    ; 2. High Address (A8-A15) 출력 및 MODE = 11
    out pins, 8                 side SIDE_HIGH_ADDR [4]
    ; 3. 데이터 버스 Floating (GPIO 0-7 -> INPUT) 및 MODE = 10 (Data)
    mov osr, null               side SIDE_DATA
    out pindirs, 8              side SIDE_DATA [3]
    ; 4. ROM 액세스 타임 대기 (~300ns)
    nop [7]                     side SIDE_DATA
    nop [7]                     side SIDE_DATA
    ; 5. 데이터 버스 샘플링 및 RX FIFO 전송
    in pins, 8                  side SIDE_DATA
    push block                  side SIDE_DATA
    ; 6. 데이터 버스 출력 복구 및 MODE = 11 (Idle)
    mov osr, ~null              side SIDE_IDLE
    out pindirs, 8              side SIDE_IDLE [2]
.wrap
```

- **동작 특징**: 50MHz PIO 클록 분주 기준으로 단 8~10사이클(약 160~200ns) 안에 16비트 주소 전체가 래치에 고정됩니다.

### 3.2 PIO 읽기 엔진 (`msxbus_read_byte`)
카트리지 메모리 읽기 시 제어 신호 인가부터 `/WAIT` 핀 실시간 폴링 및 데이터 버스 래치까지 하드웨어로 수행합니다.

```pasm
.program msxbus_read_byte
.wrap_target
    pull block              ; 제어 마스크 수신 (/RD, /MREQ, /SLTSL 등)
    out pins, 8             ; 제어 신호 활성화 (Active LOW 출력)
    set pindirs, 0x00 [4]   ; GPIO 0~7을 입력 모드로 전환 (4사이클 버스 안정화)
wait_loop:
    jmp pin, wait_done      ; /WAIT 핀 샘플링 (HIGH면 정상 완료)
    jmp wait_loop           ; 카트리지가 LOW를 유지하면 대기 루프 지속
wait_done:
    in pins, 8              ; GPIO 0~7의 데이터 8비트 샘플링
    push block              ; RX FIFO로 데이터 푸시
.wrap
```

### 3.3 SIO Direct Fast Mode (단일 사이클 고속 실행)
PIO와 함께 C++ 레벨에서 초고속 처리를 위해 RP2350 SIO 하드웨어 레지스터를 직접 매핑한 인라인 루틴을 제공합니다.

```cpp
#define SIO_GPIO_SET(mask)   (sio_hw->gpio_set = (mask))
#define SIO_GPIO_CLR(mask)   (sio_hw->gpio_clr = (mask))
#define SIO_GPIO_IN()        (sio_hw->gpio_in)
#define SIO_GPIO_OE_SET(m)   (sio_hw->gpio_oe_set = (m))
#define SIO_GPIO_OE_CLR(m)   (sio_hw->gpio_oe_clr = (m))
```

---

## 4. 버스 트랜잭션 흐름 (Bus Transaction Flow)

### 4.1 Read 트랜잭션 (`MsxBus_ReadRaw`)
1. **스핀락 획득**: `spin_lock_blocking(s_bus_spinlock)`을 통해 멀티코어 버스 접근 독점.
2. **주소 래치 (`SetAddress`)**:
   - `GPIO 0~7`에 하위 주소 `addr & 0xFF` 출력 후 `MODE1` 펄스.
   - `GPIO 0~7`에 상위 주소 `addr >> 8` 출력 후 `MODE2` 펄스.
3. **데이터 버스 입력 전환**: `SIO_GPIO_OE_CLR(PIN_DATA_MASK)` (GPIO 0~7 Hi-Z/입력).
4. **제어 신호 Assert**: 명령에 따라 `/RD`, `/MREQ`, `/SLTSL1` 또는 `/SLTSL2`를 LOW로 내림.
5. **안정화 및 `/WAIT` 샘플링**: 버스 안정화 딜레이 후 `/WAIT`(GPIO 16)가 LOW인 동안 대기.
6. **데이터 읽기**: `SIO_GPIO_IN() & 0xFF`로부터 8비트 수신.
7. **제어 신호 복구**: 제어 라인을 모두 HIGH(Idle)로 복원하고 스핀락 해제.

### 4.2 Write 트랜잭션 (`MsxBus_Write`)
1. **스핀락 획득**
2. **주소 래치 (`SetAddress`)**
3. **데이터 버스 출력**: `GPIO 0~7`에 `value`를 출력.
4. **제어 신호 Assert**: `/MREQ`, `/SLTSL` 등을 LOW로 설정.
5. **`/WR` 펄스 인가**: `/WR`을 LOW로 전환 후 셋업 타임 유지.
6. **`/WAIT` 확인 후 복구**: `/WR` 및 제어 신호를 HIGH로 복원하고 스핀락 해제.

---

## 5. 슬롯 캐시 (Slot Cache) 설정

### 5.1 `#define USE_SLOT_CACHE 0` (기본값)
- **비활성화 이유**:
  1. 실제 하드웨어 카트리지 중 메가롬(Konami, ASCII, SCC, Mapper)은 특정 주소 읽기/쓰기에 의해 내부 뱅크가 동적으로 스위칭되므로, 일반 정적 캐시를 사용할 경우 뱅크 전환이 감지되지 않아 그래픽 깨짐이나 오동작이 발생합니다.
  2. I/O 슬롯 장치(FM 사운드 팩, 사운드 인터페이스 등)는 실시간 읽기가 필수적입니다.
  3. 캐시 메모리 비활성화 시 RP2350의 귀중한 SRAM을 **256KB(2 × 64KB Cache + 2 × 64KB Valid Flag)** 절약하여 에뮬레이터 코어 및 프레임버퍼에 할당할 수 있습니다.

### 5.2 캐시 활성화 방법 (필요 시)
`msxbus/msxbus.cpp` 상단의 매크로를 `1`로 변경:
```cpp
#define USE_SLOT_CACHE 1
```
활성화 시 읽기 작업은 SRAM 버퍼(`s_SlotCache`)에서 즉시 반환되며, 슬롯 쓰기나 하드웨어 리셋 시 `MsxBus_InvalidateCache()`를 통해 캐시가 무효화됩니다.

---

## 6. 소스 파일 구조

| 파일 경로 | 설명 |
|---|---|
| [`msxbus/msxbus.h`](file:///home/msx/pizero/Pico2MSX/msxbus/msxbus.h) | MSX 버스 공용 C API 선언 (`MsxBus_Init`, `MsxBus_Read`, `MsxBus_Write`, `MsxBus_Reset` 등) |
| [`msxbus/msxbus.cpp`](file:///home/msx/pizero/Pico2MSX/msxbus/msxbus.cpp) | Blueberry GPIO 및 Zemmix Mini 하드웨어 드라이버, SIO 제어, 스핀락 동기화 구현 |
| [`msxbus/romdump.h`](file:///home/msx/pizero/Pico2MSX/msxbus/romdump.h) | 실시간 카트리지 롬덤프 및 슬롯 진단 UI 헤더 |
| [`msxbus/romdump.cpp`](file:///home/msx/pizero/Pico2MSX/msxbus/romdump.cpp) | `ALT+D` 롬덤프 화면 렌더링, 10회 다중 읽기 버스 안정성 검사, 키보드 네비게이션 구현 |
| [`msxbus/msxbus.pio`](file:///home/msx/pizero/Pico2MSX/msxbus/msxbus.pio) | RP2350 PIO 스테이트 머신 어셈블리 소스 (주소 래치 및 고속 버스 읽기) |
| [`CMakeLists.txt`](file:///home/msx/pizero/Pico2MSX/CMakeLists.txt) | PIO 헤더 자동 생성(`pico_generate_pio_header`) 및 빌드 링크 설정 |

---

## 7. 실물 슬롯 진단: ROM Dump 기능 (`ALT+D`)

에뮬레이터 동작 중 실물 카트리지 슬롯 인식 상태와 버스 데이터 안정성을 실시간으로 확인하기 위한 **ROM Dump** 기능이 내장되어 있습니다.

### 7.1 주요 단축키 및 조작법
- **`ALT+D`**: 에뮬레이터를 일시정지하고 롬덤프 화면을 열거나 Slot 1 / Slot 2 토글.
- **`ESC`**: 롬덤프 화면을 닫고 에뮬레이터로 복귀.
- **`1` / `2`**: Slot 1(기본) 또는 Slot 2 슬롯 선택 및 덤프.
- **`UP` / `DOWN`**: 1행(8바이트) 스크롤.
- **`PageUp` / `PageDown` (또는 `Left` / `Right`)**: 1페이지(20행, 160바이트) 스크롤.
- **`Home` / `End`**: 카트리지 시작 영역(`0x4000`) / 끝 영역(`0xBFFF`) 이동.
- **`Enter` / `R`**: 현재 페이지 재읽기 (10회 반복 샘플링으로 불안정 비트/에러 검출).
- **`F5`**: 슬롯 하드웨어 리셋 펄스 인가 (`MsxBus_Reset(50)`).

### 7.2 화면 구성
1. **Title Bar**: 슬롯 종류 (`GPIO` / `ZEMMIX`), 슬롯 번호, 현재 덤프 주소 범위 표시.
2. **ROM Header**: `0x4000` 번지의 MSX 식별자 (`41 42` = `AB`) 및 `INIT`, `STMT`, `DEV` 포인터 확인.
3. **Hex & ASCII View**: 8바이트 단위 주소, 16진수 데이터 및 ASCII 문자 표시 (10회 읽기 중 데이터 불일치가 발생한 불안정 바이트는 빨간색으로 표시).
4. **Help Footer**: 사용 가능한 단축키 안내.

---

## 8. 빌드 및 테스트

RP2350 환경에서 MSX1 빌드:

```bash
export PICO_SDK_PATH=/home/msx/pico-sdk
cd /home/msx/pizero/Pico2MSX/build_msx1
ninja
picotool uf2 convert picomsx.elf picomsx_waveshare_msx1.uf2 --family rp2350-arm-s
```

생성된 `picomsx_waveshare_msx1.uf2`를 Waveshare RP2350-PiZero의 BOOTSEL 드라이브에 복사하면 즉시 실물 슬롯 카트리지와 통신할 수 있습니다.
