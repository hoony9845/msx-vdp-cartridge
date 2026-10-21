"""
Single source of truth for the cartridge netlist.

Both gen_schematic.py and gen_pcb.py read this file, so the schematic and
the PCB always describe the same circuit (DRC checks schematic parity).
See docs/hardware.md for the reasoning behind the circuit.
"""

import os

# Which RP2350 module the cartridge carries:
#   picoplus2 : Pimoroni Pico Plus 2 (Pico form factor, USB-C on the module)
#   core2350b : Waveshare Core2350B (25.4mm square, 64 pins, PSRAM on the back:
#               mount on pin headers). USB/BOOTSEL/RESET live on Waveshare's
#               FFC USB adapter. Needs the B1 (2MB) or B2 (8MB) PSRAM version.
VARIANT = os.environ.get("MSX_VARIANT", "picoplus2")
assert VARIANT in ("picoplus2", "core2350b"), VARIANT

PROJECT = "msx_v9990_cart" if VARIANT == "picoplus2" else "msx_v9990_cart_core2350b"
LIB = "msx_v9990"          # project symbol / footprint library nickname

# ---------------------------------------------------------------------------
# MSX cartridge slot (pin -> signal). Odd pins on the back (B.Cu),
# even pins on the front (F.Cu). Source: msx.org wiki "MSX Cartridge slot".
# ---------------------------------------------------------------------------
MSX_SLOT = {
    1: "CS1", 2: "CS2", 3: "CS12", 4: "SLTSL", 5: "RSV5", 6: "RFSH",
    7: "WAIT", 8: "INT", 9: "M1", 10: "BUSDIR", 11: "IORQ", 12: "MERQ",
    13: "WR", 14: "RD", 15: "RESET", 16: "RSV16", 17: "A9", 18: "A15",
    19: "A11", 20: "A10", 21: "A7", 22: "A6", 23: "A12", 24: "A8",
    25: "A14", 26: "A13", 27: "A1", 28: "A0", 29: "A3", 30: "A2",
    31: "A5", 32: "A4", 33: "D1", 34: "D0", 35: "D3", 36: "D2",
    37: "D5", 38: "D4", 39: "D7", 40: "D6", 41: "GND", 42: "CLOCK",
    43: "GND", 44: "SW1", 45: "+5V", 46: "SW2", 47: "+5V", 48: "+12V",
    49: "SOUNDIN", 50: "-12V",
}

# Slot signals this design uses, mapped to net names. Active-low signals get
# an _N suffix. Everything else on the slot is left unconnected.
SLOT_NETS = {
    "A0": "MSX_A0", "A1": "MSX_A1", "A2": "MSX_A2", "A3": "MSX_A3",
    "A4": "MSX_A4", "A5": "MSX_A5", "A6": "MSX_A6", "A7": "MSX_A7",
    "D0": "MSX_D0", "D1": "MSX_D1", "D2": "MSX_D2", "D3": "MSX_D3",
    "D4": "MSX_D4", "D5": "MSX_D5", "D6": "MSX_D6", "D7": "MSX_D7",
    "IORQ": "IORQ_N", "M1": "M1_N", "RD": "RD_N", "WR": "WR_N",
    "RESET": "RESET_N", "INT": "INT_N", "WAIT": "WAIT_N",
    "BUSDIR": "BUSDIR_N", "GND": "GND", "+5V": "+5V",
}

POWER_NETS = {"GND", "+5V", "+3V3"}

# ---------------------------------------------------------------------------
# Custom symbols: name -> (description, left pins, right pins)
# pin = (number, name, electrical type); None = spacer
# ---------------------------------------------------------------------------
I, O, B, P, OC, PI, PO = ("input", "output", "bidirectional", "passive",
                          "open_collector", "power_in", "power_out")

SYMBOLS = {
    "74LVC245": ("Octal bus transceiver, 3-state, 5V tolerant inputs",
        [("1", "DIR", I), ("19", "~{OE}", I), None,
         ("2", "A1", B), ("3", "A2", B), ("4", "A3", B), ("5", "A4", B),
         ("6", "A5", B), ("7", "A6", B), ("8", "A7", B), ("9", "A8", B)],
        [("20", "VCC", PI), None, None,
         ("18", "B1", B), ("17", "B2", B), ("16", "B3", B), ("15", "B4", B),
         ("14", "B5", B), ("13", "B6", B), ("12", "B7", B), ("11", "B8", B),
         None, ("10", "GND", PI)]),
    "74HCT688": ("8-bit identity comparator",
        [("1", "~{G}", I), None,
         ("2", "P0", I), ("4", "P1", I), ("6", "P2", I), ("8", "P3", I),
         ("11", "P4", I), ("13", "P5", I), ("15", "P6", I), ("17", "P7", I)],
        [("20", "VCC", PI), None,
         ("3", "Q0", I), ("5", "Q1", I), ("7", "Q2", I), ("9", "Q3", I),
         ("12", "Q4", I), ("14", "Q5", I), ("16", "Q6", I), ("18", "Q7", I),
         None, ("19", "~{P=Q}", O), ("10", "GND", PI)]),
    "74HCT32": ("Quad 2-input OR gate",
        [("1", "1A", I), ("2", "1B", I), ("4", "2A", I), ("5", "2B", I),
         ("9", "3A", I), ("10", "3B", I), ("12", "4A", I), ("13", "4B", I)],
        [("14", "VCC", PI), ("3", "1Y", O), ("6", "2Y", O), ("8", "3Y", O),
         ("11", "4Y", O), None, None, ("7", "GND", PI)]),
    "74LVC07": ("Hex buffer, open drain outputs, 5V tolerant",
        [("1", "1A", I), ("3", "2A", I), ("5", "3A", I), ("9", "4A", I),
         ("11", "5A", I), ("13", "6A", I)],
        [("14", "VCC", PI), ("2", "1Y", OC), ("4", "2Y", OC), ("6", "3Y", OC),
         ("8", "4Y", OC), ("10", "5Y", OC), ("12", "6Y", OC), ("7", "GND", PI)]),
    "Pico_Plus_2": ("Pimoroni Pico Plus 2 (RP2350B, 16MB flash, 8MB PSRAM), Pico pinout",
        [("1", "GP0", B), ("2", "GP1", B), ("3", "GND", PI), ("4", "GP2", B),
         ("5", "GP3", B), ("6", "GP4", B), ("7", "GP5", B), ("8", "GND", PI),
         ("9", "GP6", B), ("10", "GP7", B), ("11", "GP8", B), ("12", "GP9", B),
         ("13", "GND", PI), ("14", "GP10", B), ("15", "GP11", B), ("16", "GP12", B),
         ("17", "GP13", B), ("18", "GND", PI), ("19", "GP14", B), ("20", "GP15", B)],
        [("40", "VBUS", P), ("39", "VSYS", PI), ("38", "GND", PI), ("37", "3V3_EN", I),
         ("36", "3V3", PO), ("35", "ADC_VREF", P), ("34", "GP28", B), ("33", "GND", PI),
         ("32", "GP27", B), ("31", "GP26", B), ("30", "RUN", I), ("29", "GP22", B),
         ("28", "GND", PI), ("27", "GP21", B), ("26", "GP20", B), ("25", "GP19", B),
         ("24", "GP18", B), ("23", "GND", PI), ("22", "GP17", B), ("21", "GP16", B)]),
    "MSX_Cartridge": ("MSX cartridge edge connector (50 pins, 2.54mm)",
        [(str(n), MSX_SLOT[n], P) for n in range(1, 50, 2)],
        [(str(n), MSX_SLOT[n], P) for n in range(2, 51, 2)]),
    "HDMI_A": ("HDMI type A receptacle",
        [(str(n), name, P) for n, name in [
            (1, "D2+"), (2, "D2S"), (3, "D2-"), (4, "D1+"), (5, "D1S"), (6, "D1-"),
            (7, "D0+"), (8, "D0S"), (9, "D0-"), (10, "CK+"), (11, "CKS"), (12, "CK-"),
            (13, "CEC"), (14, "UTILITY"), (15, "SCL"), (16, "SDA"), (17, "GND"),
            (18, "+5V"), (19, "HPD")]],
        [None, None, None, None, None, None, None, None, None,
         ("SH", "SHIELD", P)]),
}

# ---------------------------------------------------------------------------
# Components: ref -> dict(symbol, value, footprint, mpn, pins{pin: net})
# symbol "Device:R" etc. come from the KiCad standard libraries.
# A pin missing from `pins` gets a no-connect flag.
# ---------------------------------------------------------------------------
FP_SOIC20W = "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm"
FP_SOIC14 = "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm"
FP_R = "Resistor_SMD:R_0805_2012Metric"
FP_C = "Capacitor_SMD:C_0805_2012Metric"

COMPONENTS = {}

def comp(ref, symbol, value, footprint, mpn, pins, description=""):
    COMPONENTS[ref] = dict(symbol=symbol, value=value, footprint=footprint,
                           mpn=mpn, pins=pins, description=description)

# J1: MSX edge connector
comp("J1", f"{LIB}:MSX_Cartridge", "MSX_Cartridge", f"{LIB}:MSX_Cartridge_Edge", "",
     {str(n): SLOT_NETS[sig] for n, sig in MSX_SLOT.items() if sig in SLOT_NETS})

# U3: port decoder. /CS = 0 when A7-A4 = 0110, /IORQ = 0, /M1 = 1
comp("U3", f"{LIB}:74HCT688", "74HCT688", FP_SOIC20W, "74HCT688D", {
    "1": "GND",
    "2": "MSX_A7", "3": "GND",
    "4": "MSX_A6", "5": "+5V",
    "6": "MSX_A5", "7": "+5V",
    "8": "MSX_A4", "9": "GND",
    "11": "IORQ_N", "12": "GND",
    "13": "M1_N", "14": "+5V",
    "15": "GND", "16": "GND", "17": "GND", "18": "GND",
    "19": "CS_N", "10": "GND", "20": "+5V"})

# U1: data bus transceiver. A = MSX, B = RP2350. DIR = /RD (read: B->A)
comp("U1", f"{LIB}:74LVC245", "74LVC245", FP_SOIC20W, "SN74LVC245ADWR", {
    "1": "RD_N", "19": "CS_N",
    **{str(2 + i): f"MSX_D{i}" for i in range(8)},
    **{str(18 - i): f"GP_D{i}" for i in range(8)},
    "10": "GND", "20": "+3V3"})

# U2: address / control input buffer, always A->B
u2_in = ["MSX_A0", "MSX_A1", "MSX_A2", "MSX_A3", "RD_N", "WR_N", "RESET_N", "CS_N"]
u2_out = ["GP_A0", "GP_A1", "GP_A2", "GP_A3", "GP_RD_N", "GP_WR_N", "GP_RESET_N", "GP_CS_N"]
comp("U2", f"{LIB}:74LVC245", "74LVC245", FP_SOIC20W, "SN74LVC245ADWR", {
    "1": "+3V3", "19": "GND",
    **{str(2 + i): n for i, n in enumerate(u2_in)},
    **{str(18 - i): n for i, n in enumerate(u2_out)},
    "10": "GND", "20": "+3V3"})

# U5: /BUSDIR request = /CS OR /RD (low while the MSX reads our ports)
comp("U5", f"{LIB}:74HCT32", "74HCT32", FP_SOIC14, "74HCT32D", {
    "1": "CS_N", "2": "RD_N", "3": "BUSDIR_REQ_N",
    "4": "GND", "5": "GND", "9": "GND", "10": "GND", "12": "GND", "13": "GND",
    "7": "GND", "14": "+5V"})

# U4: open-drain drivers for /INT, /WAIT, /BUSDIR
comp("U4", f"{LIB}:74LVC07", "74LVC07", FP_SOIC14, "SN74LVC07AD", {
    "1": "GP_INT", "2": "INT_N",
    "3": "GP_WAIT", "4": "WAIT_N",
    "5": "BUSDIR_REQ_N", "6": "BUSDIR_N",
    "9": "+3V3", "11": "+3V3", "13": "+3V3",
    "7": "GND", "14": "+3V3"})

# RP2350 GPIO -> net (= firmware/src/board_pins.h, identical for both modules)
GPIO_NETS = {
    0: "GP_A0", 1: "GP_A1", 2: "GP_A2", 3: "GP_A3",
    4: "GP_D0", 5: "GP_D1", 6: "GP_D2", 7: "GP_D3",
    8: "GP_D4", 9: "GP_D5", 10: "GP_D6", 11: "GP_D7",
    12: "DVI_D0_P", 13: "DVI_D0_N", 14: "DVI_CK_P", 15: "DVI_CK_N",
    16: "DVI_D2_P", 17: "DVI_D2_N", 18: "DVI_D1_P", 19: "DVI_D1_N",
    20: "GP_CS_N", 21: "GP_RD_N", 22: "GP_WR_N",
    26: "GP_RESET_N", 27: "GP_INT", 28: "GP_WAIT",
}

# Waveshare Core2350B: four 2x8 headers P1..P4 (schematic Core2350B.pdf),
# numbered here as pads 1-16 (P1), 17-32 (P2), 33-48 (P3), 49-64 (P4).
CORE_HEADERS = {
    "P1": ["GND", "GP0", "GP2", "GP1", "GP4", "GP3", "GP6", "GP5",
           "GP8", "GP7", "GP10", "GP9", "GP12", "GP11", "GP14", "GP13"],
    "P2": ["GND", "GP45", "GP47", "GP46", "BOOTSEL", "SWD", "USBD_N", "GND",
           "USBD_P", "SWCLK", "RUN", "ADC_VREF", "3V3_EN", "GND", "VBUS", "3V3"],
    "P3": ["GND", "GP15", "GP17", "GP16", "GP19", "GP18", "GP21", "GP20",
           "GP23", "GP22", "GP25", "GP24", "GP27", "GP26", "GP29", "GP28"],
    "P4": ["GND", "GP30", "GP32", "GP31", "GP34", "GP33", "GP36", "GP35",
           "GP38", "GP37", "GP40", "GP39", "GP42", "GP41", "GP44", "GP43"],
}
CORE_PADS = {}          # pad number -> signal name
for h, (name, sigs) in enumerate(CORE_HEADERS.items()):
    for i, sig in enumerate(sigs):
        CORE_PADS[str(h * 16 + i + 1)] = sig


def _core_type(sig):
    if sig == "GND" or sig == "VBUS":
        return PI
    if sig == "3V3":
        return PO
    if sig in ("BOOTSEL", "RUN", "3V3_EN"):
        return I
    if sig in ("ADC_VREF",):
        return P
    return B


SYMBOLS["Core2350B"] = (
    "Waveshare Core2350B (RP2350B, 16MB flash, PSRAM on B1/B2), four 2x8 headers",
    [(n, f"{sig}" if sig != "GND" else "GND", _core_type(sig)) for n, sig in
     sorted(CORE_PADS.items(), key=lambda x: int(x[0])) if int(n) <= 32],
    [(n, sig, _core_type(sig)) for n, sig in
     sorted(CORE_PADS.items(), key=lambda x: int(x[0])) if int(n) > 32],
)

if VARIANT == "core2350b":
    core = {}
    for pad, sig in CORE_PADS.items():
        if sig.startswith("GP") and int(sig[2:]) in GPIO_NETS:
            core[pad] = GPIO_NETS[int(sig[2:])]
        elif sig in ("GND", "VBUS"):
            core[pad] = sig
        elif sig == "3V3":
            core[pad] = "+3V3"
    comp("M1", f"{LIB}:Core2350B", "Waveshare Core2350B", f"{LIB}:Waveshare_Core2350B",
         "Core2350B2", core)

    # Power: MSX +5V -> module VBUS. USB (firmware upload) comes in through the
    # module's FFC connector and Waveshare's USB adapter board, which also has
    # the BOOTSEL/RESET buttons; D1 keeps USB power out of the MSX.
    comp("D1", "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123", "B5819W",
         {"1": "VBUS", "2": "+5V"})

# M1: Pimoroni Pico Plus 2 (pin map = firmware/src/board_pins.h)
pico = {
    "1": "GP_A0", "2": "GP_A1", "4": "GP_A2", "5": "GP_A3",
    "6": "GP_D0", "7": "GP_D1", "9": "GP_D2", "10": "GP_D3",
    "11": "GP_D4", "12": "GP_D5", "14": "GP_D6", "15": "GP_D7",
    "16": "DVI_D0_P", "17": "DVI_D0_N", "19": "DVI_CK_P", "20": "DVI_CK_N",
    "21": "DVI_D2_P", "22": "DVI_D2_N", "24": "DVI_D1_P", "25": "DVI_D1_N",
    "26": "GP_CS_N", "27": "GP_RD_N", "29": "GP_WR_N",
    "31": "GP_RESET_N", "32": "GP_INT", "34": "GP_WAIT",
    "36": "+3V3", "39": "VSYS",
}
for gnd in ("3", "8", "13", "18", "23", "28", "33", "38"):
    pico[gnd] = "GND"
if VARIANT == "picoplus2":
    comp("M1", f"{LIB}:Pico_Plus_2", "Pimoroni Pico Plus 2",
         "Module:RaspberryPi_Pico_Common_THT", "", pico)

    # D1: MSX +5V -> VSYS (blocks back-feeding the MSX from USB)
    comp("D1", "Device:D_Schottky", "B5819W", "Diode_SMD:D_SOD-123", "B5819W",
         {"1": "VSYS", "2": "+5V"})

# HDMI (DVI) output with 270R series resistors (~500mV TMDS swing into 50R)
DVI_PAIRS = [("D2", "1", "3"), ("D1", "4", "6"), ("D0", "7", "9"), ("CK", "10", "12")]
hdmi = {"2": "GND", "5": "GND", "8": "GND", "11": "GND", "17": "GND",
        "18": "+5V", "SH": "GND"}
r = 1
for lane, p_pin, n_pin in DVI_PAIRS:
    for pol, pin in (("P", p_pin), ("N", n_pin)):
        hdmi[pin] = f"HDMI_{lane}_{pol}"
        comp(f"R{r}", "Device:R", "270", "Resistor_SMD:R_0603_1608Metric", "0603WAF2700T5E",
             {"1": f"DVI_{lane}_{pol}", "2": f"HDMI_{lane}_{pol}"})
        r += 1
comp("J2", f"{LIB}:HDMI_A", "HDMI_A",
     "Connector_Video:HDMI_A_Amphenol_10029449-x01xLF_Horizontal",
     "10029449-001RLF", hdmi)

# /CS pull-up so U1 stays disabled while the MSX is off and USB powers the Pico
comp("R9", "Device:R", "10k", FP_R, "0805W8F1002T5E", {"1": "+3V3", "2": "CS_N"})

# Decoupling
for i, (ref, net) in enumerate([("U1", "+3V3"), ("U2", "+3V3"), ("U3", "+5V"),
                                ("U4", "+3V3"), ("U5", "+5V")]):
    comp(f"C{i + 1}", "Device:C", "100n", FP_C, "CL21B104KBCNNNC",
         {"1": net, "2": "GND"})
comp("C6", "Device:C", "10u", FP_C, "CL21A106KAYNNNE", {"1": "+5V", "2": "GND"})
comp("C7", "Device:C", "10u", FP_C, "CL21A106KAYNNNE", {"1": "+3V3", "2": "GND"})

# Nets that need a PWR_FLAG (driven only by passive pins)
PWR_FLAG_NETS = (["+5V", "GND", "VSYS"] if VARIANT == "picoplus2"
                 else ["+5V", "GND", "VBUS"])
