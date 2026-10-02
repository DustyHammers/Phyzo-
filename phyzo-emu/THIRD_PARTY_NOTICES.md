# Third-party notices

## Musashi

Musashi is used as the 68000-family execution core. It is an external build
dependency (cloned into `work/deps/Musashi`); its source is not copied into this
repository. The build generates its opcode tables and applies the same
opcode-table bounds patch as the EPS-16+ project, in the build directory only.

Project: https://github.com/kstenerud/Musashi
Copyright 1998-2002 Karl Stenerud

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## MAME (reference only)

The MC68340 timer, serial and DMA models were written for this project. MAME's
`m68340` devices (BSD-3-Clause; David Haywood, Joakim Larsson Edstrom and
contributors) were consulted as a behavioural reference for register bit
meanings. No MAME source is included.

## Manufacturer material

No operating-system image, firmware, wave data or other manufacturer material is
included. The OS image is supplied by the user at run time.


## MAME es5506 device (behavioural reference)

`src/voice_core.cpp` is written for this project. Its interpolation, 4-pole filter equations, log volume law,
loop/IRQ handling and the ES5506 register semantics follow the behaviour of MAME's `es5506` device by
Aaron Giles; no MAME source file is included. MAME's es5506 device is licensed under BSD-3-Clause:

Copyright (c) Aaron Giles. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that
the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
   following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
   following disclaimer in the documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or
   promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


## ESP2 effects processor (behavioural reference)

`src/esp2_core.cpp` and `src/esp2_core.h` are written for this project. The instruction set, MAC/ALU/condition-code
behaviour, AGEN addressing, special-purpose register map, host interface and object format follow these public
documents; no code from them is included:

- J. Dattorro et al., "ESP2 Part I: Instruction and Hardware Specification" (public copy:
  https://ccrma.stanford.edu/~dattorro/ESP2.pdf).
- "ESP2 Object Format Specification" (public copy: https://convexoptimization.com/TOOLS/ESP2APPX.pdf).
- US Patent 5,517,436 (condition-mask table, AGEN equations, pointer latencies).

`tools/esp2obj_parser.py` (repository root) parses objects in the format described by the Object Format
Specification; it contains no program data.

## Planned dependencies (entries to complete when each is added)

- **JUCE 8** (plugin wrapper, Phase 3). Open-source licence: AGPLv3; a commercial licence is also offered. Under the
  AGPLv3, distributing the plugin requires its source to be released under AGPL-compatible terms. Add the licence
  text and version here when JUCE is added.
- **RmlUi** (panel skin system, Phase 4). MIT licence. Add the copyright notice and licence text here when RmlUi is
  added.
