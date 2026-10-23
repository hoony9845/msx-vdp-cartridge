# V9990 코어 ↔ openMSX 비교 환경

우리 V9990 코어(`core/`)의 화면 출력을 openMSX의 GFX9000 에뮬레이션과 **V9990 도트 단위로** 비교한다.
기판 없이 PC에서 호환성을 맞추기 위한 도구다.

## 구성

| 파일 | 역할 |
|---|---|
| `scenarios.py` | 테스트 장면을 포트 트레이스로 생성 (P1, P2, 스프라이트, 스크롤, 비트맵, LMMV/LMMM, 보정용) |
| `../../host/replay.c` | 트레이스를 우리 코어로 재생하고 캡처 (`build-host/replay`) |
| `run_openmsx.tcl`, `run_openmsx.sh` | 같은 트레이스를 openMSX(GFX9000)에서 실행하고 캡처 (창 없이) |
| `compare.py` | 보정 캡처로 화면 위치와 도트 열을 찾아, 두 결과를 도트 단위로 비교 |
| `run_compare.sh` | 위 과정을 한 번에 실행: `build-compare/report/` |
| `record.tcl`, `record.sh` | **실제 소프트웨어**를 openMSX에서 실행하며 V9990 포트 접근을 기록하고 주기적으로 캡처 |
| `compare_trace.sh` | 기록한 트레이스를 우리 코어로 재생해 openMSX 캡처와 비교 |
| `selftest_record.*` | 기록 경로 자체 테스트 (Z80 프로그램 → 기록 → 재생 → 비교) |
| `shot_window.py` | 기록 재생 시 각 캡처 주변 −4~+2 프레임도 함께 렌더링 (캡처 시점 차이 흡수) |
| `inputs/*.tcl` | 게임별 자동 입력 스크립트 (`V9990_EXTRA_SCRIPT`로 지정) |
| `games/chasehq.sh` | 실제 게임 회귀 테스트: Chase H.Q. V9990 (ROM은 공식 릴리스에서 받아 SHA1 확인) |
| `games/anotherworld.sh` | 실제 게임 회귀 테스트: Another World (터보 R + V9990 + OPL4, ROM 경로를 인자로) |
| `setup_machines.sh`, `make_cbios_turbor.py` | 시스템 ROM 없이 쓰는 테스트 머신: C-BIOS를 고친 터보 R(R800), 웨이브 ROM 없는 MoonSound |
| `compare_vram.py` | 기록 시점마다 openMSX VRAM과 우리 VRAM을 바이트 단위로 비교 (`V9990_DUMP_VRAM=1`, `REPLAY_DUMP_VRAM=1`) |

트레이스 형식(한 줄에 한 동작): `w <포트> <값>`, `r <포트>`, `frame`, `wait_ce`, `shot <이름>`.
자세한 내용은 `host/replay.c` 머리말 참고.

## 준비 (한 번, sudo 불필요)

openMSX **16.0**(Debian 11 빌드)을 `~/tools/openmsx16`에 풀어서 쓴다. 설치 명령은 `openmsx_env.sh` 머리말에 있다.

- Ubuntu 20.04 기본 패키지인 0.15에는 2019–2020년의 V9990 P1/P2 렌더링 수정이 빠져 있어 기준으로 쓰지 않는다.
- openMSX 설정은 `~/.cache/msx_v9990_openmsx16`에 따로 둔다.
  - `horizontal_stretch 320`: V9990 1도트가 정확히 2픽셀이 되어 보간이 생기지 않는다.
  - 프레임 건너뛰기, 스캔라인, 번짐, 감마 효과는 모두 끈다.

## 사용법

```sh
# 1) 테스트 장면 비교 (약 15초)
tools/compare/run_compare.sh
#    -> build-compare/report/report.md, *_ours_openmsx_diff.png (우리 | openMSX | 차이=빨강)

# 2) 실제 게임/데모 기록 (창이 뜨고 직접 플레이, openMSX를 닫으면 종료)
tools/compare/record.sh rec/mygame -diska mygame.dsk
V9990_FRAMES=1800 V9990_SHOT_EVERY=60 tools/compare/record.sh rec/demo -cart demo.rom
#    기본 머신은 C-BIOS MSX2+ (BASIC 없음). 실제 MSX BIOS가 필요하면 시스템 ROM을 준비하고
#    MSX_MACHINE=<openMSX 머신 이름> 을 지정한다.

# 3) 기록과 우리 코어 비교
tools/compare/compare_trace.sh rec/mygame
#    -> rec/mygame/report/report.md

# 기록 경로 자체 테스트
tools/compare/selftest_record.sh
```

## 현재 결과

**테스트 장면:** 모두 openMSX 16.0과 도트 단위로 완전히 일치한다.
대상은 P1 2레이어, 스크롤, P2, 스프라이트(앞/뒤 우선순위 포함), BD16, BP4(B3), LMMV, LMMM이다.
기록 자체 테스트(Z80 실행 → 기록 → 재생)도 일치한다.

**실제 게임 1: Chase H.Q. V9990** (`games/chasehq.sh`, 90초 자동 플레이, V9990 쓰기 91만 회, 캡처 78장)

| 결과 | 장수 | 내용 |
|---|---|---|
| 일치 | 74 | 로고, 타이틀, 크레딧, 브리핑, 주행 화면 |
| 일치 (티어링) | 2 | 모든 줄이 인접 프레임 중 하나와 일치 (게임이 스캔 중에 다시 그림) |
| 다름 | 2 | 화면 스캔 타이밍 차이. **이 시점의 VRAM은 openMSX와 동일**함을 확인 |

- 사용 기능: B1 모드 BP6, LMMM 18,493회(투명색 4,343회), LMMV 4,090회, CMMC 84회, CMMM 40회.
- **VRAM 비교** (`compare_vram.py`, 73개 시점): 69개 시점에서 openMSX와 동일했다.
  나머지 4개 시점은 1~2줄(22–87바이트)만 달랐는데, openMSX에서 아직 실행 중이던 커맨드로 보인다.

**실제 게임 2: Another World** (Edd Biddulph, 터보 R + V9990 + OPL4) (`games/anotherworld.sh`, 4분 인트로, V9990 쓰기 267만 회)

| 결과 | 장수 |
|---|---|
| 일치 | **113 / 113** (로고, 크레딧, 폴리곤 인트로 영상 전체) |

- 사용 기능: B2 모드(384×240, MCS=1) BP4, LMMV 16,128회(XOR), LMMM, LMMC.
- 실행 환경: 시스템 ROM 없이 C-BIOS를 터보 R로 고친 테스트 머신(R800)과 웨이브 ROM 없는 MoonSound를 썼다.
- B2 모드에서 openMSX는 첫 줄을 화면 밖으로 잘라 보여 준다. 그래서 세로 위치도 `calib_rows_*`로 줄 단위 보정한다.

## 알려진 한계

- **캡처 시점:** openMSX의 캡처는 기록 위치보다 몇 프레임 앞의 화면일 수 있다. `compare_trace.sh`는 −4~+2 프레임 중 가장 잘 맞는 것을 고른다(`--window`).
  화면 중간의 레지스터 변경(래스터 효과)과 실행 중인 커맨드는 여전히 차이를 낼 수 있다.
- **전원 투입 상태:** 소프트웨어가 V9990을 설정하기 전의 화면은 실기에서도 정해지지 않은 값이다(openMSX는 흰색, 우리 코어는 검은색). `V9990_SKIP_BEFORE`로 제외한다.
- **커맨드 실행 시간:** 우리 코어는 커맨드를 즉시 끝낸다. 실행 중에 찍힌 캡처는 다를 수 있다.
- **IN 처리:** openMSX 실행기의 `r` 줄은 디버그 읽기라 읽기 부작용이 없다. 부작용은 기록·재생 경로에서만 정확하다.
- **기준의 한계:** openMSX도 V9990의 일부 동작(P1/P2 모드의 커맨드 좌표계 등)은 확실하지 않다고 적어 두고 있다. 일치한다는 것은 openMSX와 같다는 뜻이고, 실기와 같다는 보장은 아니다.
