# MSX V9990 / V9968 Emulator Cartridge (RP2350)

[English](#english)

MSX 실기기에서 **V9990(GFX9000)과 V9968을 가장 저렴하게 경험**해 보기 위한 프로젝트다.
실제 V9990 칩이나 FPGA 없이 RP2350 보드 하나가 MSX 버스를 받아 VDP처럼 동작하고,
화면은 HDMI(DVI 640×480@60)로 출력한다. V9990 모드에서는 I/O 포트 0x60–0x6F를 쓴다.

| MSX 실기 동작 | Geo3D 테스트 (V9968 + geo3d) |
|---|---|
| [![MSX 실기에서 동작하는 모습 (YouTube)](https://img.youtube.com/vi/r0W5C8UGfvY/hqdefault.jpg)](https://youtu.be/r0W5C8UGfvY) | [![Geo3D 테스트 (YouTube)](https://img.youtube.com/vi/VtkUM869Wkc/hqdefault.jpg)](https://youtu.be/VtkUM869Wkc) |

> **주의: 에뮬레이터다.** 실제 칩이 아니라 소프트웨어로 VDP 동작을 흉내 낸다.
> V9990은 Yamaha 매뉴얼과 openMSX 동작을, V9968은 HRA!의 RTL을 기준으로 맞췄지만,
> 실제 V9990 칩이나 V9968 카트리지(RTL)와 결과(화면, 타이밍, 커맨드 속도 등)가 다를 수 있다.
> 알려진 차이는 아래 [알려진 위험](#알려진-위험)에 정리했다.

> **면책: 사용에 따른 모든 위험은 이용자에게 있다.** 이 프로젝트의 회로도, PCB, 펌웨어,
> 소스 코드는 "있는 그대로(AS IS)" 제공되며 어떤 보증도 하지 않는다. 이를 제작하거나 사용하면서
> 생기는 MSX 본체·주변기기·모니터의 고장, 데이터 손실 등 모든 손해와 위험은 전적으로 이용자가
> 부담하며, 제작자는 어떠한 책임도 지지 않는다.

카트리지의 슬라이드 스위치(SW1, PCB rev 0.2)로 **V9968 + geo3d v2.0**(HRA!의 V9968 카트리지 호환, 포트 88h–8Fh)도 고를 수 있다.
스위치가 포트 디코더와 펌웨어 선택을 함께 바꾼다. 스위치 없는 rev 0.1 기판은 펌웨어 설정(`-DMSX_VDP_SWITCH=OFF`, USB 시리얼 `chip v9968`)으로 고른다.
자세한 내용은 [docs/v9968.md](docs/v9968.md)

- 프로토타입 보드: **Pimoroni Pico Plus 2** (RP2350B, 플래시 16MB, PSRAM 8MB)
  또는 **Waveshare Core2350B1/B2** (`-DPICO_BOARD=waveshare_core2350b`)
- 결선: [docs/hardware.md](docs/hardware.md)
- 카트리지 PCB (KiCad 8, JLCPCB 주문용 거버 포함): [hardware/](hardware/README.md)

> **실기 검증된 조합은 하나뿐이다:** Core2350B판 PCB(rev 0.3) + **Waveshare Core2350B2**(PSRAM 8MB).
> Pico Plus 2판 PCB와 Core2350B1(PSRAM 2MB)은 제작·검증하지 않았다.

## 구조

```
core/        이식 가능한 VDP 코어 (Pico SDK 의존성 없음, PC와 펌웨어가 공유)
  v9990.c        포트, 레지스터, 팔레트, 모드 해석, 타이밍 이벤트
  v9990_cmd.c    커맨드 엔진(블리터)
  v9990_render.c 스캔라인 렌더러 → RGB565
  v9990_vram.h   VRAM 물리 배치 (P1/P2/Bx 뱅크 인터리브)
  v9968*.c       V9968: 포트/레지스터, 커맨드 엔진(LRMM/LFMx 포함), 렌더러(스프라이트 모드 3)
  geo3d.c        geo3d v2.0 (참조 모델을 C로 옮김, V9968 커맨드 엔진에 명령 발행)
host/        PC용 테스트, 렌더 데모, 트레이스 재생기 (gcc)
tools/compare/  openMSX(GFX9000)와 도트 단위 비교 환경, 게임 기록 도구
firmware/    RP2350 펌웨어 (pico-sdk)
  src/bus.pio    버스 모니터 + 읽기 응답 PIO 프로그램
  src/bus.c      core1: 버스 이벤트 처리, 커맨드 엔진 실행, /INT
  src/dvi_out.c  HSTX DVI 출력, V9990 타이밍(VBlank, 라인 인터럽트)
  src/psram.c    PSRAM(VRAM) 초기화
  src/main.c     클럭(384MHz, HSTX 126MHz), 칩 선택, core0 렌더 루프
  src/vdp_select.c  칩 선택 설정 (플래시 저장, USB 시리얼 명령)
hardware/    카트리지 PCB (KiCad 프로젝트 생성 스크립트, 거버/BOM/CPL)
docs/        하드웨어 문서
```

## 동작 방식

```
MSX OUT ─► PIO monitor ─► DMA ─► 링버퍼 ─► core1 ─► v9990_write_port()
MSX IN  ─► PIO read SM ─► DMA ─► read_table[port] ─► PIO가 D0-D7 구동
                                      ▲
                     core1이 v9990_peek_port()로 항상 최신 값 유지
core0: DVI DMA IRQ ─► 렌더러가 2–3라인 앞서 v9990_render_line()
```

- 읽기는 CPU를 거치지 않고 PIO와 DMA만으로 응답하므로 Z80 I/O 사이클(약 700ns) 안에 충분히 들어온다.
- VRAM 512KB는 PSRAM에 둔다(내부 SRAM은 520KB라 부족).

## 빌드

툴체인 경로(이 머신 기준):

```sh
export PICO_SDK_PATH=~/tools/pico/pico-sdk
export PICO_TOOLCHAIN_PATH=~/tools/pico/toolchain
export PATH=~/tools/pico/cmake/bin:$PATH     # picotool 빌드에 cmake 3.17 이상 필요
```

### PC 테스트

```sh
cmake -S host -B build-host -G Ninja && ninja -C build-host
./build-host/test_v9990                           # 단위 테스트
./build-host/render_demo build-host/demo.ppm      # 640x480 출력 이미지 생성
```

### 펌웨어

```sh
cmake -S firmware -B build-fw -G Ninja && ninja -C build-fw
# BOOTSEL 상태로 연결한 뒤 build-fw/msx_v9990.uf2 복사
```

다른 RP2350B 보드:
`cmake -S firmware -B build-fw -DPICO_BOARD=<board> -DV9990_PSRAM_CS_PIN=<gpio>`

USB 시리얼 명령(진단용):

| 명령 | 내용 |
|---|---|
| `stat [reset]` | core1 루프·버스 이벤트·포트별 횟수, `/INT`·`/WAIT`, V9990 레지스터, 렌더 시간, DVI 상태(클럭 실측, 늦은 IRQ, 출력 재시작). `reset`은 최대값을 지운다 |
| `psram [sweep]` | PSRAM 쓰기/읽기 검사와 스트리밍 읽기 속도. `sweep`은 클럭 분주·RX 지연별로 검사한다 (VRAM을 지움) |
| `vram` | VRAM 64KB 블록별 CRC32 (PC 재생 결과와 비교용) |
| `out <포트> <값>` | V9990 포트에 직접 쓰기 (16진, MSX 없이 모드 시험) |
| `bench` | V9990: 현재 화면 한 줄의 렌더 시간(전체, 스프라이트 없이, VRAM 전부 SRAM일 때, 화면 꺼짐, VRAM 읽기 속도). V9968: 현재 화면 전체 줄의 평균·최대와 단계별 시간 |
| `dump` | V9968 상태(레지스터, 팔레트, VRAM)를 출력한다. 로그를 `host/v9968_dump_replay`로 PC에서 그려 볼 수 있다 (약 70초, 그동안 커맨드 엔진이 멈춤) |

`-DMSX_BOOT_TRACE=ON`으로 빌드하면 부팅 시 USB 터미널을 최대 60초 기다린 뒤 부팅 단계를 출력한다.
`stat`의 출력은 렌더 루프에서 나가므로, 주기적으로 보내면 그 프레임의 화면이 깨진다.

## 진행 상황

| 영역 | 상태 |
|---|---|
| 포트 P#0–P#7, 레지스터 쓰기 마스크, VRAM 포인터(R#2/R#5에서 갱신), 팔레트 | ✅ (PC 테스트) |
| 표시 모드 판정 (P1, P2, B1–B7, MCS 포함, 매뉴얼 8.3 표) | ✅ (PC 테스트) |
| 인터럽트 (VI, HI, CE), 상태 레지스터 | ✅ 기본 |
| P1 모드: 2레이어, 레이어별 팔레트, 앞/뒤 영역(R#27), 스크롤 | ✅ (PC 테스트) |
| P2 모드: 1024×512 이미지, 짝수/홀수 바이트 팔레트 | ✅ (PC 테스트) |
| P1/P2 스프라이트: 125개, 줄당 16개, 앞/뒤 우선순위, 팔레트 | ✅ (PC 테스트) |
| 세로 스크롤 분할(라인 중간 SCAY 쓰기), R512/R256 롤 | ✅ (PC 테스트) |
| 비트맵 렌더링 BP2/BP4/BP6/BD8/BD16 | ✅ (PC 테스트) |
| 커맨드: STOP, LMMC, LMMV, LMMM, CMMC, CMMM, LINE, PSET (비트맵 좌표계, FC/BC/WM 비트 위치 대응) | ✅ (PC 테스트) |
| 비트맵 커서 2개 (32×32, 팔레트/EOR, 커서 0 우선) | ✅ (PC 테스트, openMSX 동작 기준) |
| 펌웨어: PIO 버스, DVI 출력, PSRAM, 듀얼 코어 | ✅ 실기 확인 (Core2350B: Chase H.Q. 주행, Petscii Robots 게임 화면) |
| YJK/YUV, R#16 화면 위치 조정 | ⬜ TODO |
| 커맨드 P1/P2 좌표계 (openMSX와 같은 매핑, 실기 Ghosts'n Goblins에서 확인) | ✅ |
| 커맨드: LMCM, CMMK, BMxx, SRCH, POINT, ADVN | ⬜ TODO |
| HR 비트 (펌웨어: DVI 두 줄마다 토글) | ✅ 기본 |
| 커맨드 실행 시간, 라인 타이밍 정밀도, PAL 50Hz 타이밍 | ⬜ TODO |
| 15kHz RGB 출력 (PIO + 저항 DAC) | ⬜ 선택 사항 |
| **V9968 + geo3d v2.0** (펌웨어 설정으로 선택): 전 화면 모드, 스프라이트 모드 1–3, 전 커맨드, 인터럽트 | ✅ (PC 테스트, RTL·참조 모델·openMSX 포크와 비교) — [docs/v9968.md](docs/v9968.md) |

기준 자료: Yamaha V9990 Application Manual(1992), 매뉴얼에 없는 동작은 openMSX의 구현을 참고했다.
`tools/compare/`로 openMSX 16.0과 비교한 결과, 테스트 장면 전부가 도트 단위로 일치한다.
실제 게임에서는 다음과 같다.
- **Chase H.Q. V9990**(DiHalt Studio, 90초 플레이): 캡처 78장 중 76장이 일치한다. 나머지 2장도 그 시점의 VRAM은 openMSX와 같다(화면 스캔 타이밍 차이).
- **Another World**(Edd Biddulph, 터보 R, 4분 인트로): 113장 모두 일치한다.
- MSXdev/Mapax 작품(openMSX 60초 자동 입력, 전원 직후 화면 제외):
  - **Attack of the Petscii Robots**(P1, 스프라이트): 33장 모두 일치.
  - **MSX in a Row**(B3 BD16, LMMC, 비트맵 커서): 21장 일치, 11장은 0.1% 미만(커서 아래 끝 1줄, 미확인).
  - **Dreams Puzzle**(B3 BD16, LINE), **Mr. Balloon remake**(B7 1024 BP4): VRAM이 26/30 시점에서 같고,
    나머지는 직전 프레임에 시작된 커맨드 영역뿐이다(openMSX는 커맨드에 시간이 걸림). B7 화면은 openMSX
    캡처가 640픽셀로 줄어 도트 비교를 할 수 없다.
- **Ghosts'n Goblins**(ASM Team, V9990 데모, Konami SCC 2MB, B1 BP6/BP4 + P1 스프라이트, P1에서 LMMV/LMMM):
  직접 플레이한 기록 66장 모두 일치. P1 커맨드 좌표계가 없을 때는 악마 스프라이트가 깨졌다.
펌웨어의 렌더러는 스캔보다 2–3줄 앞서 그리므로, 화면 중간의 레지스터 변경은 몇 줄 일찍 적용된다(TODO).
새 프레임은 첫 줄 4줄 전에 시작한다(라인 카운터 리셋): 그 전에 VI 핸들러가 쓴 스크롤이 첫 줄부터 적용된다.

코드의 `VERIFY` 주석은 V9990 Application Manual이나 openMSX와 아직 대조하지 않은 부분이다
(표시 모드 표, 비트 순서, NX/NY=0의 의미 등).

## 다음 단계

1. 다른 GFX9000 소프트웨어(P1/P2, 스프라이트, 커맨드 엔진)로 실기 호환성 테스트
2. PAL(R#7 bit 3) 게임: VI를 50Hz로 낼지 결정 (HDMI 출력은 60Hz 고정)
3. 커맨드 엔진 속도: 게임 진행 중 PSRAM 경합으로 렌더 시간이 늘어난다 (B1 평균 34us/63us)

## 알려진 위험

- **읽기마다 `/WAIT`**: 읽기 SM이 모든 읽기에서 `/WAIT`를 걸고, core1이 앞선 접근을 모두 처리한 뒤에 응답한다
  (쓰기 직후의 상태 읽기, 같은 포트 연속 읽기도 순서대로 맞는다). 그만큼 읽기 사이클이 길어진다. core1이 20us 안에
  응답하지 못하면 `stat`의 ordered-read timeouts가 늘어난다.
- **렌더 시간**: VRAM0(물리 0x00000-0x3FFFF)은 SRAM, VRAM1은 PSRAM이다. 비트맵 한 줄은 XIP 스트리밍 DMA로
  읽는다(126MHz). 한계에 가까운 모드: B3 512×424 BD8 줄당 약 29us(한계 31.7us), P1(Petscii Robots)
  평균 60us(한계 63us). BD16 512×424는 아직 측정하지 않았다.
- **P1/P2 이름 표 캐시**: 이름 표 한 줄은 타일 행(8줄)마다 한 번 읽는다. 프레임 중간에 바뀐 이름은 그 타일 행의
  나머지 줄에서 다음 프레임에 보일 수 있다.
- **PSRAM 126MHz**: Core2350B 한 장에서 RX 지연 2–5가 정상(3 사용). 온도 여유는 미확인.
- **384MHz 오버클럭 + 1.30V**: clk_sys는 pll_usb(384MHz), HSTX는 pll_sys(252MHz)/2. HSTX 분주기는 정수
  분주만 되고 듀티 보정이 없어서, clk_sys/3(378MHz)에서는 모니터 화면이 대부분 검게 나왔다. Core2350B 한 장에서
  확인했고 개체별·온도별 여유는 미확인.
- **V9968 2줄 앞서 렌더**: 스프라이트 모드 3이 많은 화면(LUMEN FORGE)은 줄당 최대 약 90us라 바로 앞 줄에서
  그리면(31.7us) 늦는다. 대신 줄 인터럽트로 바꾼 레지스터가 실제 카트리지보다 2줄 늦게 보인다.
- **V9968 LRMM 속도**: core1의 LRMM이 실제 칩(픽셀당 약 12클럭)보다 수 배 느려서, LRMM으로 화면 전체를
  그리는 데모(SUPER CAT 약 11fps)는 실제보다 느리게 움직인다. 결과는 HRA의 vdp_command.v와 같다.

## 참고

- Yamaha V9990 E-VDP-III Application Manual
- openMSX `src/video/v9990/` (동작만 참고, 코드는 가져오지 않음)
- pico-examples `hstx/dvi_out_hstx_encoder`

## 라이선스

- 소프트웨어(`core/`, `firmware/`, `host/`, `tools/`): MIT (`LICENSE`)
- 하드웨어(`hardware/`): CERN-OHL-P v2 (`hardware/LICENSE`)
- 예외: V9968 코어(`core/v9968*.c`, `core/v9968.h`)는 HRA!의 V9968 RTL을 따라 만들었기 때문에
  그 RTL의 조건을 따른다(저작권 표시 유지, 허락 없는 판매·상업적 사용 금지).
  이 코어가 들어간 펌웨어 바이너리(`.uf2`)를 배포할 때도 같은 표시를 함께 넣어야 한다.
- 외부 코드와 참고 자료의 표시: `THIRD_PARTY_NOTICES.md`
- 모든 라이선스는 보증 없이(AS IS) 제공되며, 사용에 따른 위험과 책임은 이용자에게 있다(위의 면책 참고).

---

<a id="english"></a>

# English

This project is the **cheapest way to try the V9990 (GFX9000) and the V9968 on a real MSX**.
There is no real V9990 chip or FPGA: a single RP2350 board listens to the MSX bus and behaves like the VDP,
and the picture goes out over HDMI (DVI 640×480@60). In V9990 mode it uses I/O ports 0x60–0x6F.

| On a real MSX | Geo3D test (V9968 + geo3d) |
|---|---|
| [![Running on a real MSX (YouTube)](https://img.youtube.com/vi/r0W5C8UGfvY/hqdefault.jpg)](https://youtu.be/r0W5C8UGfvY) | [![Geo3D test (YouTube)](https://img.youtube.com/vi/VtkUM869Wkc/hqdefault.jpg)](https://youtu.be/VtkUM869Wkc) |

> **Note: this is an emulator.** The VDP is imitated in software, not built as a chip.
> The V9990 follows the Yamaha manual and openMSX's behaviour, the V9968 follows HRA!'s RTL,
> but results (picture, timing, command speed, ...) can differ from a real V9990 chip or the V9968
> cartridge (RTL). Known differences are listed under [Known risks](#known-risks).

> **Disclaimer: you use this project entirely at your own risk.** The schematics, PCB, firmware and
> source code are provided "AS IS", without any warranty. All damage and risk from building or using
> them, including failure of the MSX, peripherals or monitor and loss of data, are borne entirely by
> the user; the author accepts no liability whatsoever.

The cartridge's slide switch (SW1, PCB rev 0.2) also selects **V9968 + geo3d v2.0** (compatible with HRA!'s
V9968 cartridge, ports 88h–8Fh). The switch changes both the port decoder and the firmware's choice.
On the rev 0.1 board without the switch, choose in the firmware (`-DMSX_VDP_SWITCH=OFF`, USB serial `chip v9968`).
Details: [docs/v9968.md](docs/v9968.md) (Korean)

- Prototype boards: **Pimoroni Pico Plus 2** (RP2350B, 16MB flash, 8MB PSRAM)
  or **Waveshare Core2350B1/B2** (`-DPICO_BOARD=waveshare_core2350b`)
- Wiring: [docs/hardware.md](docs/hardware.md) (Korean)
- Cartridge PCB (KiCad 8, with Gerbers for JLCPCB): [hardware/](hardware/README.md) (Korean)

> **Only one combination has been verified on hardware:** the Core2350B PCB (rev 0.3) + **Waveshare Core2350B2** (8MB PSRAM).
> The Pico Plus 2 PCB and the Core2350B1 (2MB PSRAM) have not been built or tested.

## Layout

```
core/        portable VDP core (no Pico SDK dependencies, shared by the PC and the firmware)
  v9990.c        ports, registers, palette, mode decoding, timing events
  v9990_cmd.c    command engine (blitter)
  v9990_render.c scanline renderer -> RGB565
  v9990_vram.h   physical VRAM layout (P1/P2/Bx bank interleave)
  v9968*.c       V9968: ports/registers, command engine (incl. LRMM/LFMx), renderer (sprite mode 3)
  geo3d.c        geo3d v2.0 (reference model ported to C, issues commands to the V9968 engine)
host/        PC tests, render demo, trace replayers (gcc)
tools/compare/  dot-exact comparison against openMSX (GFX9000), game recording tools
firmware/    RP2350 firmware (pico-sdk)
  src/bus.pio    bus monitor + read response PIO programs
  src/bus.c      core1: bus events, command engine, /INT
  src/dvi_out.c  HSTX DVI output, V9990 timing (VBlank, line interrupts)
  src/psram.c    PSRAM (VRAM) setup
  src/main.c     clocks (384MHz, HSTX 126MHz), chip selection, core0 render loop
  src/vdp_select.c  chip selection setting (stored in flash, USB serial command)
hardware/    cartridge PCB (KiCad project generator scripts, Gerbers/BOM/CPL)
docs/        hardware documents
```

## How it works

```
MSX OUT ─► PIO monitor ─► DMA ─► ring buffer ─► core1 ─► v9990_write_port()
MSX IN  ─► PIO read SM ─► DMA ─► read_table[port] ─► PIO drives D0-D7
                                      ▲
                     core1 keeps it current with v9990_peek_port()
core0: DVI DMA IRQ ─► renderer runs v9990_render_line() 2–3 lines ahead
```

- Reads are answered by PIO and DMA alone, without the CPU, well inside a Z80 I/O cycle (about 700ns).
- The 512KB of VRAM lives in PSRAM (the 520KB of internal SRAM is not enough).

## Build

Toolchain paths (on the development machine):

```sh
export PICO_SDK_PATH=~/tools/pico/pico-sdk
export PICO_TOOLCHAIN_PATH=~/tools/pico/toolchain
export PATH=~/tools/pico/cmake/bin:$PATH     # picotool needs cmake 3.17 or later
```

### PC tests

```sh
cmake -S host -B build-host -G Ninja && ninja -C build-host
./build-host/test_v9990                           # unit tests
./build-host/render_demo build-host/demo.ppm      # 640x480 output image
```

### Firmware

```sh
cmake -S firmware -B build-fw -G Ninja && ninja -C build-fw
# connect in BOOTSEL mode and copy build-fw/msx_v9990.uf2
```

Other RP2350B boards:
`cmake -S firmware -B build-fw -DPICO_BOARD=<board> -DV9990_PSRAM_CS_PIN=<gpio>`

USB serial commands (diagnostics):

| Command | What it does |
|---|---|
| `stat [reset]` | core1 loop, bus events, counts per port, `/INT` and `/WAIT`, V9990 registers, render times, DVI state (measured clocks, late IRQs, output restarts). `reset` clears the maxima |
| `psram [sweep]` | PSRAM write/read check and streaming read speed. `sweep` checks every clock divider and RX delay (clears VRAM) |
| `vram` | CRC32 of each 64KB VRAM block (to compare with a PC replay) |
| `out <port> <value>` | write a V9990 port directly (hex; tries modes without an MSX) |
| `bench` | V9990: render time of one line of the current picture (full, without sprites, with all VRAM in SRAM, display off, VRAM read speed). V9968: average and maximum over all lines of the current picture, per stage |
| `dump` | prints the V9968 state (registers, palette, VRAM); `host/v9968_dump_replay` draws the log on the PC (about 70 s, the command engine stops meanwhile) |

Built with `-DMSX_BOOT_TRACE=ON`, the firmware waits up to 60 s for a USB terminal at boot and prints the boot steps.
`stat` prints from the render loop, so sending it periodically breaks the picture of that frame.

## Status

| Area | Status |
|---|---|
| Ports P#0–P#7, register write masks, VRAM pointers (updated on R#2/R#5), palette | ✅ (PC tests) |
| Display mode decoding (P1, P2, B1–B7, incl. MCS, manual table 8.3) | ✅ (PC tests) |
| Interrupts (VI, HI, CE), status registers | ✅ basic |
| P1: two layers, palette per layer, front/back areas (R#27), scrolling | ✅ (PC tests) |
| P2: 1024×512 image, even/odd byte palettes | ✅ (PC tests) |
| P1/P2 sprites: 125, 16 per line, front/back priority, palettes | ✅ (PC tests) |
| Vertical scroll splits (SCAY written mid-line), R512/R256 roll | ✅ (PC tests) |
| Bitmap rendering BP2/BP4/BP6/BD8/BD16 | ✅ (PC tests) |
| Commands: STOP, LMMC, LMMV, LMMM, CMMC, CMMM, LINE, PSET (bitmap coordinates, FC/BC/WM bit positions) | ✅ (PC tests) |
| Two bitmap cursors (32×32, palette/EOR, cursor 0 first) | ✅ (PC tests, after openMSX's behaviour) |
| Firmware: PIO bus, DVI output, PSRAM, dual core | ✅ on hardware (Core2350B: Chase H.Q. racing, Petscii Robots in game) |
| YJK/YUV, R#16 display adjust | ⬜ TODO |
| Command P1/P2 coordinate systems (same mapping as openMSX, checked with Ghosts'n Goblins on hardware) | ✅ |
| Commands: LMCM, CMMK, BMxx, SRCH, POINT, ADVN | ⬜ TODO |
| HR bit (firmware: toggles every two DVI lines) | ✅ basic |
| Command timing, line timing accuracy, PAL 50Hz timing | ⬜ TODO |
| 15kHz RGB output (PIO + resistor DAC) | ⬜ optional |
| **V9968 + geo3d v2.0** (chosen by the firmware setting): all screen modes, sprite modes 1–3, all commands, interrupts | ✅ (PC tests, compared with the RTL, the reference model and the openMSX fork) — [docs/v9968.md](docs/v9968.md) |

References: Yamaha V9990 Application Manual (1992); where the manual is silent, openMSX's implementation.
Compared with openMSX 16.0 through `tools/compare/`, every test scene matches dot for dot.
In real games:
- **Chase H.Q. V9990** (DiHalt Studio, 90 s of play): 76 of 78 captures match. For the other 2 the VRAM at that moment equals openMSX's (a difference in scan timing).
- **Another World** (Edd Biddulph, turbo R, 4-minute intro): all 113 match.
- MSXdev/Mapax titles (60 s of automatic input in openMSX, power-on screens excluded):
  - **Attack of the Petscii Robots** (P1, sprites): all 33 match.
  - **MSX in a Row** (B3 BD16, LMMC, bitmap cursor): 21 match, 11 differ by under 0.1% (one line at the bottom of the cursor, not investigated).
  - **Dreams Puzzle** (B3 BD16, LINE), **Mr. Balloon remake** (B7 1024 BP4): VRAM equal at 26 of 30 points;
    the rest is only the area of a command started in the previous frame (openMSX gives commands time).
    The B7 picture cannot be compared dot by dot because openMSX scales its capture to 640 pixels.
- **Ghosts'n Goblins** (ASM Team, V9990 demo, Konami SCC 2MB, B1 BP6/BP4 + P1 sprites, LMMV/LMMM in P1):
  all 66 shots of a recorded play match. Without the P1 command coordinates the demon sprite was garbled.

The firmware renderer draws 2–3 lines ahead of the scan, so register changes mid-picture take effect a few lines early (TODO).
A new frame starts 4 lines before its first line (line counter reset), so scroll values written by the VI handler apply from the first line.

`VERIFY` comments in the code mark what has not yet been checked against the V9990 Application Manual or openMSX
(the display mode table, bit orders, the meaning of NX/NY = 0, ...).

## Next steps

1. Compatibility tests on hardware with other GFX9000 software (P1/P2, sprites, command engine)
2. PAL games (R#7 bit 3): decide whether VI runs at 50Hz (the HDMI output is fixed at 60Hz)
3. Command engine speed: PSRAM contention during play lengthens render times (B1 average 34us of 63us)

## Known risks

- **`/WAIT` on every read**: the read SM asserts `/WAIT` on every read and answers after core1 has applied every
  earlier access (a status read right after a write, or back-to-back reads of one port, come out in order). Read
  cycles get longer by that much. If core1 does not answer within 20us, `stat` counts an ordered-read timeout.
- **Render time**: VRAM0 (physical 0x00000-0x3FFFF) is in SRAM, VRAM1 in PSRAM. A bitmap line is read with XIP
  streaming DMA (126MHz). Modes near the limit: B3 512×424 BD8 about 29us a line (limit 31.7us), P1 (Petscii Robots)
  60us average (limit 63us). BD16 512×424 not measured yet.
- **P1/P2 name table cache**: a name table row is read once per tile row (8 lines). A name changed mid-frame may
  show on the remaining lines of that tile row only in the next frame.
- **PSRAM at 126MHz**: RX delays 2–5 work on one Core2350B (3 is used). Temperature margin unknown.
- **384MHz overclock at 1.30V**: clk_sys from pll_usb (384MHz), HSTX from pll_sys (252MHz)/2. The HSTX divider
  only divides by integers and has no duty-cycle correction, so at clk_sys/3 (378MHz) the monitor showed mostly black.
  Checked on one Core2350B; margins across boards and temperatures unknown.
- **V9968 renders two lines ahead**: pictures with much sprite mode 3 (LUMEN FORGE) take up to about 90us a line,
  too late when drawn just before the line (31.7us). In exchange, registers changed by line interrupts show two lines
  later than on the real cartridge.
- **V9968 LRMM speed**: LRMM on core1 is several times slower than the real chip (about 12 clocks per pixel), so demos
  that draw the whole picture with LRMM (SUPER CAT about 11 fps) move slower than on the real thing. The results
  equal HRA's vdp_command.v.

## References

- Yamaha V9990 E-VDP-III Application Manual
- openMSX `src/video/v9990/` (behaviour only, no code taken)
- pico-examples `hstx/dvi_out_hstx_encoder`

## License

- Software (`core/`, `firmware/`, `host/`, `tools/`): MIT (`LICENSE`)
- Hardware (`hardware/`): CERN-OHL-P v2 (`hardware/LICENSE`)
- Exception: the V9968 core (`core/v9968*.c`, `core/v9968.h`) follows HRA!'s V9968 RTL and therefore keeps
  that RTL's conditions (keep the copyright notice; no sale or commercial use without permission).
  Firmware binaries (`.uf2`) containing this core must carry the same notice when distributed.
- Notices for third-party code and references: `THIRD_PARTY_NOTICES.md`
- All licences come without warranty (AS IS); the risk and responsibility of use lie with the user (see the disclaimer above).
