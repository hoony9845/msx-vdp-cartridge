# Third-party notices

This repository is MIT (`LICENSE`) except:

- `hardware/` — CERN-OHL-P v2 (`hardware/LICENSE`).
- The V9968 core (`core/v9968.c`, `core/v9968.h`, `core/v9968_cmd.c`,
  `core/v9968_render.c`) follows HRA!'s V9968 RTL closely and is
  distributed under the conditions of that RTL (section 1 below), including
  its non-commercial clause, until the author permits otherwise.

## 1. HRA! V9968 RTL (Takayuki Hara)

Reference for the V9968 core: <https://github.com/hra1129/V9968_Cartridge>,
`fpga/V9968_Cartridge_TangNano20K/src/v9968/*.v` (vdp_command.v,
vdp_cpu_interface.v, vdp_timing_control_*.v, vdp_sprite_*.v, ...). No RTL
file is included in this repository; `tools/compare/v9968/rtl` compiles
them from a separate clone. The notice of those files:

```
Copyright (C) 2025 Takayuki Hara

本ソフトウェアおよび本ソフトウェアに基づいて作成された派生物は、以下の条件を
満たす場合に限り、再頒布および使用が許可されます。

1.ソースコード形式で再頒布する場合、上記の著作権表示、本条件一覧、および下記
  免責条項をそのままの形で保持すること。
2.バイナリ形式で再頒布する場合、頒布物に付属のドキュメント等の資料に、上記の
  著作権表示、本条件一覧、および下記免責条項を含めること。
3.書面による事前の許可なしに、本ソフトウェアを販売、および商業的な製品や活動
  に使用しないこと。

本ソフトウェアは、著作権者によって「現状のまま」提供されています。著作権者は、
特定目的への適合性の保証、商品性の保証、またそれに限定されない、いかなる明示
的もしくは暗黙な保証責任も負いません。著作権者は、事由のいかんを問わず、損害
発生の原因いかんを問わず、かつ責任の根拠が契約であるか厳格責任であるか（過失
その他の）不法行為であるかを問わず、仮にそのような損害が発生する可能性を知ら
されていたとしても、本ソフトウェアの使用によって発生した（代替品または代用サ
ービスの調達、使用の喪失、データの喪失、利益の喪失、業務の中断も含め、またそ
れに限定されない）直接損害、間接損害、偶発的な損害、特別損害、懲罰的損害、ま
たは結果損害について、一切責任を負わないものとします。

Note that above Japanese version license is the formal document.
The following translation is only for reference.

Redistribution and use of this software or any derivative works,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials
   provided with the distribution.
3. Redistributions may not be sold, nor may they be used in a
   commercial product or activity without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

As the original files state, the Japanese text is the formal licence and
the English translation is for reference only.

## 2. geo3d reference model (Alex Moncks)

`core/geo3d.c` and `core/geo3d.h` port the Python reference model
(`geo3d/sim/gen_scenes.py`, `gen_vectors.py`) and follow `geo3d_engine.v`
of <https://github.com/alexmoncks/V9968_Cartridge>, branch `geo3d-v2.0`.

```
MIT License

Copyright (c) 2026 Alex Moncks

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 3. Raspberry Pi pico-examples / pico-sdk

`firmware/src/dvi_out.c` is based on `hstx/dvi_out_hstx_encoder` of
<https://github.com/raspberrypi/pico-examples>; `firmware/pico_sdk_import.cmake`
comes from the pico-sdk (with its notice).

```
Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
   disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following
   disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.```

## Behaviour references (no code copied)

- openMSX `src/video/v9990/` (GPL-2.0): V9990 behaviour where the Yamaha
  manual is silent (P1/P2 command coordinates, bitmap cursors, ...).
- Yamaha V9990 E-VDP-III Application Manual.
