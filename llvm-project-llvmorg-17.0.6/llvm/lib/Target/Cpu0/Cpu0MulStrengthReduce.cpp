//===-- Cpu0MulStrengthReduce.cpp - Reduce mul by small constant ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Convert high-latency integer MUL-by-constant patterns into SHL/ADDu/SUBu
// sequences when profitable.
//
//===----------------------------------------------------------------------===//

#include "Cpu0.h"

#include "Cpu0InstrInfo.h"
#include "Cpu0TargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "cpu0-mul-strength-reduce"

STATISTIC(NumMulStrengthReduced,
          "Number of Cpu0 MUL instructions strength-reduced");

static cl::opt<bool> EnableCpu0MulStrengthReduce(
    "enable-cpu0-mul-strength-reduce", cl::init(true), cl::Hidden,
    cl::desc("Enable Cpu0 machine-level multiply strength reduction."));

namespace {

class Cpu0MulStrengthReduce : public MachineFunctionPass {
public:
  static char ID;

  explicit Cpu0MulStrengthReduce(Cpu0TargetMachine &TM)
      : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "Cpu0 Multiply Strength Reduction";
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  bool reduceMulInstr(MachineInstr &MulMI, MachineRegisterInfo &MRI,
                      const Cpu0InstrInfo &TII) const;

  bool extractConstant(Register Reg, MachineRegisterInfo &MRI, int64_t &Value,
                       MachineInstr *&DefMI) const;
  bool extractConstantFromPrevDef(MachineInstr &MulMI, Register Reg,
                                  int64_t &Value, MachineInstr *&DefMI) const;

  static bool decomposeAbsConst(unsigned AbsC, unsigned &ShiftAmt,
                                bool &PlusOne, bool &MinusOne);
};

} // end anonymous namespace

char Cpu0MulStrengthReduce::ID = 0;

bool Cpu0MulStrengthReduce::extractConstant(Register Reg,
                                            MachineRegisterInfo &MRI,
                                            int64_t &Value,
                                            MachineInstr *&DefMI) const {
  if (!Reg.isVirtual())
    return false;

  DefMI = MRI.getVRegDef(Reg);
  if (!DefMI)
    return false;

  unsigned Opc = DefMI->getOpcode();
  if (Opc != Cpu0::ADDiu && Opc != Cpu0::ORi)
    return false;

  if (!DefMI->getOperand(1).isReg() || DefMI->getOperand(1).getReg() != Cpu0::ZERO)
    return false;

  if (!DefMI->getOperand(2).isImm())
    return false;

  Value = DefMI->getOperand(2).getImm();
  return true;
}

bool Cpu0MulStrengthReduce::extractConstantFromPrevDef(
    MachineInstr &MulMI, Register Reg, int64_t &Value, MachineInstr *&DefMI) const {
  if (!Reg.isPhysical())
    return false;

  MachineInstr *Prev = MulMI.getPrevNode();
  while (Prev && Prev->isDebugInstr())
    Prev = Prev->getPrevNode();
  if (!Prev)
    return false;

  unsigned Opc = Prev->getOpcode();
  if (Opc != Cpu0::ADDiu && Opc != Cpu0::ORi)
    return false;
  if (!Prev->getOperand(0).isReg() || Prev->getOperand(0).getReg() != Reg)
    return false;
  if (!Prev->getOperand(1).isReg() || Prev->getOperand(1).getReg() != Cpu0::ZERO)
    return false;
  if (!Prev->getOperand(2).isImm())
    return false;

  Value = Prev->getOperand(2).getImm();
  DefMI = Prev;
  return true;
}

bool Cpu0MulStrengthReduce::decomposeAbsConst(unsigned AbsC, unsigned &ShiftAmt,
                                              bool &PlusOne,
                                              bool &MinusOne) {
  ShiftAmt = 0;
  PlusOne = false;
  MinusOne = false;

  if (AbsC == 0 || AbsC == 1)
    return true;

  if (isPowerOf2_64(AbsC)) {
    ShiftAmt = Log2_64(AbsC);
    return true;
  }

  for (unsigned K = 1; K < 31; ++K) {
    unsigned Pow2 = 1u << K;
    if (AbsC == Pow2 + 1) {
      ShiftAmt = K;
      PlusOne = true;
      return true;
    }
    if (AbsC == Pow2 - 1) {
      ShiftAmt = K;
      MinusOne = true;
      return true;
    }
  }

  return false;
}

bool Cpu0MulStrengthReduce::reduceMulInstr(MachineInstr &MulMI,
                                           MachineRegisterInfo &MRI,
                                           const Cpu0InstrInfo &TII) const {
  if (MulMI.getOpcode() != Cpu0::MUL)
    return false;

  if (!MulMI.getOperand(0).isReg() || !MulMI.getOperand(1).isReg() ||
      !MulMI.getOperand(2).isReg())
    return false;

  Register Dst = MulMI.getOperand(0).getReg();
  Register Src0 = MulMI.getOperand(1).getReg();
  Register Src1 = MulMI.getOperand(2).getReg();

  int64_t C0 = 0;
  int64_t C1 = 0;
  MachineInstr *Def0 = nullptr;
  MachineInstr *Def1 = nullptr;

  bool IsConst0 = extractConstant(Src0, MRI, C0, Def0);
  bool IsConst1 = extractConstant(Src1, MRI, C1, Def1);
  if (!IsConst0)
    IsConst0 = extractConstantFromPrevDef(MulMI, Src0, C0, Def0);
  if (!IsConst1)
    IsConst1 = extractConstantFromPrevDef(MulMI, Src1, C1, Def1);

  if (IsConst0 == IsConst1)
    return false;

  Register VarReg = IsConst0 ? Src1 : Src0;
  Register ConstReg = IsConst0 ? Src0 : Src1;
  int64_t C = IsConst0 ? C0 : C1;
  MachineInstr *ConstDefMI = IsConst0 ? Def0 : Def1;
  bool ConstDefDirectlyFeedsMul = false;
  if (ConstDefMI && ConstReg.isPhysical()) {
    MachineInstr *Next = ConstDefMI->getNextNode();
    while (Next && Next->isDebugInstr())
      Next = Next->getNextNode();
    ConstDefDirectlyFeedsMul = (Next == &MulMI);
  }

  if (!isInt<32>(C))
    return false;

  int64_t Abs64 = C < 0 ? -C : C;
  if (Abs64 > 0x7fffffff)
    return false;

  unsigned AbsC = static_cast<unsigned>(Abs64);
  unsigned ShiftAmt = 0;
  bool PlusOne = false;
  bool MinusOne = false;
  if (!decomposeAbsConst(AbsC, ShiftAmt, PlusOne, MinusOne))
    return false;

  MachineBasicBlock &MBB = *MulMI.getParent();
  MachineBasicBlock::iterator InsertPt = MulMI;
  DebugLoc DL = MulMI.getDebugLoc();

  Register ResultReg;

  auto *RC = &Cpu0::CPURegsRegClass;
  auto CreateTmp = [&]() { return MRI.createVirtualRegister(RC); };

  if (AbsC == 0) {
    BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::ADDiu), Dst)
        .addReg(Cpu0::ZERO)
        .addImm(0);
    ResultReg = Dst;
  } else if (AbsC == 1) {
    ResultReg = VarReg;
  } else {
    Register ShiftReg = (Dst != VarReg) ? Dst : CreateTmp();
    BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::SHL), ShiftReg)
        .addReg(VarReg)
        .addImm(ShiftAmt);

    if (PlusOne) {
      BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::ADDu), ShiftReg)
          .addReg(ShiftReg)
          .addReg(VarReg);
    } else if (MinusOne) {
      BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::SUBu), ShiftReg)
          .addReg(ShiftReg)
          .addReg(VarReg);
    }

    ResultReg = ShiftReg;
  }

  if (C < 0) {
    BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::SUBu), Dst)
        .addReg(Cpu0::ZERO)
        .addReg(ResultReg);
  } else if (ResultReg != Dst) {
    BuildMI(MBB, InsertPt, DL, TII.get(Cpu0::ADDu), Dst)
        .addReg(ResultReg)
        .addReg(Cpu0::ZERO);
  }

  MulMI.eraseFromParent();

  if (ConstDefMI && ConstDefMI->getParent()) {
    if (ConstReg.isVirtual() && MRI.use_nodbg_empty(ConstReg))
      ConstDefMI->eraseFromParent();
    else if (ConstReg.isPhysical() && ConstDefDirectlyFeedsMul)
      ConstDefMI->eraseFromParent();
  }

  ++NumMulStrengthReduced;
  return true;
}

bool Cpu0MulStrengthReduce::runOnMachineFunction(MachineFunction &MF) {
  if (!EnableCpu0MulStrengthReduce)
    return false;

  const auto &TII = *static_cast<const Cpu0InstrInfo *>(MF.getSubtarget().getInstrInfo());
  MachineRegisterInfo &MRI = MF.getRegInfo();

  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    for (MachineBasicBlock::iterator I = MBB.begin(); I != MBB.end();) {
      MachineInstr &MI = *I++;
      Changed |= reduceMulInstr(MI, MRI, TII);
    }
  }

  return Changed;
}

FunctionPass *llvm::createCpu0MulStrengthReducePass(Cpu0TargetMachine &TM) {
  return new Cpu0MulStrengthReduce(TM);
}
