; Copyright (C) Codeplay Software Limited
;
; Licensed under the Apache License, Version 2.0 (the "License") with LLVM
; Exceptions; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;     https://github.com/codeplaysoftware/oneapi-construction-kit/blob/main/LICENSE.txt
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
; WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
; License for the specific language governing permissions and limitations
; under the License.
;
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; RUN: veczc -k extract_runtime_index -vecz-simd-width=4 -vecz-passes=packetizer -vecz-choices=TargetIndependentPacketization -S < %s | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "spir64-unknown-unknown"

declare i64 @__mux_get_global_id(i32) #1

; Function Attrs: nounwind
define spir_kernel void @extract_runtime_index(<4 x float> addrspace(1)* %in, i32 addrspace(1)* %x, float addrspace(1)* %out) #0 {
entry:
  %call = call i64 @__mux_get_global_id(i32 0) #2
  %arrayidx = getelementptr inbounds <4 x float>, <4 x float> addrspace(1)* %in, i64 %call
  %arrayidx2 = getelementptr inbounds i32, i32 addrspace(1)* %x, i64 %call
  %0 = load <4 x float>, <4 x float> addrspace(1)* %arrayidx, align 4
  %1 = load i32, i32 addrspace(1)* %arrayidx2, align 4
  %vecext = extractelement <4 x float> %0, i32 %1
  %arrayidx1 = getelementptr inbounds float, float addrspace(1)* %out, i64 %call
  store float %vecext, float addrspace(1)* %arrayidx1, align 4
  ret void
}

; CHECK: define spir_kernel void @__vecz_v4_extract_runtime_index
; CHECK: %[[SRC:.+]] = load <16 x float>, ptr addrspace(1) %
; CHECK: %[[IDX:.+]] = load <4 x i32>, ptr addrspace(1) %

; No splitting of the widened source vector
; CHECK-NOT: shufflevector

; Offset the indices
; CHECK: %[[ADD:.+]] = add <4 x i32> %[[IDX]], <i32 0, i32 4, i32 8, i32 12>

; Extract directly from the widened source with vectorized indices and insert directly into result
; CHECK: %[[IND0:.+]] = extractelement <4 x i32> %[[ADD]], i32 0
; CHECK: %[[EXT0:.+]] = extractelement <16 x float> %[[SRC]], i32 %[[IND0]]
; CHECK: %[[INS0:.+]] = insertelement <4 x float> undef, float %[[EXT0]], i32 0
; CHECK: %[[IND1:.+]] = extractelement <4 x i32> %[[ADD]], i32 1
; CHECK: %[[EXT1:.+]] = extractelement <16 x float> %[[SRC]], i32 %[[IND1]]
; CHECK: %[[INS1:.+]] = insertelement <4 x float> %[[INS0]], float %[[EXT1]], i32 1
; CHECK: %[[IND2:.+]] = extractelement <4 x i32> %[[ADD]], i32 2
; CHECK: %[[EXT2:.+]] = extractelement <16 x float> %[[SRC]], i32 %[[IND2]]
; CHECK: %[[INS2:.+]] = insertelement <4 x float> %[[INS1]], float %[[EXT2]], i32 2
; CHECK: %[[IND3:.+]] = extractelement <4 x i32> %[[ADD]], i32 3
; CHECK: %[[EXT3:.+]] = extractelement <16 x float> %[[SRC]], i32 %[[IND3]]
; CHECK: %[[INS3:.+]] = insertelement <4 x float> %[[INS2]], float %[[EXT3]], i32 3
; CHECK: store <4 x float> %[[INS3]], ptr addrspace(1) %{{.+}}
; CHECK: ret void
