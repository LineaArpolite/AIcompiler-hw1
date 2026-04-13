; RUN: llc -march=cpu0 -mcpu=cpu032II -O2 < %s | FileCheck %s

target triple = "cpu0"

declare i32 @llvm.smax.i32(i32, i32)
declare i32 @llvm.ctpop.i32(i32)

define i32 @relu(i32 %x) {
entry:
  %m = call i32 @llvm.smax.i32(i32 %x, i32 0)
  ret i32 %m
}

define i32 @fma_i32(i32 %a, i32 %b, i32 %c) {
entry:
  %mul = mul nsw i32 %a, %b
  %add = add nsw i32 %mul, %c
  ret i32 %add
}

define i32 @popcount_i32(i32 %x) {
entry:
  %p = call i32 @llvm.ctpop.i32(i32 %x)
  ret i32 %p
}

define i32 @div10(i32 %x) {
entry:
  %q = sdiv i32 %x, 10
  ret i32 %q
}

; CHECK-LABEL: relu:
; CHECK:       max

; CHECK-LABEL: fma_i32:
; CHECK:       imac

; CHECK-LABEL: popcount_i32:
; CHECK:       popc

; CHECK-LABEL: div10:
; CHECK:       mult
; CHECK:       mfhi
; CHECK:       sra
