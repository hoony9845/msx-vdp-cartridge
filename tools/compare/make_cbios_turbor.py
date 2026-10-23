"""
Build an openMSX test machine "C-BIOS_turboR_test": the C-BIOS MSX2+ machine
turned into an MSX turbo R (R800 CPU, S1990, E6 timer, pause port, PCM;
C-BIOS already has the F4 reset status register) for
running turbo R software without Panasonic system ROMs.

The C-BIOS main ROM (BSD licensed) is patched:
  0x002D  MSX version 2 -> 3 (turbo R)
  0x0180  CHGCPU: A bits 1-0 = 0 Z80, 1 R800 ROM mode, 2 R800 DRAM mode
          (DRAM mode is treated as ROM mode); writes S1990 register 6
          (bit 5 = 1 selects the Z80)
  0x0183  GETCPU: returns 0 (Z80) or 1 (R800)
The rest of C-BIOS stays MSX2+; this is only meant for testing V9990 output.

    python3 make_cbios_turbor.py <openmsx_share_dir> <openmsx_system_dir>
      e.g. ~/.cache/msx_v9990_openmsx16/.openMSX/share  ~/tools/openmsx16/root/usr/share/openmsx
"""
import os
import re
import sys

user_share, system = sys.argv[1], sys.argv[2]
NAME = "C-BIOS_turboR_test"
src_xml = os.path.join(system, "machines", "C-BIOS_MSX2+.xml")
src_rom = os.path.join(system, "machines", "C-BIOS_MSX2+", "roms", "cbios_main_msx2+.rom")
if not os.path.exists(src_rom):
    src_rom = os.path.join(system, "systemroms", "cbios_main_msx2+.rom")

rom = bytearray(open(src_rom, "rb").read())
assert rom[0x2D] == 2, "unexpected C-BIOS main ROM"
assert all(b == 0 for b in rom[0x180:0x1B0]), "0x0180 area is not free"

chgcpu = bytes([
    0xF5,              # push af
    0xE6, 0x03,        # and 3
    0x06, 0x20,        # ld b,20h      ; Z80
    0x28, 0x02,        # jr z,+2
    0x06, 0x40,        # ld b,40h      ; R800 (ROM mode)
    0x3E, 0x06,        # ld a,6
    0xD3, 0xE4,        # out (0E4h),a  ; S1990 register select
    0x78,              # ld a,b
    0xD3, 0xE5,        # out (0E5h),a  ; S1990 register 6
    0xF1,              # pop af
    0xC9,              # ret
])
getcpu = bytes([
    0x3E, 0x06,        # ld a,6
    0xD3, 0xE4,        # out (0E4h),a
    0xDB, 0xE5,        # in a,(0E5h)
    0xE6, 0x20,        # and 20h
    0x3E, 0x00,        # ld a,0        ; flags unchanged
    0xC0,              # ret nz        ; Z80
    0x3C,              # inc a         ; R800
    0xC9,              # ret
])
code = 0x0186
rom[0x2D] = 3
rom[0x180:0x183] = bytes([0xC3, code & 0xFF, code >> 8])
g = code + len(chgcpu)
rom[0x183:0x186] = bytes([0xC3, g & 0xFF, g >> 8])
rom[code:code + len(chgcpu)] = chgcpu
rom[g:g + len(getcpu)] = getcpu

out_dir = os.path.join(user_share, "machines", NAME)
os.makedirs(os.path.join(out_dir, "roms"), exist_ok=True)
with open(os.path.join(out_dir, "roms", "cbios_main_turbor_test.rom"), "wb") as f:
    f.write(rom)

xml = open(src_xml).read()
xml = xml.replace("<type>MSX2+</type>", "<type>MSXturboR</type>")
xml = re.sub(r"<description>.*?</description>",
             "<description>Test machine: C-BIOS MSX2+ patched to report MSX turbo R, "
             "with R800, S1990, E6 timer, pause and PCM (msx_v9990 tools).</description>", xml)
xml = re.sub(r"<sha1>12ebcebc65de0e8927c75d7b9b38e53ade65ce7c</sha1>\s*<filename>cbios_main_msx2\+\.rom</filename>",
             "<filename>roms/cbios_main_turbor_test.rom</filename>", xml)
assert "cbios_main_turbor_test.rom" in xml
turbo = """
    <S1990 id="S1990">
      <io base="0xE4" num="2"/>
    </S1990>
    <E6Timer id="Turbo-R timer">
      <io base="0xE6" num="2"/>
    </E6Timer>
    <TurboRPause id="TurboRPause">
      <io base="0xA7" num="1"/>
    </TurboRPause>
    <TurboRPCM id="PCM">
      <io base="0xA4" num="2"/>
      <sound>
        <volume>21000</volume>
      </sound>
    </TurboRPCM>
  </devices>"""
xml = xml.replace("  </devices>", turbo, 1)
with open(os.path.join(out_dir, "hardwareconfig.xml"), "w") as f:
    f.write(xml)
print("wrote", out_dir)
