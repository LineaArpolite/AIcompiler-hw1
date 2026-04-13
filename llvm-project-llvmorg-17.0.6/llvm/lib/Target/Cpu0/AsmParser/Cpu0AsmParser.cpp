//===-- Cpu0AsmParser.cpp - Parse Cpu0 assembly to MCInst instructions ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Cpu0.h"
#include "Cpu0RegisterInfo.h"
#include "MCTargetDesc/Cpu0MCTargetDesc.h"
#include "TargetInfo/Cpu0TargetInfo.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCParser/MCAsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCParsedAsmOperand.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "cpu0-asm-parser"

namespace {

class Cpu0Operand;

class Cpu0AsmParser : public MCTargetAsmParser {
  MCAsmParser &Parser;

#define GET_ASSEMBLER_HEADER
#include "Cpu0GenAsmMatcher.inc"

  bool MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                               OperandVector &Operands, MCStreamer &Out,
                               uint64_t &ErrorInfo,
                               bool MatchingInlineAsm) override;

  bool parseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                     SMLoc &EndLoc) override;

  OperandMatchResultTy tryParseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                                        SMLoc &EndLoc) override;

  bool ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                        SMLoc NameLoc, OperandVector &Operands) override;

  bool parseOperand(OperandVector &Operands, StringRef Mnemonic);
  OperandMatchResultTy parseRegisterOperand(OperandVector &Operands);
  OperandMatchResultTy parseImmediateOrMemOperand(OperandVector &Operands);

  int matchRegisterName(StringRef Name) const;
  int matchRegisterByNumber(unsigned RegNum) const;

  bool parseRegisterWithDollar(MCRegister &RegNo, SMLoc &StartLoc,
                               SMLoc &EndLoc);

public:
  Cpu0AsmParser(const MCSubtargetInfo &STI, MCAsmParser &Parser,
                const MCInstrInfo &MII, const MCTargetOptions &Options)
      : MCTargetAsmParser(Options, STI, MII), Parser(Parser) {
    setAvailableFeatures(ComputeAvailableFeatures(STI.getFeatureBits()));
  }

  MCAsmParser &getParser() const { return Parser; }
  MCAsmLexer &getLexer() const { return Parser.getLexer(); }
};

class Cpu0Operand : public MCParsedAsmOperand {
public:
  enum KindTy { k_Token, k_Register, k_Immediate, k_Memory } Kind;

  struct TokenOp {
    const char *Data;
    unsigned Length;
  };

  struct RegOp {
    unsigned RegNum;
  };

  struct ImmOp {
    const MCExpr *Val;
  };

  struct MemOp {
    unsigned Base;
    const MCExpr *Off;
  };

  union {
    TokenOp Tok;
    RegOp Reg;
    ImmOp Imm;
    MemOp Mem;
  };

  SMLoc StartLoc;
  SMLoc EndLoc;

  Cpu0Operand(KindTy K) : Kind(K) {}

  bool isReg() const override { return Kind == k_Register; }
  bool isImm() const override { return Kind == k_Immediate; }
  bool isMem() const override { return Kind == k_Memory; }
  bool isToken() const override { return Kind == k_Token; }

  bool isSimm16() const {
    if (!isImm())
      return false;
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    return CE && isInt<16>(CE->getValue());
  }

  bool isUimm16() const {
    if (!isImm())
      return false;
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    return CE && isUInt<16>(CE->getValue());
  }

  bool isShamt() const {
    if (!isImm())
      return false;
    const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val);
    return CE && isUInt<5>(CE->getValue());
  }

  bool isBrtarget16() const { return isSimm16(); }

  bool isBrtarget24() const {
    if (!isImm())
      return false;
    if (const auto *CE = dyn_cast<MCConstantExpr>(Imm.Val))
      return isInt<24>(CE->getValue());
    return true;
  }

  bool isJmptarget() const { return isImm(); }
  bool isCalltarget() const { return isImm(); }
  bool isMem_ea() const { return isMem(); }

  StringRef getToken() const {
    assert(Kind == k_Token && "Invalid token access");
    return StringRef(Tok.Data, Tok.Length);
  }

  unsigned getReg() const override {
    assert(Kind == k_Register && "Invalid register access");
    return Reg.RegNum;
  }

  const MCExpr *getImm() const {
    assert(Kind == k_Immediate && "Invalid immediate access");
    return Imm.Val;
  }

  unsigned getMemBase() const {
    assert(Kind == k_Memory && "Invalid memory base access");
    return Mem.Base;
  }

  const MCExpr *getMemOff() const {
    assert(Kind == k_Memory && "Invalid memory offset access");
    return Mem.Off;
  }

  void addRegOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of register operands");
    Inst.addOperand(MCOperand::createReg(getReg()));
  }

  void addExpr(MCInst &Inst, const MCExpr *Expr) const {
    if (!Expr) {
      Inst.addOperand(MCOperand::createImm(0));
      return;
    }

    if (const auto *CE = dyn_cast<MCConstantExpr>(Expr)) {
      Inst.addOperand(MCOperand::createImm(CE->getValue()));
      return;
    }

    Inst.addOperand(MCOperand::createExpr(Expr));
  }

  void addImmOperands(MCInst &Inst, unsigned N) const {
    assert(N == 1 && "Invalid number of immediate operands");
    addExpr(Inst, getImm());
  }

  void addMemOperands(MCInst &Inst, unsigned N) const {
    assert(N == 2 && "Invalid number of memory operands");
    Inst.addOperand(MCOperand::createReg(getMemBase()));
    addExpr(Inst, getMemOff());
  }

  static std::unique_ptr<Cpu0Operand> createToken(StringRef Str, SMLoc Loc) {
    auto Op = std::make_unique<Cpu0Operand>(k_Token);
    Op->Tok.Data = Str.data();
    Op->Tok.Length = Str.size();
    Op->StartLoc = Loc;
    Op->EndLoc = Loc;
    return Op;
  }

  static std::unique_ptr<Cpu0Operand> createReg(unsigned RegNum, SMLoc Start,
                                                SMLoc End) {
    auto Op = std::make_unique<Cpu0Operand>(k_Register);
    Op->Reg.RegNum = RegNum;
    Op->StartLoc = Start;
    Op->EndLoc = End;
    return Op;
  }

  static std::unique_ptr<Cpu0Operand> createImm(const MCExpr *Val, SMLoc Start,
                                                SMLoc End) {
    auto Op = std::make_unique<Cpu0Operand>(k_Immediate);
    Op->Imm.Val = Val;
    Op->StartLoc = Start;
    Op->EndLoc = End;
    return Op;
  }

  static std::unique_ptr<Cpu0Operand> createMem(unsigned Base, const MCExpr *Off,
                                                SMLoc Start, SMLoc End) {
    auto Op = std::make_unique<Cpu0Operand>(k_Memory);
    Op->Mem.Base = Base;
    Op->Mem.Off = Off;
    Op->StartLoc = Start;
    Op->EndLoc = End;
    return Op;
  }

  SMLoc getStartLoc() const override { return StartLoc; }
  SMLoc getEndLoc() const override { return EndLoc; }

  void print(raw_ostream &OS) const override {
    switch (Kind) {
    case k_Token:
      OS << "Token(" << getToken() << ")";
      return;
    case k_Register:
      OS << "Reg(" << getReg() << ")";
      return;
    case k_Immediate:
      OS << "Imm(";
      getImm()->print(OS, nullptr);
      OS << ")";
      return;
    case k_Memory:
      OS << "Mem(base=" << getMemBase() << ", off=";
      getMemOff()->print(OS, nullptr);
      OS << ")";
      return;
    }
    llvm_unreachable("Unhandled Cpu0 operand kind");
  }
};

} // end anonymous namespace

#define GET_REGISTER_MATCHER
#define GET_MATCHER_IMPLEMENTATION
#include "Cpu0GenAsmMatcher.inc"

static SMLoc refineErrorLoc(SMLoc IDLoc, const OperandVector &Operands,
                            uint64_t ErrorInfo) {
  if (ErrorInfo == ~0ULL || ErrorInfo >= Operands.size())
    return IDLoc;

  SMLoc Loc = Operands[ErrorInfo]->getStartLoc();
  return Loc.isValid() ? Loc : IDLoc;
}

bool Cpu0AsmParser::MatchAndEmitInstruction(SMLoc IDLoc, unsigned &Opcode,
                                            OperandVector &Operands,
                                            MCStreamer &Out,
                                            uint64_t &ErrorInfo,
                                            bool MatchingInlineAsm) {
  MCInst Inst;
  auto MatchResult =
      MatchInstructionImpl(Operands, Inst, ErrorInfo, MatchingInlineAsm);

  switch (MatchResult) {
  case Match_Success:
    Inst.setLoc(IDLoc);
    Out.emitInstruction(Inst, getSTI());
    return false;
  case Match_MissingFeature:
    return Error(IDLoc, "instruction requires a disabled subtarget feature");
  case Match_MnemonicFail:
    return Error(IDLoc, "unrecognized instruction mnemonic");
  case Match_InvalidOperand:
    return Error(refineErrorLoc(IDLoc, Operands, ErrorInfo),
                 "invalid operand for instruction");
  default:
    return Error(refineErrorLoc(IDLoc, Operands, ErrorInfo),
                 "invalid operand for instruction");
  }
}

int Cpu0AsmParser::matchRegisterByNumber(unsigned RegNum) const {
  switch (RegNum) {
  case 0:
    return Cpu0::ZERO;
  case 1:
    return Cpu0::AT;
  case 2:
    return Cpu0::V0;
  case 3:
    return Cpu0::V1;
  case 4:
    return Cpu0::A0;
  case 5:
    return Cpu0::A1;
  case 6:
    return Cpu0::T9;
  case 7:
    return Cpu0::T0;
  case 8:
    return Cpu0::T1;
  case 9:
    return Cpu0::S0;
  case 10:
    return Cpu0::S1;
  case 11:
    return Cpu0::GP;
  case 12:
    return Cpu0::FP;
  case 13:
    return Cpu0::SP;
  case 14:
    return Cpu0::LR;
  case 15:
    return Cpu0::SW;
  default:
    return 0;
  }
}

int Cpu0AsmParser::matchRegisterName(StringRef Name) const {
  StringRef Lower = Name.lower();

  int Numbered = StringSwitch<int>(Lower)
                     .Case("0", Cpu0::ZERO)
                     .Case("1", Cpu0::AT)
                     .Case("2", Cpu0::V0)
                     .Case("3", Cpu0::V1)
                     .Case("4", Cpu0::A0)
                     .Case("5", Cpu0::A1)
                     .Case("6", Cpu0::T9)
                     .Case("7", Cpu0::T0)
                     .Case("8", Cpu0::T1)
                     .Case("9", Cpu0::S0)
                     .Case("10", Cpu0::S1)
                     .Case("11", Cpu0::GP)
                     .Case("12", Cpu0::FP)
                     .Case("13", Cpu0::SP)
                     .Case("14", Cpu0::LR)
                     .Case("15", Cpu0::SW)
                     .Default(0);
  if (Numbered)
    return Numbered;

  return StringSwitch<int>(Lower)
      .Case("zero", Cpu0::ZERO)
      .Case("at", Cpu0::AT)
      .Case("v0", Cpu0::V0)
      .Case("v1", Cpu0::V1)
      .Case("a0", Cpu0::A0)
      .Case("a1", Cpu0::A1)
      .Case("t9", Cpu0::T9)
      .Case("t0", Cpu0::T0)
      .Case("t1", Cpu0::T1)
      .Case("s0", Cpu0::S0)
      .Case("s1", Cpu0::S1)
      .Case("gp", Cpu0::GP)
      .Case("fp", Cpu0::FP)
      .Case("sp", Cpu0::SP)
      .Case("lr", Cpu0::LR)
      .Case("sw", Cpu0::SW)
      .Case("pc", Cpu0::PC)
      .Case("epc", Cpu0::EPC)
      .Case("hi", Cpu0::HI)
      .Case("lo", Cpu0::LO)
      .Default(0);
}

bool Cpu0AsmParser::parseRegisterWithDollar(MCRegister &RegNo, SMLoc &StartLoc,
                                            SMLoc &EndLoc) {
  if (getLexer().getKind() != AsmToken::Dollar)
    return true;

  StartLoc = getLexer().getLoc();
  Parser.Lex(); // Eat '$'.

  int Reg = 0;
  if (getLexer().getKind() == AsmToken::Identifier) {
    Reg = matchRegisterName(getLexer().getTok().getIdentifier());
  } else if (getLexer().getKind() == AsmToken::Integer) {
    Reg = matchRegisterByNumber(static_cast<unsigned>(getLexer().getTok().getIntVal()));
  } else {
    return true;
  }

  if (!Reg)
    return true;

  EndLoc = getLexer().getLoc();
  Parser.Lex(); // Eat register token.
  RegNo = Reg;
  return false;
}

bool Cpu0AsmParser::parseRegister(MCRegister &RegNo, SMLoc &StartLoc,
                                  SMLoc &EndLoc) {
  return parseRegisterWithDollar(RegNo, StartLoc, EndLoc);
}

OperandMatchResultTy Cpu0AsmParser::tryParseRegister(MCRegister &RegNo,
                                                     SMLoc &StartLoc,
                                                     SMLoc &EndLoc) {
  if (getLexer().getKind() != AsmToken::Dollar)
    return MatchOperand_NoMatch;

  if (parseRegisterWithDollar(RegNo, StartLoc, EndLoc))
    return MatchOperand_ParseFail;

  return MatchOperand_Success;
}

OperandMatchResultTy
Cpu0AsmParser::parseRegisterOperand(OperandVector &Operands) {
  MCRegister Reg;
  SMLoc StartLoc, EndLoc;
  OperandMatchResultTy Match = tryParseRegister(Reg, StartLoc, EndLoc);
  if (Match != MatchOperand_Success)
    return Match;

  Operands.push_back(Cpu0Operand::createReg(Reg, StartLoc, EndLoc));
  return MatchOperand_Success;
}

OperandMatchResultTy
Cpu0AsmParser::parseImmediateOrMemOperand(OperandVector &Operands) {
  SMLoc StartLoc = getLexer().getLoc();

  const MCExpr *Expr = nullptr;
  if (getLexer().getKind() == AsmToken::LParen) {
    // ( $reg )
    Parser.Lex(); // Eat '('.

    MCRegister BaseReg;
    SMLoc BaseStart, BaseEnd;
    if (parseRegisterWithDollar(BaseReg, BaseStart, BaseEnd))
      return MatchOperand_ParseFail;

    if (getLexer().getKind() != AsmToken::RParen)
      return MatchOperand_ParseFail;

    SMLoc EndLoc = getLexer().getLoc();
    Parser.Lex(); // Eat ')'.

    Expr = MCConstantExpr::create(0, getContext());
    Operands.push_back(Cpu0Operand::createMem(BaseReg, Expr, StartLoc, EndLoc));
    return MatchOperand_Success;
  }

  if (getParser().parseExpression(Expr))
    return MatchOperand_ParseFail;

  if (getLexer().getKind() == AsmToken::LParen) {
    // imm($reg)
    Parser.Lex(); // Eat '('.

    MCRegister BaseReg;
    SMLoc BaseStart, BaseEnd;
    if (parseRegisterWithDollar(BaseReg, BaseStart, BaseEnd))
      return MatchOperand_ParseFail;

    if (getLexer().getKind() != AsmToken::RParen)
      return MatchOperand_ParseFail;

    SMLoc EndLoc = getLexer().getLoc();
    Parser.Lex(); // Eat ')'.

    Operands.push_back(Cpu0Operand::createMem(BaseReg, Expr, StartLoc, EndLoc));
    return MatchOperand_Success;
  }

  Operands.push_back(
      Cpu0Operand::createImm(Expr, StartLoc, getLexer().getLoc()));
  return MatchOperand_Success;
}

bool Cpu0AsmParser::parseOperand(OperandVector &Operands, StringRef Mnemonic) {
  (void)Mnemonic;

  OperandMatchResultTy Match = parseRegisterOperand(Operands);
  if (Match == MatchOperand_Success)
    return false;
  if (Match == MatchOperand_ParseFail)
    return Error(getLexer().getLoc(), "invalid register operand");

  Match = parseImmediateOrMemOperand(Operands);
  if (Match == MatchOperand_Success)
    return false;

  return Error(getLexer().getLoc(), "invalid operand");
}

bool Cpu0AsmParser::ParseInstruction(ParseInstructionInfo &Info, StringRef Name,
                                     SMLoc NameLoc,
                                     OperandVector &Operands) {
  (void)Info;
  Operands.push_back(Cpu0Operand::createToken(Name, NameLoc));

  if (getLexer().is(AsmToken::EndOfStatement)) {
    Parser.Lex();
    return false;
  }

  while (true) {
    if (parseOperand(Operands, Name))
      return true;

    if (getLexer().is(AsmToken::EndOfStatement)) {
      Parser.Lex();
      return false;
    }

    if (getLexer().isNot(AsmToken::Comma))
      return Error(getLexer().getLoc(), "expected ',' or end of statement");

    Parser.Lex(); // Eat ','.

    if (getLexer().is(AsmToken::EndOfStatement))
      return Error(getLexer().getLoc(), "unexpected end of statement");
  }
}

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeCpu0AsmParser() {
  RegisterMCAsmParser<Cpu0AsmParser> X(getTheCpu0Target());
  RegisterMCAsmParser<Cpu0AsmParser> Y(getTheCpu0elTarget());
}
