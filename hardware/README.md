# MSX V9990 카트리지 PCB (KiCad 8)

RP2350 모듈을 얹는 MSX 카트리지 기판. 회로 설명은 [../docs/hardware.md](../docs/hardware.md) 참고.
같은 생성 스크립트로 두 가지 버전을 만든다(외형, 엣지 커넥터, 구멍 H1/H2, 버스 회로는 동일).

| 버전 | 모듈 | KiCad 프로젝트 | 제조 파일 | 빌드 |
|---|---|---|---|---|
| Pico Plus 2 | Pimoroni Pico Plus 2 (2×20 핀헤더) | `msx_v9990_cart/` | `fab/` | `./build.sh` |
| Core2350B | Waveshare Core2350B **B1(2MB) 또는 B2(8MB) PSRAM판** (2×8 핀헤더 4개) | `msx_v9990_cart_core2350b/` | `fab_core2350b/` | `MSX_VARIANT=core2350b ./build.sh` |

> **검증 상태:** 실기에서 확인한 것은 **Core2350B판 rev 0.3 + Waveshare Core2350B2(PSRAM 8MB)** 뿐이다.
> Pico Plus 2판과 Core2350B1(PSRAM 2MB) 조합은 제작·검증하지 않았다.

### Core2350B 버전 메모

- PSRAM이 모듈 뒷면에 있으므로 **반드시 핀헤더로 띄워서** 장착한다. 모듈 아래에는 배선과 비아가 지나간다.
- USB, BOOTSEL, RESET은 모듈의 **FFC 커넥터 + Waveshare USB 어댑터**를 쓴다(카트리지에는 없음).
  FFC 커넥터는 모듈 왼쪽 위 모서리 쪽에 오며, 케이블은 모듈 위로 지나간다.
- 전원: MSX +5V → D1(쇼트키) → 모듈 VBUS. USB 어댑터 연결 시 USB 전원이 MSX 쪽으로 역류하지 않는다.
- GPIO 할당과 PSRAM CS(GP47)는 Pico Plus 2판과 같다. 펌웨어는 `cmake -DPICO_BOARD=waveshare_core2350b`로 빌드.
- 모듈 핀 배치는 Waveshare 회로도(Core2350B.pdf)의 P1–P4 헤더와 제품 사진의 실크 표기를 대조해 정했다.
  **주문 전에 구멍 확인용 PDF(`fab_core2350b/*_hole_check*.pdf`) 위에 모듈 실물을 올려 확인할 것.**
- 패드 번호: 1–16 = P1, 17–32 = P2, 33–48 = P3, 49–64 = P4.

- **rev 0.2**: VDP 선택 슬라이드 스위치 SW1(C&K JS202011AQN, DPDT 직각형)을 기판 위쪽 가장자리, HDMI 커넥터 오른쪽에 추가했다(레버가 위쪽 가장자리 밖으로 약 1.2mm 나옴).
  - SW1이 디코더 비교값(V9990: 60h–6Fh, V9968: 88h–8Fh)과 GP22를 바꾼다.
  - 보조 부품으로 R10/R11(10k)을 추가했다.
  - GP22는 /WR 대신 SW1을 읽는다. U2는 /WR을 받지만 출력은 비워 둔다.
  - SW1은 스루홀 부품이라 손으로 납땜한다. 카트리지 케이스를 쓰면 레버 자리를 파 줘야 한다.
- **rev 0.3**: 외곽선을 Overrich 카트리지 케이스(Konami형)에 맞췄다.
  - 기준은 MSXLegend의 KC MSX FLASH V3 거버(Overrich 케이스용, github.com/msxlegend/KC-MSX-FLASH)이고, 엣지 핑거 끝선에 맞춰 옮겨 왔다.
  - rev 0.2 대비 차이:
    - 위로 1.96mm, 왼쪽 0.4mm, 오른쪽 0.9mm 넓어졌다.
    - 엣지 탭이 2.07mm 길어졌다(11.89mm).
    - 왼쪽 아래 노치가 2.4mm 높아졌다.
    - 케이스 기둥 구멍 H1/H2가 Ø5.0에서 Ø4.36으로 바뀌고 약 0.7mm 위로 올라갔다.
  - 핑거 위치와 치수(1.5 × 8 mm, 2.54mm 피치)는 그대로다.
  - 엣지 탭이 길어진 만큼 버스 IC U1–U3을 2mm 위로 올렸다(핑거에서 IC로 들어가는 배선 자리).
  - HDMI 커넥터(J2)와 SW1은 위쪽 가장자리를 따라 같이 올라갔고, SW1은 오른쪽 모서리에서 8mm 안쪽(x 116.5 → 108.5)으로 옮겼다. 케이스 위쪽에 HDMI 구멍과 SW1 레버 자리는 직접 파야 한다.
  - 기판 폭이 100mm를 넘어서(101.09mm) JLCPCB 최저가(100 × 100 mm 이하) 구간에서 벗어난다.
- 2층, 1.6mm, 101.09 × 64.77 mm (엣지 탭 64.79 × 11.89 mm)
- 엣지 커넥터 치수는 실제 슬롯에 맞는 것으로 검증된 costarc의 MSX-Protoboard 카트리지를 따르고, 외곽선과 기둥 구멍은 Overrich 케이스용 KC MSX FLASH V3를 따름
- 홀수 핀은 뒷면(B.Cu), 짝수 핀은 앞면(F.Cu)에 있고, 앞면에서 볼 때 1·2번 핀이 오른쪽 끝

## 파일

```
hardware/
  build.sh                 전체 재생성: 회로도 → PCB → 자동 배선 → GND 면 → ERC/DRC → 제조 파일
  gen/design.py            ★ 넷리스트의 원본 (부품, 핀-넷 연결). 회로를 바꿀 때는 여기만 수정
  gen/gen_schematic.py     회로도(.kicad_sch)와 프로젝트 심볼 라이브러리 생성
  gen/gen_pcb.py           배치, 외곽선, 실크, 금지 영역, HDMI 팬아웃
  gen/route.py             Freerouting 자동 배선 (여러 전략을 시도해 미배선 0인 결과 사용)
  gen/finish_pcb.py        GND 팬아웃, 앞뒷면 GND 면, 스티칭 비아
  gen/export_fab.py        거버, 드릴, BOM, CPL, 회로도 PDF
  gen/export_hole_check.py 구멍 위치 확인용 1:1 인쇄 PDF (fab/*_hole_check.pdf)
  msx_v9990_cart/          KiCad 프로젝트 (KiCad에서 열어 보거나 수정 가능)
  fab/                     ★ JLCPCB 주문용 출력물
  reports/                 ERC / DRC 보고서
```

KiCad에서 직접 수정해도 되지만, `build.sh`를 다시 실행하면 덮어쓰니 주의.
생성기를 고치는 쪽을 권장한다.

```sh
cd hardware && ./build.sh      # 2~5분 (자동 배선 포함)
```

필요한 것: KiCad 8 (`kicad-cli`, pcbnew 파이썬 모듈), `~/tools/freerouting/freerouting-2.4.1.jar`, Java 25 (`~/tools/jre25`).

## 검증 결과 (현재 버전)

| 항목 | 결과 |
|---|---|
| ERC | 0 errors, 0 warnings |
| DRC (회로도 일치 검사 포함) | 0 violations, 미연결 0, 풋프린트 오류 0 |
| 넷리스트 대조 | design.py와 회로도 넷 일치 (DRC schematic parity) |

## JLCPCB 주문

업로드: `fab/msx_v9990_cart_gerbers_jlcpcb.zip`

| 항목 | 값 |
|---|---|
| Layers | 2 |
| Dimensions | 101.09 × 64.77 mm (자동 인식) |
| PCB Thickness | **1.6 mm** |
| Surface Finish | ENIG 권장 (HASL도 가능) |
| **Gold Fingers** | **Yes**, **45° finger chamfer(베벨) 선택** |
| Min via hole | 0.3 mm |
| Min track/spacing | 0.15 mm (GND 면의 가장 좁은 부분 0.14 mm, JLC 최소 0.127 mm 이내) |

SMT 조립까지 맡길 경우:
- `fab/msx_v9990_cart_bom_jlcpcb.csv`, `fab/msx_v9990_cart_cpl_jlcpcb.csv` 업로드
- BOM의 `LCSC Part #` 칸은 비워 두었다. JLC 부품 검색에서 MPN으로 찾아 채울 것
- M1(Pico Plus 2)은 손으로 핀헤더 장착, SW1(스루홀 슬라이드 스위치)도 손납땜
- J2(HDMI)는 스루홀 실드 핀이 있어 CPL에서 빠져 있다. 손납땜하거나 JLC 부품이 있으면 추가할 것
- JLC 미리보기에서 SOIC 부품의 회전 방향(1번 핀 위치)을 반드시 확인할 것

## 조립 순서

1. SMD 부품: U1–U5 (SOIC), R1–R9, C1–C7, D1 (방향: 캐소드 띠가 Pico 쪽)
2. J2 HDMI 커넥터
3. M1: **2×20 핀헤더로 장착** (소켓 또는 직접 납땜). 모듈 아래로 신호선과 비아가 지나가므로 모듈을 기판에 바로 붙이지 말 것
4. 펌웨어 기록 (USB-C, BOOTSEL) 후 MSX에 삽입. **실크의 "LABEL SIDE / PINS 2-50" 면이 카트리지 라벨 방향**

## 주문 전에 직접 확인할 것

자동 검사(ERC/DRC)로는 잡을 수 없는 항목들이다.

1. **엣지 커넥터 방향**: 이 기판은 msx.org 위키와 두 오픈소스 카트리지 설계의 공통 규칙을 따랐다(짝수 핀이 라벨면).
   가지고 있는 상용 카트리지에서 1번 핀(/CS1) 쪽이 어느 면 어느 끝인지 한 번 대조할 것.
2. **HDMI 커넥터 품번**: 풋프린트는 Amphenol 10029449-x01xLF 기준이다. 다른 제품을 사면 핀 간격과 실드 위치가 다를 수 있다.
3. **Pico Plus 2 핀배치**: Raspberry Pi Pico와 같은 40핀 배치(GND 3·8·13·18·23·28·33·38번, VSYS 39번, 3V3 36번)라고 가정했다. Pimoroni 핀아웃 문서로 확인할 것.
4. **슬롯 높이 여유**: 핀헤더로 올린 Pico 높이는 약 10mm다. 사용할 MSX의 슬롯 덮개와 간섭하지 않는지 확인할 것.
5. **/BUSDIR**: 오픈 드레인(74LVC07)으로 구동하며 본체에 풀업이 있다고 가정했다.
