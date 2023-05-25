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

; RUN: veczc -k partial_linearization4 -vecz-passes=cfg-convert,cleanup-divergence -vecz-choices=LinearizeBOSCC -S < %s | FileCheck %s

; The CFG of the following kernel is:
;
;     a
;     |
;     b <-.
;    / \  |
;   e   c |
;   |  / \|
;   | f   d
;   |/
;   g
;
; * where node b is a uniform branch, and node c is a varying branch.
; * where nodes f, d and g are divergent.
;
; With BOSCC, it will be transformed as follows:
;
;     a
;     |
;     b <-.     b' <--.
;    / \  |    / \    |
;   e   c_|_  e'  c'  |
;   |  / \| \_|__ |   |
;   | f   d   |  `d' -'
;   |/         \ /
;   g           f'
;   |           |
;   `---> & <-- g'
;
; where '&' represents merge blocks of BOSCC regions.
;
; __kernel void partial_linearization4(__global int *out, int n) {
;   int id = get_global_id(0);
;
;   int x = id / n;
;   int y = id % n;
;   int i = 0;
;   for (;;) {
;     if (n > 20) goto e;
;     if (x + y > n) goto f;
;     y++;
;     x++;
;     i++;
;   }
;
; goto g;
;
; e:
;   i *= 2 + n;
;   goto g;
;
; f:
;   i /= i + n;
;
; g:
;   out[id] = x + y + i;
; }

; ModuleID = 'Unknown buffer'
source_filename = "Unknown buffer"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "spir64-unknown-unknown"

; Function Attrs: nounwind
define spir_kernel void @partial_linearization4(i32 addrspace(1)* %out, i32 %n) #0 {
entry:
  %call = call spir_func i64 @_Z13get_global_idj(i32 0) #2
  %conv = trunc i64 %call to i32
  %0 = icmp eq i32 %conv, -2147483648
  %1 = icmp eq i32 %n, -1
  %2 = and i1 %1, %0
  %3 = icmp eq i32 %n, 0
  %4 = or i1 %3, %2
  %5 = select i1 %4, i32 1, i32 %n
  %div = sdiv i32 %conv, %5
  %6 = icmp eq i32 %conv, -2147483648
  %7 = icmp eq i32 %n, -1
  %8 = and i1 %7, %6
  %9 = icmp eq i32 %n, 0
  %10 = or i1 %9, %8
  %11 = select i1 %10, i32 1, i32 %n
  %rem = srem i32 %conv, %11
  br label %for.cond

for.cond:                                         ; preds = %if.end5, %entry
  %x.0 = phi i32 [ %div, %entry ], [ %inc6, %if.end5 ]
  %y.0 = phi i32 [ %rem, %entry ], [ %inc, %if.end5 ]
  %storemerge = phi i32 [ 0, %entry ], [ %inc7, %if.end5 ]
  %cmp = icmp sgt i32 %n, 20
  br i1 %cmp, label %e, label %if.end

if.end:                                           ; preds = %for.cond
  %add = add nsw i32 %y.0, %x.0
  %cmp2 = icmp sgt i32 %add, %n
  br i1 %cmp2, label %f, label %if.end5

if.end5:                                          ; preds = %if.end
  %inc = add nsw i32 %y.0, 1
  %inc6 = add nsw i32 %x.0, 1
  %inc7 = add nsw i32 %storemerge, 1
  br label %for.cond

e:                                                ; preds = %for.cond
  %add8 = add nsw i32 %n, 2
  %mul = mul nsw i32 %storemerge, %add8
  br label %g

f:                                                ; preds = %if.end
  %add9 = add nsw i32 %storemerge, %n
  %12 = icmp eq i32 %add9, 0
  %13 = select i1 %12, i32 1, i32 %add9
  %div10 = sdiv i32 %storemerge, %13
  br label %g

g:                                                ; preds = %f, %e
  %storemerge1 = phi i32 [ %div10, %f ], [ %mul, %e ]
  %add11 = add i32 %y.0, %x.0
  %add12 = add i32 %add11, %storemerge1
  %idxprom = sext i32 %conv to i64
  %arrayidx = getelementptr inbounds i32, i32 addrspace(1)* %out, i64 %idxprom
  store i32 %add12, i32 addrspace(1)* %arrayidx, align 4
  ret void
}

; Function Attrs: nounwind readonly
declare spir_func i64 @_Z13get_global_idj(i32) #1

attributes #0 = { nounwind "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="0" "stackrealign" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readonly "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="0" "stackrealign" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nobuiltin nounwind readonly }

!llvm.module.flags = !{!0}
!opencl.ocl.version = !{!1}
!opencl.spir.version = !{!1}
!opencl.kernels = !{!2}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 1, i32 2}
!2 = !{void (i32 addrspace(1)*, i32)* @partial_linearization4, !3, !4, !5, !6, !7, !8}
!3 = !{!"kernel_arg_addr_space", i32 1, i32 0}
!4 = !{!"kernel_arg_access_qual", !"none", !"none"}
!5 = !{!"kernel_arg_type", !"int*", !"int"}
!6 = !{!"kernel_arg_base_type", !"int*", !"int"}
!7 = !{!"kernel_arg_type_qual", !"", !""}
!8 = !{!"kernel_arg_name", !"out", !"n"}

; CHECK: spir_kernel void @__vecz_v4_partial_linearization4
; CHECK: br i1 true, label %[[FORCONDUNIFORM:.+]], label %[[FORCOND:.+]]

; CHECK: [[FORCOND]]:
; CHECK: %[[CMP:.+]] = icmp
; CHECK: br i1 %[[CMP]], label %[[E:.+]], label %[[IFEND:.+]]

; CHECK: [[IFEND]]:
; CHECK: br label %[[IFEND5:.+]]

; CHECK: [[FORCONDUNIFORM]]:
; CHECK: %[[CMPUNIFORM:.+]] = icmp
; CHECK: br i1 %[[CMPUNIFORM]], label %[[EUNIFORM:.+]], label %[[IFENDUNIFORM:.+]]

; CHECK: [[IFENDUNIFORM]]:
; CHECK: br i1 %{{.+}}, label %[[FUNIFORM:.+]], label %[[IFENDUNIFORMBOSCCINDIR:.+]]

; CHECK: [[IFEND5UNIFORM:.+]]:
; CHECK: br label %[[FORCONDUNIFORM]]

; CHECK: [[FUNIFORM]]:
; CHECK: br label %[[GUNIFORM:.+]]

; CHECK: [[IFENDUNIFORMBOSCCINDIR]]:
; CHECK: br i1 %{{.+}}, label %[[IFEND5UNIFORM:.+]], label %[[IFENDUNIFORMBOSCCSTORE:.+]]

; CHECK: [[IFENDUNIFORMBOSCCSTORE]]:
; CHECK: br label %[[IFEND5]]

; CHECK: [[EUNIFORM]]:
; CHECK: br label %[[G:.+]]

; CHECK: [[IFEND5]]:
; CHECK: br i1 %{{.+}}, label %[[FORCOND]], label %[[FORCONDPUREEXIT:.+]]

; CHECK: [[FORCONDPUREEXIT]]:
; CHECK: br label %[[F:.+]]

; CHECK: [[E]]:
; CHECK: br label %[[FORCONDPUREEXIT]]

; CHECK: [[EELSE:.+]]:
; CHECK: br label %[[G]]

; CHECK: [[ESPLIT:.+]]:
; CHECK: br label %[[G]]

; CHECK: [[F]]:
; CHECK: br label %[[FELSE:.+]]

; CHECK: [[FELSE]]:
; CHECK: br i1 %{{.+}}, label %[[EELSE]], label %[[ESPLIT]]

; CHECK: [[G]]:
; CHECK: ret void
