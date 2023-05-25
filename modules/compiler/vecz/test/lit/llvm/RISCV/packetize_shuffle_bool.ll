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

; REQUIRES: llvm-13+
; RUN: veczc -vecz-target-triple="riscv64-unknown-unknown" -vecz-scalable -vecz-simd-width=4 -vecz-passes=packetizer -S < %s | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "spir64-unknown-unknown"

define spir_kernel void @f(<4 x i32> addrspace(1)* %in, <4 x i32> addrspace(1)* %out) {
entry:
  %gid = call spir_func i64 @_Z13get_global_idj(i32 0)
  %in.ptr = getelementptr inbounds <4 x i32>, <4 x i32> addrspace(1)* %in, i64 %gid
  %in.data = load <4 x i32>, <4 x i32> addrspace(1)* %in.ptr
  %in.bool = icmp ne <4 x i32> %in.data, zeroinitializer
  %out.data = shufflevector <4 x i1> %in.bool, <4 x i1> undef, <4 x i32> <i32 3, i32 2, i32 1, i32 0>
  %out.sext = sext <4 x i1> %out.data to <4 x i32>
  %out.ptr = getelementptr inbounds <4 x i32>, <4 x i32> addrspace(1)* %out, i64 %gid
  store <4 x i32> %out.sext, <4 x i32> addrspace(1)* %out.ptr, align 32
  ret void
}

declare spir_func i64 @_Z13get_global_idj(i32) #1

; It checks that a single-operand shuffle that doesn't change the length is packetized to a gather intrinsic,
; and that it works with a vector of i1 type by temporarily extending to i8.
; CHECK: define spir_kernel void @__vecz_nxv4_f({{.*}}) {{.*}} {
; CHECK: entry:
; CHECK:  %[[DATA:.+]] = load <vscale x 16 x i32>, {{(<vscale x 16 x i32> addrspace\(1\)\*)|(ptr addrspace\(1\))}} %{{.*}}
; CHECK:  %[[DATA_i1:.+]] = icmp ne <vscale x 16 x i32> %[[DATA]], zeroinitializer
; CHECK:  %[[DATA_i8:.+]] = zext <vscale x 16 x i1> %[[DATA_i1]] to <vscale x 16 x i8>
; CHECK:  %[[GATHER:.+]] = call <vscale x 16 x i8> @llvm.riscv.vrgatherei16.vv.nxv16i8.i64(<vscale x 16 x i8> undef, <vscale x 16 x i8> %[[DATA_i8]], <vscale x 16 x i16> %{{.+}}, i64 %{{.+}})
; CHECK:  %[[GATHER_i1:.+]] = trunc <vscale x 16 x i8> %[[GATHER]] to <vscale x 16 x i1>
; CHECK:  %[[RESULT:.+]] = sext <vscale x 16 x i1> %[[GATHER_i1]] to <vscale x 16 x i32>
; CHECK:  store <vscale x 16 x i32> %[[RESULT]]
; CHECK:  ret void
; CHECK: }
