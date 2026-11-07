# MSX V9990 Emulator Cartridge (RP2350)

GFX9000 / PowerGraph 호환 카트리지를 실제 V9990 칩 없이 RP2350 하나로 구현하는 프로젝트.
MSX의 I/O 포트 0x60–0x6F를 받아 V9990처럼 동작하고, 화면은 HDMI(DVI 640×480@60)로 출력한다.

카트리지의 슬라이드 스위치(SW1, PCB rev 0.2)로 **V9968 + geo3d v2.0**(HRA!의 V9968 카트리지 호환, 포트 88h–8Fh)도 고를 수 있다.
스위치가 포트 디코더와 펌웨어 선택을 함께 바꾼다. 스위치 없는 rev 0.1 기판은 펌웨어 설정(`-DMSX_VDP_SWITCH=OFF`, USB 시리얼 `chip v9968`)으로 고른다.
자세한 내용은 [docs/v9968.md](docs/v9968.md)

- 프로토타입 보드: **Pimoroni Pico Plus 2** (RP2350B, 플래시 16MB, PSRAM 8MB)
  또는 **Waveshare Core2350B1/B2** (`-DPICO_BOARD=waveshare_core2350b`)
- 결선: [docs/hardware.md](docs/hardware.md)
- 카트리지 PCB (KiCad 8, JLCPCB 주문용 거버 포함): [hardware/](hardware/README.md)

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
  src/main.c     클럭(252MHz), 칩 선택, core0 렌더 루프
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
| `bench` | 현재 화면 한 줄의 렌더 시간: 전체, 스프라이트 없이, VRAM 전부 SRAM일 때, 화면 꺼짐, VRAM 읽기 속도 |

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

- **같은 포트를 연속으로 빠르게 읽을 때** (R800 `INIR`): core1이 `read_table`을 갱신하기 전에 다음 읽기가 오면 이전 값이 나간다.
  대책은 `/WAIT`(GPIO28, 배선은 해 둠)이다.
- **렌더 시간**: VRAM0(물리 0x00000-0x3FFFF)은 SRAM, VRAM1은 PSRAM이다. 비트맵 한 줄은 XIP 스트리밍 DMA로
  읽는다(126MHz). 한계에 가까운 모드: B3 512×424 BD8 줄당 약 29us(한계 31.7us), P1(Petscii Robots)
  평균 60us(한계 63us). BD16 512×424는 아직 측정하지 않았다.
- **P1/P2 이름 표 캐시**: 이름 표 한 줄은 타일 행(8줄)마다 한 번 읽는다. 프레임 중간에 바뀐 이름은 그 타일 행의
  나머지 줄에서 다음 프레임에 보일 수 있다.
- **PSRAM 126MHz**: Core2350B 한 장에서 RX 지연 2–5가 정상(3 사용). 온도 여유는 미확인.
- **252MHz 오버클럭 + 1.20V**: 대부분의 RP2350에서 동작한다고 알려져 있지만 개체별로 확인이 필요하다.

## 참고

- Yamaha V9990 E-VDP-III Application Manual
- openMSX `src/video/v9990/` (레퍼런스 구현, GPL: 코드를 가져오면 이 프로젝트도 GPL이 됨)
- pico-examples `hstx/dvi_out_hstx_encoder`
