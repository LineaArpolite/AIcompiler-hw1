; RUN: llc -march=cpu0 -mcpu=cpu032II -O2 < %s | FileCheck %s --check-prefix=ON
; RUN: llc -march=cpu0 -mcpu=cpu032II -O2 -enable-cpu0-mul-strength-reduce=false < %s | FileCheck %s --check-prefix=OFF

target triple = "cpu0"

define i32 @mul3(i32 %x) {
entry:
  %mul = mul nsw i32 %x, 3
  ret i32 %mul
}

; ON-LABEL: mul3:
; ON:       shl
; ON:       addu

; OFF-LABEL: mul3:
; OFF:       mul
