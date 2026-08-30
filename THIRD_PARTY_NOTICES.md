# Third-party notices

## Xaymar/vst2sdk

EchoNull fetches revision `339d4f31590bf77c0d0d248e09a380ac6285e069`
of [Xaymar/vst2sdk](https://github.com/Xaymar/vst2sdk) at configure time.
It is a clean-room interoperability header distributed under the BSD 3-Clause
License.

Copyright 2020 Michael Fabian 'Xaymar' Dirks <info@xaymar.com>

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## NVIDIA Audio Effects SDK

NVIDIA headers, binaries, dependencies, and model packages are not committed to
this source repository. A self-contained release binary embeds the redistributable
runtime, selected model packages, and copies of the applicable NVIDIA agreements
from the builder's local SDK. EchoNull extracts that payload into a versioned
runtime cache when the plug-in starts. All NVIDIA materials remain subject to
the agreements embedded in the release DLL and extracted beside the runtime.
