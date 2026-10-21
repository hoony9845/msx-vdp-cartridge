# MSX V9990 카트리지 PCB (KiCad 8)

Pimoroni Pico Plus 2를 얹는 MSX 카트리지 기판. 회로 설명은 [../docs/hardware.md](../docs/hardware.md) 참고.

- 2층, 1.6mm, 99.8 × 64.0 mm (엣지 탭 65.45 × 11 mm)
- 외곽선과 엣지 커넥터 치수는 실제 슬롯에 맞는 것으로 검증된 costarc의 MSX-Protoboard 카트리지 외형을 따름
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
| 넷리스트 대조 | design.py와 회로도 넷 101개 일치 |

## JLCPCB 주문

업로드: `fab/msx_v9990_cart_gerbers_jlcpcb.zip`

| 항목 | 값 |
|---|---|
| Layers | 2 |
| Dimensions | 99.8 × 64 mm (자동 인식) |
| PCB Thickness | **1.6 mm** |
| Surface Finish | ENIG 권장 (HASL도 가능) |
| **Gold Fingers** | **Yes**, **45° finger chamfer(베벨) 선택** |
| Min via hole | 0.3 mm |
| Min track/spacing | 0.15 mm (GND 면의 가장 좁은 부분 0.14 mm, JLC 최소 0.127 mm 이내) |

SMT 조립까지 맡길 경우:
- `fab/msx_v9990_cart_bom_jlcpcb.csv`, `fab/msx_v9990_cart_cpl_jlcpcb.csv` 업로드
- BOM의 `LCSC Part #` 칸은 비워 두었다. JLC 부품 검색에서 MPN으로 찾아 채울 것
- M1(Pico Plus 2)은 손으로 핀헤더 장착
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
