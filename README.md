# MSX V9990 Emulator Cartridge (RP2350)

GFX9000 / PowerGraph 호환 카트리지를 실제 V9990 칩 없이 RP2350 하나로 구현하는 프로젝트.
MSX의 I/O 포트 0x60–0x6F를 받아 V9990처럼 동작하고, 화면은 HDMI(DVI 640×480@60)로 출력한다.

- 프로토타입 보드: **Pimoroni Pico Plus 2** (RP2350B, 플래시 16MB, PSRAM 8MB)
  또는 **Waveshare Core2350B1/B2** (`-DPICO_BOARD=waveshare_core2350b`)
- 결선: [docs/hardware.md](docs/hardware.md)
- 카트리지 PCB (KiCad 8, JLCPCB 주문용 거버 포함): [hardware/](hardware/README.md)

## 구조

```
core/        이식 가능한 V9990 코어 (Pico SDK 의존성 없음, PC와 펌웨어가 공유)
  v9990.c        포트, 레지스터, 팔레트, 모드 해석, 타이밍 이벤트
  v9990_cmd.c    커맨드 엔진(블리터)
  v9990_render.c 스캔라인 렌더러 → RGB565
  v9990_vram.h   VRAM 물리 배치 (P1/P2/Bx 뱅크 인터리브)
host/        PC용 테스트와 렌더 데모 (gcc)
firmware/    RP2350 펌웨어 (pico-sdk)
  src/bus.pio    버스 모니터 + 읽기 응답 PIO 프로그램
  src/bus.c      core1: 버스 이벤트 처리, 커맨드 엔진 실행, /INT
  src/dvi_out.c  HSTX DVI 출력, V9990 타이밍(VBlank, 라인 인터럽트)
  src/psram.c    PSRAM(VRAM) 초기화
  src/main.c     클럭(252MHz), core0 렌더 루프
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
| 커맨드: STOP, LMMC, LMMV, LMMM, PSET (비트맵 좌표계) | ✅ 기본 |
| 펌웨어: PIO 버스, DVI 출력, PSRAM, 듀얼 코어 | ✅ 빌드됨, **실기 미검증** |
| 비트맵 커서, YJK/YUV, R#16 화면 위치 조정 | ⬜ TODO |
| 커맨드: LMCM, CMMx, BMxx, LINE, SRCH, POINT, ADVN, P1/P2 좌표계 | ⬜ TODO |
| 커맨드 실행 시간, HR 비트, 라인 타이밍 정밀도 | ⬜ TODO |
| 15kHz RGB 출력 (PIO + 저항 DAC) | ⬜ 선택 사항 |

기준 자료: Yamaha V9990 Application Manual(1992), 매뉴얼에 없는 동작은 openMSX의 구현을 참고했다.
펌웨어의 렌더러는 스캔보다 2–3줄 앞서 그리므로, 화면 중간의 레지스터 변경은 몇 줄 일찍 적용된다(TODO).

코드의 `VERIFY` 주석은 V9990 Application Manual이나 openMSX와 아직 대조하지 않은 부분이다
(표시 모드 표, 비트 순서, NX/NY=0의 의미 등).

## 다음 단계

1. 실기 버스 검증: MSX BASIC에서 `OUT &H64,15: OUT &H63,5` / `PRINT INP(&H65)`, USB 시리얼 로그 확인
2. DVI 출력 확인: 백드롭 색과 테스트 패턴
3. VRAM을 PSRAM에 둔 상태에서 렌더 성능 측정 (부족하면 라인 캐시나 SRAM 배치 검토)
4. GFX9000 소프트웨어로 호환성 테스트하면서 TODO 구현

## 알려진 위험

- **같은 포트를 연속으로 빠르게 읽을 때** (R800 `INIR`): core1이 `read_table`을 갱신하기 전에 다음 읽기가 오면 이전 값이 나간다.
  대책은 `/WAIT`(GPIO28, 배선은 해 둠)이다.
- **PSRAM 대역폭**: 비트맵 모드는 충분할 것으로 예상하지만, 패턴 모드와 블리터의 랜덤 접근은 측정이 필요하다.
- **252MHz 오버클럭 + 1.20V**: 대부분의 RP2350에서 동작한다고 알려져 있지만 개체별로 확인이 필요하다.

## 참고

- Yamaha V9990 E-VDP-III Application Manual
- openMSX `src/video/v9990/` (레퍼런스 구현, GPL: 코드를 가져오면 이 프로젝트도 GPL이 됨)
- pico-examples `hstx/dvi_out_hstx_encoder`
