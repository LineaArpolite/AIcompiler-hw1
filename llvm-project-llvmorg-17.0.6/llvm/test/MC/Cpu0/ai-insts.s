# RUN: llvm-mc -triple=cpu0 -show-encoding %s | FileCheck %s --check-prefix=ENC
# RUN: llvm-mc -triple=cpu0 -filetype=obj %s -o - | llvm-objdump -d --triple=cpu0 - | FileCheck %s --check-prefix=DIS

imac $2, $3, $4, $5
max $2, $3, $4
min $6, $7, $8
popc $9, $10

# ENC: imac $2, $3, $4, $5                  # encoding: [0x00,0x45,0x23,0x53]
# ENC: max  $2, $3, $4                      # encoding: [0x00,0x40,0x23,0x54]
# ENC: min  $t9, $7, $8                     # encoding: [0x00,0x80,0x67,0x55]
# ENC: popc $9, $10                         # encoding: [0x00,0x00,0x9a,0x56]

# DIS: 00 45 23 53   imac $2, $3, $4, $5
# DIS: 00 40 23 54   max  $2, $3, $4
# DIS: 00 80 67 55   min  $t9, $7, $8
# DIS: 00 00 9a 56   popc $9, $10
