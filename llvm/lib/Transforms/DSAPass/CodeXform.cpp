#include "CodeXform.h"
#include "DFGAnalysis.h"
#include "Util.h"

#include "dsa-ext/rf.h"

// raw_ostream &operator<<(raw_ostream &os, DFGEntry &DE) {
//   os << "# [" << dsa::xform::KindStr[DE.Kind] << "]" << " DFG" <<
//   DE.Parent->ID << " Entry" << DE.ID; if (DE.UnderlyingInsts().size() == 1) {
//     os << " " << *DE.UnderlyingInst();
//   } else {
//     os << "\n";
//     for (auto Elem : DE.UnderlyingInsts()) {
//       os << *Elem << "\n";
//     }
//   }
//   return os;
// }
//
// raw_ostream &operator<<(raw_ostream &os, DFGBase &DB) {
//   std::ostringstream oss;
//   DB.dump(oss);
//   os << oss.str();
//   return os;
// }

namespace dsa {
namespace xform {

namespace {

/*!
 * \brief To avoid some unexpected iterator issue.
 *        We delete the instructions together.
 * \param Insts The instructions to be removed.
 */
void EliminateInstructions(const std::vector<Instruction *> &Insts) {
  for (auto I : Insts) {
    I->replaceAllUsesWith(UndefValue::get(I->getType()));
    I->eraseFromParent();
  }
}

} // namespace

void EliminateTemporal(Function &F) {
  std::vector<Instruction *> ToRemove;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto Intrin = dyn_cast<IntrinsicInst>(&I)) {
        switch (Intrin->getIntrinsicID()) {
        case Intrinsic::ss_temporal_region_start:
        case Intrinsic::ss_temporal_region_end:
          ToRemove.push_back(Intrin);
          break;
        default:
          break;
        }
      }
    }
  }
  EliminateInstructions(ToRemove);
}

std::vector<utils::StickyRegister> InjectDSARegisterFile(Function &F) {
  IRBuilder<> IB(F.getContext());
  IB.SetInsertPoint(&F.getEntryBlock().back());
  std::vector<utils::StickyRegister> Res;
  std::string TyName("reg.type");
  auto RegType = StructType::create(TyName, IB.getInt64Ty(), IB.getInt8Ty());
  for (int i = 0; i < DSARF::TOTAL_REG; ++i) {
    auto A = IB.CreateAlloca(RegType, nullptr, REG_NAMES[i]);
    auto R = IB.CreateInBoundsGEP(A, {IB.getInt32(0), IB.getInt32(0)},
                                  std::string(REG_NAMES[i]) + ".r.ptr");
    auto S = IB.CreateInBoundsGEP(A, {IB.getInt32(0), IB.getInt32(1)},
                                  std::string(REG_NAMES[i]) + ".s.ptr");
    IB.CreateStore(IB.getInt64(REG_STICKY[i]), R);
    IB.CreateStore(IB.getInt8(i == DSARF::TBC), S);
    Res.emplace_back(A, R, S);
  }
  return Res;
}

std::string ValueToOperandText(Value *Val) {

  if (auto CI = dyn_cast<ConstantInt>(Val)) {
    return formatv("{0}", CI->getValue().getSExtValue());
  } else if (auto CFP = dyn_cast<ConstantFP>(Val)) {
    // TODO: Support more data types
    double FPV = CFP->getValueAPF().convertToDouble();
    uint64_t *RI = reinterpret_cast<uint64_t *>(&FPV);
    return formatv("{0}", *RI);
  }

  CHECK(false) << "Cannot dump " << *Val;
  return "";
}

std::string GetOperationStr(Instruction *Inst, bool isAcc, bool predicated) {
  std::string OpStr;
  int BitWidth = Inst->getType()->getScalarSizeInBits();

  if (auto Cmp = dyn_cast<CmpInst>(Inst)) {
    // TODO: Support floating point
    OpStr = "compare";
    BitWidth = Cmp->getOperand(0)->getType()->getScalarSizeInBits();
  } else if (auto Call = dyn_cast<CallInst>(Inst)) {
    OpStr = Call->getCalledFunction()->getName().str();
  } else {
    OpStr = Inst->getOpcodeName();
    if (isAcc) {
      if (!predicated) {
        OpStr = OpStr[0] == 'f' ? "facc" : "acc";
      } else {
        OpStr = OpStr[0] == 'f' ? "faccumulate" : "accumulate";
      }
    }
  }

  for (int i = 0, e = OpStr[0] == 'f' ? 2 : 1; i < e; ++i) {
    OpStr[i] -= 'a';
    OpStr[i] += 'A';
  }

  return formatv("{0}{1}", OpStr, BitWidth);
}

/*!
 * \brief I anyways need the DFG entries to be reordered in which the order the
 * are emitted. \param DB The DFG to reorder the entries.
 */
static std::vector<DFGEntry *> ReorderEntries(DFGBase *DB) {
  std::vector<DFGEntry *> Res;
  Res.reserve(DB->Entries.size());
  for (auto Elem : DB->EntryFilter<InputPort>()) {
    Res.push_back(Elem);
  }
  for (auto Elem : DB->Entries) {
    if (isa<InputPort>(Elem) || isa<OutputPort>(Elem)) {
      continue;
    }
    Res.push_back(Elem);
  }
  for (auto Elem : DB->EntryFilter<OutputPort>()) {
    Res.push_back(Elem);
  }
  return Res;
}

struct DFGPrinter : dsa::DFGVisitor {

  struct ControlBit {
    std::ostringstream SubCtrl[3];
    Predicate *Pred{nullptr};

    void addControlledMemoryStream(int Idx, DFGEntry *DE) {
      if (auto CMP = dyn_cast<CtrlMemPort>(DE)) {
        if (Pred == nullptr) {
          Pred = CMP->Pred;
        } else {
          CHECK(Pred == CMP->Pred) << "An instruction cannot be controlled by "
                                      "more than one predication.";
        }
        assert(Pred);
        for (int j = 0; j < 3; ++j) {
          if (~CMP->Mask >> j & 1) {
            if (!SubCtrl[j].str().empty()) {
              SubCtrl[j] << "|";
            }
            SubCtrl[j] << "b" << (Idx + 1);
          }
        }
      }
    }

    bool empty() {
      for (int i = 0; i < 3; ++i)
        if (!SubCtrl[i].str().empty())
          return false;
      return true;
    }

    std::string finalize() {
      std::ostringstream Ctrl;
      for (int i = 0; i < 3; ++i) {
        if (!SubCtrl[i].str().empty()) {
          if (!Ctrl.str().empty())
            Ctrl << ", ";
          Ctrl << i << ":" << SubCtrl[i].str();
        }
      }
      return Ctrl.str();
    }

    void updateAbstain(int x, bool isAcc) {
      for (int i = 0; i < 3; ++i) {
        if (x >> i & 1) {
          if (!SubCtrl[i].str().empty()) {
            SubCtrl[i] << "|";
          }
          SubCtrl[i] << "a";
        } else if (isAcc) {
          if (!SubCtrl[i].str().empty()) {
            SubCtrl[i] << "|";
          }
          SubCtrl[i] << "d";
        }
      }
    }
  };

  struct DFGEntryEmitter : DFGEntryVisitor {
    void Visit(DFGEntry *DE) override {
      os << "# [" << KindStr[DE->Kind] << "]: DFG" << DE->Parent->ID << " Entry"
         << DE->ID << "\n";
      int i = 0;
      auto Insts = DE->UnderlyingInsts();
      for (auto Elem : Insts) {
        os << "# Inst";
        if (Insts.size() != 1) {
          os << i++;
        }
        os << ": " << *Elem << "\n";
      }
    }

    void Visit(Accumulator *Acc) {

      Visit(cast<DFGEntry>(Acc));
      auto Inst = Acc->Operation;
      std::queue<std::string> Q;
      auto Reduce = GetOperationStr(Inst, false, false);
      ControlBit CtrlBit;

      for (int vec = 0; vec < Acc->Parent->getUnroll(); ++vec) {
        for (size_t j = 0; j < 2; ++j) {
          auto Operand = Inst->getOperand(j);
          if (!isa<PHINode>(Operand)) {
            auto Entry = Acc->Parent->InThisDFG(Operand);
            CHECK(Entry);
            Q.push(Entry->Name(vec));
          }
        }
      }

      while (Q.size() > 1) {
        auto A = Q.front();
        Q.pop();
        assert(!Q.empty());
        auto B = Q.front();
        Q.pop();
        os << formatv("TMP{0} = {1}({2}, {3})", Parent->TmpCnt, Reduce, A, B)
                  .str()
           << "\n";
        Q.push(formatv("TMP{0}", Parent->TmpCnt++));
      }

      auto P = Acc->getPredicate();
      os << Acc->Name() << " = " << GetOperationStr(Inst, true, P != nullptr)
         << "(" << Q.front() << ", ";

      if (dsa::utils::ModuleContext().PRED) {
        if (P || !CtrlBit.empty()) {
          CtrlBit.updateAbstain(Acc->getAbstainBit(), true);
          os << "ctrl=" << P->Name(-1) << "{" << CtrlBit.finalize() << "}";
        } else {
          os << Acc->Ctrl->Name();
        }
      } else {
        os << "ctrl=" << Acc->Ctrl->Name() << "{2:d}";
      }

      os << ")\n";
    }

    void Visit(CtrlSignal *CS) {
      if (!CS->Controlled->getPredicate() ||
          !dsa::utils::ModuleContext().PRED) {
        Visit(cast<DFGEntry>(CS));
        os << "Input: " << CS->Name() << "\n";
      }
    }

    void Visit(InputConst *IC) {
      Visit(cast<DFGEntry>(IC));
      os << "Input" << IC->Val->getType()->getScalarSizeInBits() << ": "
         << IC->Name() << "\n";
    }

    void Visit(InputPort *IP) override {
      if (!utils::ModuleContext().PRED) {
        for (auto User : IP->UnderlyingInst()->users()) {
          auto Entry = IP->Parent->InThisDFG(User);
          if (Entry && !isa<Predicate>(Entry)) {
            return;
          }
        }
      }
      if (auto IMP = dyn_cast<IndMemPort>(IP)) {
        if (IMP->duplicated()) {
          return;
        }
      }
      Visit(cast<DFGEntry>(IP));
      {
        using dfg::MetaPort;
        auto Meta = IP->Meta;
        if (Meta.source != MetaPort::Data::Unknown) {
          os << "#pragma src=" << dfg::MetaPort::DataText[(int)Meta.source]
             << "\n";
        }
        if (Meta.dest != MetaPort::Data::Unknown) {
          os << "#pragma dest=" << dfg::MetaPort::DataText[(int)Meta.dest]
             << "\n";
        }
        if (!Meta.dest_port.empty()) {
          os << "#pragma dest=" << Meta.dest_port << "\n";
        }
        for (int i = 0; i < (int)MetaPort::Operation::Unknown; ++i) {
          if (Meta.op >> i & 1) {
            os << "#pragma op=" << MetaPort::OperationText[i] << "\n";
          }
        }
        if (Meta.conc) {
          os << "#pragma conc=" << Meta.conc << "\n";
        }
        os << "#pragma cmd=" << Meta.cmd << "\n";
        os << "#pragma repeat=" << Meta.repeat << "\n";
      }

      os << "Input" << IP->UnderlyingInst()->getType()->getScalarSizeInBits()
         << ": " << IP->Name();
      if (IP->ShouldUnroll()) {
        os << "[" << IP->Parent->getUnroll() << "]";
      }
      os << "\n";
    }

    void Visit(ComputeBody *CB) override {
      // If this ComputeBody is handled by a atomic operation skip it!
      if (CB->isAtomic())
        return;

      Visit(cast<DFGEntry>(CB));

      int Degree = (CB->ShouldUnroll() ? CB->Parent->getUnroll() : 1);
      for (int vec = 0; vec < Degree; ++vec) {
        os << CB->Name(vec) << " = "
           << GetOperationStr(CB->Operation, false, false) << "(";
        ControlBit CtrlBit;

        bool isCall = isa<CallInst>(CB->Operation);
        for (int i = 0; i < (int)(CB->Operation->getNumOperands() - isCall);
             ++i) {
          auto Val = CB->Operation->getOperand(i);
          if (i)
            os << ", ";
          if (auto EntryOp = CB->Parent->InThisDFG(Val)) {
            os << EntryOp->Name(vec);
            CtrlBit.addControlledMemoryStream(i, EntryOp);
          } else {
            os << ValueToOperandText(Val);
          }
        }

        if (utils::ModuleContext().PRED) {
          if (CB->getPredicate())
            CtrlBit.updateAbstain(CB->getAbstainBit(), false);
          if (!CtrlBit.empty()) {
            os << ", ctrl=" << CtrlBit.Pred->Name(vec) << "{"
               << CtrlBit.finalize() << "}";
          }
        }

        os << ")\n";
      }
    }

    // TODO(@were): Support unroll
    void Visit(MemPort *MP) override {
      auto &CMI = Parent->CMI;
      auto Belong = CMI.Belong[MP->ID];
      if (Parent->CMI.Clusters[Belong].size() == 1) {
        Visit(static_cast<InputPort*>(MP));
      } else {
        int Degree = (MP->ShouldUnroll() ? MP->Parent->getUnroll() : 1);
        os << "# Cluster " << Belong << "\n";
        Visit(static_cast<DFGEntry*>(MP));
        auto &Cluster = CMI.Clusters[Belong];
        int Bits = MP->UnderlyingInst()->getType()->getScalarSizeInBits();
        int Bytes = Bits / 8;
        int MaxOffset = Cluster.back().Offset;
        os << "# Vector Port Width: " << (MaxOffset / Bytes + 1) << " * " << Degree << "\n";
        if (!Parent->Emitted[Belong]) {
          os << "Input" << Bits << ": ICluster_" << MP->Parent->ID << "_"
             << Belong << "_[" << (MaxOffset / Bytes + 1) * Degree << "]" << "\n";
          Parent->Emitted[Belong] = true;
        }
        auto FCond = [MP](analysis::CoalMemoryInfo::CoalescedEntry &CE) {
          return CE.ID == MP->ID;
        };
        auto Entry = std::find_if(Cluster.begin(), Cluster.end(), FCond);
        CHECK(Entry != Cluster.end());
        for (int i = 0; i < Degree; ++i) {
          os << MP->Name(i) << " = " << "ICluster_" << MP->Parent->ID << "_"
             << Belong << "_" << (Entry->Offset / Bytes) + i * (MaxOffset / Bytes + 1)
             << "\n";
        }
      }
    }

    void Visit(PortMem *PM) override {
      auto &CMI = Parent->CMI;
      auto Belong = CMI.Belong[PM->ID];
      if (Parent->CMI.Clusters[Belong].size() == 1) {
        Visit(static_cast<OutputPort*>(PM));
      } else {
        int Degree = (PM->ShouldUnroll() ? PM->Parent->getUnroll() : 1);
        int Bits = PM->Store->getValueOperand()->getType()->getScalarSizeInBits();
        int Bytes = Bits / 8;
        auto &Cluster = CMI.Clusters[Belong];
        int MaxOffset = Cluster.back().Offset;
        os << "# Cluster " << Belong << "\n";
        Visit(static_cast<DFGEntry*>(PM));
        os << "# Vector Port Width: " << (MaxOffset / Bytes + 1) << " * " << Degree << "\n";
        auto FCond = [PM](analysis::CoalMemoryInfo::CoalescedEntry &CE) {
          return CE.ID == PM->ID;
        };
        auto OV = PM->Parent->InThisDFG(PM->Output);
        CHECK(OV);
        auto Entry = std::find_if(Cluster.begin(), Cluster.end(), FCond);
        CHECK(Entry != Cluster.end());
        for (int i = 0; i < Degree; ++i) {
          os << "OCluster_" << PM->Parent->ID << "_"
             << Belong << "_" << (Entry->Offset / Bytes) + i * (MaxOffset / Bytes + 1)
             << " = " << OV->Name(i) << "\n";
        }
        if (!Parent->Emitted[Belong]) {
          std::ostringstream OSS;
          OSS << "Output" << Bits << ": OCluster_" << PM->Parent->ID << "_"
              << Belong << "_[" << (MaxOffset / Bytes + 1) * Degree << "]";
          Parent->DelayEmission.push_back(OSS.str());
          Parent->Emitted[Belong] = true;
        }
      }
    }

    void Visit(Predicate *P) override {
      if (!utils::ModuleContext().PRED) {
        return;
      }

      {
        // TODO(@were): Finalize this.
        os << "# [Predication] Combination\n";
        for (auto I : P->Cond) {
          os << "# " << *I << "\n";
        }
      }

      ControlBit CtrlBit;
      os << P->Name(-1) << " = Compare"
         << P->Cond[0]->getOperand(0)->getType()->getScalarSizeInBits() << "(";
      for (size_t i = 0; i < P->Cond[0]->getNumOperands(); ++i) {
        auto Val = P->Cond[0]->getOperand(i);
        if (i)
          os << ", ";
        if (auto EntryOp = P->Parent->InThisDFG(Val)) {
          os << EntryOp->Name(-1);
          CtrlBit.addControlledMemoryStream(i, EntryOp);
          if (auto CMP = dyn_cast<CtrlMemPort>(EntryOp))
            CMP->ForPredicate = true;
        } else {
          os << ValueToOperandText(Val);
        }
      }

      if (!CtrlBit.empty()) {
        if (CtrlBit.Pred == P)
          os << ", self=";
        else
          os << ", ctrl=" << CtrlBit.Pred->Name(-1);
        os << "{" << CtrlBit.finalize() << "}";
      }

      os << ")\n";
    }

    void Visit(OutputPort *OP) {
      Visit(cast<DFGEntry>(OP));
      std::ostringstream oss;
      OP->Meta.to_pragma(oss);
      os << oss.str();
      auto Entry = OP->Parent->InThisDFG(OP->Output);
      CHECK(Entry);
      int N = (Entry->ShouldUnroll() ? OP->Parent->getUnroll() : 1);
      for (int i = 0; i < N; ++i) {
        os << OP->Name(i) << " = " << Entry->Name(i) << "\n";
      }
      os << "Output" << OP->Output->getType()->getScalarSizeInBits() << ": "
         << OP->Name();
      if (Entry->ShouldUnroll())
        os << "[" << OP->Parent->getUnroll() << "]";
      os << "\n";
    }

    raw_ostream &os;
    DFGPrinter *Parent;

    DFGEntryEmitter(raw_ostream &os_, DFGPrinter *Parent_)
        : os(os_), Parent(Parent_) {}
  };

  void Visit(DedicatedDFG *DD) override {
    if (utils::ModuleContext().TRIGGER) {
      CHECK(utils::ModuleContext().TEMPORAL)
          << "Trigger cannot be enabled without temporal";
      os << "#pragma group temporal\n";
    }
    DFGVisitor::Visit(DD);
  }

  void Visit(TemporalDFG *TD) override {
    os << "#pragma group temporal\n";
    DFGVisitor::Visit(TD);
  }

  void Visit(DFGBase *DB) {
    // FIXME(@were): Add block freqency an attribute of DFG.
    // auto BF = Parent->Query->BFI->getBlockFreq(getBlocks()[0]);
    // os << "#pragma group frequency " << 1 << "\n";
    os << "#pragma group unroll " << DB->getUnroll() << "\n";

    auto Reordered = ReorderEntries(DB);

    DFGEntryEmitter DEE(os, this);
    for (auto Elem : DB->Entries) {
      Elem->Accept(&DEE);
    }

    // TODO(@were): Bing this back for atomic operations.
    // /// Emit intermediate result for atomic operation
    // for (auto Elem : EntryFilter<ComputeBody>())
    //   Elem->EmitAtomic(os);

    // TODO(@were): Move this to a DFG.
    for (auto Elem : DB->EntryFilter<IndMemPort>()) {
      os << "\n\n----\n\n";
      os << "Input" << Elem->Index->Load->getType()->getScalarSizeInBits()
         << ": indirect_in_" << DB->ID << "_" << Elem->ID << "\n";
      os << "indirect_out_" << DB->ID << "_" << Elem->ID << " = "
         << "indirect_in_" << DB->ID << "_" << Elem->ID << "\n";
      os << "Output" << Elem->Index->Load->getType()->getScalarSizeInBits()
         << ": indirect_out_" << DB->ID << "_" << Elem->ID << "\n";
    }
  }

  raw_ostream &os;
  /*!
   * \brief Count the ticker of temp variables for accumulator reduction.
   */
  int TmpCnt{0};

  /*!
   * \brief The information of memory coalescing.
   */
  analysis::CoalMemoryInfo CMI;
  /*!
   * \brief If this CMI cluster is emitted.
   */
  std::vector<bool> Emitted;
  /*!
   * \brief Clustered output should be delayed.
   */
  std::vector<std::string> DelayEmission;

  DFGPrinter(raw_ostream &os_, analysis::CoalMemoryInfo &CMI_) :
    os(os_), CMI(CMI_), Emitted(CMI.Clusters.size(), false) {}
};

void EmitDFG(raw_ostream &os, DFGFile *DFG, std::vector<analysis::CoalMemoryInfo> &CMIs) {
  auto Graphs = DFG->DFGFilter<DFGBase>();
  CHECK(CMIs.size() == Graphs.size()) << CMIs.size() << " == " << Graphs.size();
  for (int i = 0; i < (int)Graphs.size(); ++i) {
    DFGPrinter DP(os, CMIs[i]);
    auto Elem = Graphs[i];
    if (i) {
      os << "----\n";
    }
    Elem->Accept(&DP);
    for (auto &CO : DP.DelayEmission) {
      os << CO << "\n";
    }
  }
}

void CodeGenContext::InjectFusionHints(std::string Mnemonic, REG &a, REG &b,
                                       int c) {
  // Convert an int-like value to the exact int64 type.
  auto MakeInt64 = [this](Value *Val) -> Value * {
    if (Val->getType()->isPointerTy()) {
      return IB->CreatePtrToInt(Val, IB->getInt64Ty());
    }
    auto Ty = IB->getIntNTy(Val->getType()->getScalarSizeInBits());
    auto RI = IB->CreateBitCast(Val, Ty);
    if (RI->getType()->getScalarSizeInBits() < 64) {
      RI = IB->CreateSExt(RI, IB->getInt64Ty());
    }
    return RI;
  };

  if (!utils::ModuleContext().FUSION) {
    return;
  }
  if (Mnemonic != "ss_cfg_param") {
    return;
  }
  if (c == 98) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    int idx = (c >> (i * 5)) & 31;
    int sticky = (c >> (10 + i)) & 1;
    if (idx) {
      auto AA = IB->CreateLoad(Regs[idx].Reg);
      auto AS = IB->CreateLoad(Regs[idx].Sticky);
      auto AV = MakeInt64(a.value(IB));
      IB->CreateStore(AV, Regs[idx].Reg);
      IB->CreateStore(IB->getInt8(sticky), Regs[idx].Sticky);
      IntrinsicImpl("equal.hint.v1", "r", {IB->CreateICmpEQ(AA, AV)},
                    IB->getVoidTy());
      IntrinsicImpl("equal.hint.s1", "r",
                    {IB->CreateICmpEQ(AS, IB->getInt8(sticky))},
                    IB->getVoidTy());
      auto V = IB->CreateLoad(Regs[idx].Reg);
      (i ? b : a) = V;
    }
  }
}

void CodeGenContext::IntrinsicImpl(const std::string &Mnemonic,
                                   const std::string &OpConstrain,
                                   const std::vector<Value *> &Args,
                                   Type *ResTy) {
  std::ostringstream MOSS;
  MOSS << Mnemonic << " $0";
  for (int i = 0, n = OpConstrain.size(), j = 1; i < n; ++i) {
    if (OpConstrain[i] == ',') {
      MOSS << ", $" << j;
      ++j;
    }
  }
  std::vector<Type *> Types;
  for (auto Arg : Args) {
    Types.push_back(Arg->getType());
  }
  auto FTy = FunctionType::get(ResTy, Types, false);
  auto IA = InlineAsm::get(FTy, MOSS.str(), OpConstrain, true);
  res.push_back(IB->CreateCall(IA, Args));
  if (Mnemonic == "ss_lin_strm" || Mnemonic == "ss_ind_strm" ||
      Mnemonic == "ss_recv" || Mnemonic == "ss_wr_rd") {
    for (int i = 0; i < DSARF::TOTAL_REG; ++i) {
      auto C = IB->CreateTrunc(IB->CreateLoad(Regs[i].Sticky), IB->getInt1Ty());
      auto SV = IB->CreateLoad(Regs[i].Reg);
      auto Reset = IB->CreateSelect(C, SV, IB->getInt64(REG_STICKY[i]));
      IB->CreateStore(Reset, Regs[i].Reg);
    }
  }
}

void InjectConfiguration(CodeGenContext &CGC, analysis::ConfigInfo &CI,
                         Instruction *Start, Instruction *End) {
  auto IB = CGC.IB;
  IB->SetInsertPoint(Start);
  auto GV = IB->CreateGlobalString(CI.Bitstream, CI.Name + "_bitstream",
                                   GlobalValue::ExternalLinkage);
  CGC.SS_CONFIG(GV, IB->getInt64(CI.Size));
  IB->SetInsertPoint(End);
  CGC.SS_WAIT_ALL();
}


/*!
 * \brief Emit port repeat operands;
 *        Note: First is repeat, the raw value. Second is stretch, already shifted by fixed-point.
 * \param CGC The DSA code generator wrapper.
 * \param Loops The loop nest of repeat.
 * \param N The index of loop invariant.
 * \param Unroll The unrolling degree.
 */
std::pair<Value*, Value*>
InjectComputedRepeat(CodeGenContext &CGC,
                     std::vector<dsa::analysis::LinearInfo*> Loops, int N,
                     int Unroll) {
  auto IB = CGC.IB;
  auto &SEE = CGC.SEE;
  Value *Stretch = nullptr;
  Value *Repeat = IB->getInt64(1);
  for (int i = 0; i < N; ++i) {
    if (!Loops[i]->Coef.empty()) {
      CHECK(i == 0)
          << "Only the inner most dimension supports repeat stretch.";
      for (int j = 0; j < N; ++j) {
        if (j == 1) {
          continue;
        }
        auto CI = Loops[i]->Coef[j]->ConstInt();
        CHECK(CI && *CI == 0);
      }
      // FIXME(@were): Fix this for fixed point.
      Stretch = SEE.expandCodeFor(Loops[0]->Coef[1]->Base);
    }
    auto Current = IB->CreateAdd(SEE.expandCodeFor(Loops[i]->Base), IB->getInt64(1));
    if (i == 0 && Unroll != 1) {
      Current = CeilDiv(Current, IB->getInt64(Unroll), IB);
      if (Stretch) {
        Stretch = IB->CreateMul(Stretch, IB->getInt64(1 << DSA_REPEAT_DIGITAL_POINT));
        Stretch = IB->CreateSDiv(Stretch, IB->getInt64(Unroll));
      }
    }
    Repeat = IB->CreateMul(Repeat, Current);
  }
  return {Repeat, Stretch};
}


void InjectLinearStreamImpl(CodeGenContext &CGC, const SCEV *Idx, int MaxOffset,
                            std::vector<Loop*> LoopNest,
                            std::vector<dsa::analysis::LinearInfo *> LoopLI,
                            int Port, int Unroll, int DType, MemoryOperation MO,
                            Padding P, MemoryType MT) {

  auto InjectRepeat = [&CGC](analysis::LinearInfo *LI,
                             const std::vector<analysis::LinearInfo *> &Loops,
                             int Port, int Unroll) {
    CHECK(Loops.size() == LI->Coef.size() || LI->Coef.empty());
    int N = LI->Coef.empty() ? Loops.size() : LI->PatialInvariant();
    // Sanity check on the loop invariants.
    for (int i = 0; i < N; ++i) {
      auto Coef = LI->Coef[i]->ConstInt();
      CHECK(Coef && *Coef == 0);
    }
    auto CR = InjectComputedRepeat(CGC, Loops, N, Unroll);
    auto Repeat = CR.first;
    auto Stretch = CR.second;
    if (auto CI = dyn_cast<ConstantInt>(Repeat)) {
      if (CI->getSExtValue() == 1) {
        return;
      }
    }
    CGC.SS_CONFIG_PORT(Port, DPF_PortRepeat, CGC.MUL(Repeat, 1 << DSA_REPEAT_DIGITAL_POINT));
    if (Stretch) {
      CGC.SS_CONFIG_PORT(Port, DPF_PortRepeatStretch, Stretch);
    }
  };

  auto LinearInstantiation = [&](analysis::LinearInfo *LI, int MaxOffset,
                                 const std::vector<analysis::LinearInfo *> &Loops, int Port,
                                 MemoryOperation MO, Padding P, MemoryType MT, int DType) {
    auto IB = CGC.IB;
    auto &SEE = CGC.SEE;
    if (auto LII = LI->Invariant()) {
      CGC.INSTANTIATE_1D_STREAM(SEE.expandCodeFor(LII), IB->getInt64(0),
                                IB->getInt64(1), Port, P, DSA_Access, MO, MT,
                                DType, 0);
      return;
    }
    CHECK(Loops.size() == LI->Coef.size())
        << Loops.size() << " != " << LI->Coef.size();
    int Dim = Loops.size() - LI->PatialInvariant();
    CHECK(0 <= Dim && Dim <= 3) << Dim;
    auto Start = SEE.expandCodeFor(LI->Base);
    int i = LI->PatialInvariant();
    DSA_INFO << *Start;
    auto CoalesceStrideAndWord = [IB, DType, MaxOffset] (Value *&Stride1D, Value *&N1D) {
      if (auto CI = dyn_cast<ConstantInt>(Stride1D)) {
        int Multiplier = (MaxOffset / DType) + 1;
        DSA_INFO << *N1D << " " << *Stride1D;
        if ((CI->getSExtValue() / DType) == Multiplier) {
          Stride1D = IB->getInt64(DType);
          N1D = IB->CreateMul(N1D, IB->getInt64(Multiplier));
        }
        DSA_INFO << *N1D << " " << *Stride1D;
      }
    };
    switch (Dim) {
    case 0: {
      CGC.INSTANTIATE_1D_STREAM(Start, IB->getInt64(0), IB->getInt64(1), Port,
                                P, DSA_Access, MO, MT, DType, 0);
      break;
    }
    case 1: {
      auto N1D = IB->CreateAdd(SEE.expandCodeFor(Loops[i]->Invariant()), IB->getInt64(1));
      auto Stride1D = SEE.expandCodeFor(LI->Coef[i]->Invariant());
      CoalesceStrideAndWord(Stride1D, N1D);
      CGC.INSTANTIATE_1D_STREAM(Start, Stride1D, N1D, Port, P, DSA_Access, MO, MT, DType, 0);
      break;
    }
    case 2: {
      auto N1D = IB->CreateAdd(SEE.expandCodeFor(Loops[i]->Invariant()), IB->getInt64(1));
      auto Stride1D = SEE.expandCodeFor(LI->Coef[i]->Invariant());
      CoalesceStrideAndWord(Stride1D, N1D);
      // TODO(@were): Make assumption check!
      Value *Stretch = IB->getInt64(0);
      if (!Loops[i]->Coef.empty()) {
        Stretch = SEE.expandCodeFor(Loops[i]->Coef[i + 1]->Base);
      }
      auto Length2D = IB->CreateAdd(SEE.expandCodeFor(Loops[i + 1]->Base), IB->getInt64(1));
      auto Stride2D =
          IB->CreateSDiv(SEE.expandCodeFor(LI->Coef[i + 1]->Invariant()),
                         IB->getInt64(DType));
      CGC.INSTANTIATE_2D_STREAM(Start, Stride1D, N1D, Stride2D, Stretch,
                                Length2D, Port, P, DSA_Access, MO, MT, DType,
                                0);
      break;
    }
    case 3: {
      CHECK(false) << "Unsupported dimension: " << Dim;
      break;
    }
    default:
      CHECK(false) << "Unsupported dimension: " << Dim;
    }
  };

  auto IdxLI = dsa::analysis::AnalyzeIndexExpr(&CGC.SE, Idx, LoopNest);
  if (MO == MemoryOperation::DMO_Read) {
    InjectRepeat(IdxLI, LoopLI, Port, Unroll);
  }
  LinearInstantiation(IdxLI, MaxOffset, LoopLI, Port, MO, P, MT, DType);
}


void InjectStreamIntrinsics(CodeGenContext &CGC, DFGFile &DF, std::vector<analysis::CoalMemoryInfo> &CMIs) {

  auto DFGs = DF.DFGFilter<DFGBase>();

  struct UpdateStreamInjector : DFGEntryVisitor {

    void Visit(MemPort *MP) override {
      // No outer repeat
      if (DLI.TripCount.size() <= 1) {
        return;
      }
      auto DFG = MP->Parent;
      if (!isa<DedicatedDFG>(DFG)) {
        return;
      }
      auto &SE = CGC.SE;
      auto IB = CGC.IB;
      for (auto &PM : DFG->EntryFilter<PortMem>()) {
        if (!PM->IsInMajor() || PM->IntrinInjected)
          continue;
        auto Ptr0 = dyn_cast<Instruction>(MP->Load->getPointerOperand());
        auto Ptr1 = dyn_cast<Instruction>(PM->Store->getPointerOperand());
        if (Ptr0 == Ptr1) {
          DSA_INFO << "Rec: " << *MP->Load << " " << *PM->Store;
          auto SCEV = SE.getSCEV(Ptr0);
          auto Analyzed = dsa::analysis::AnalyzeIndexExpr(&SE, SE.getSCEV(Ptr0), DLI.LoopNest);
          // No repeat!
          if (Analyzed->PatialInvariant() != 0) {
            return;
          }
          // Outer should not be stretched!
          if (!Analyzed->Coef.back()->Coef.empty()) {
            return;
          }
          if (auto O = dyn_cast<ConstantInt>(CGC.SEE.expandCodeFor(Analyzed->Coef.back()->Base))) {
            if (O->getSExtValue()) {
              return;
            }
          }
          using LinearInfo = dsa::analysis::LinearInfo;
          auto DType = MP->Load->getType()->getScalarSizeInBits() / 8;
          InjectLinearStreamImpl(CGC, SCEV, /*TODO(@were): Support this later*/0,
                                 std::vector<Loop*>(DLI.LoopNest.begin(), DLI.LoopNest.end() - 1),
                                 std::vector<LinearInfo*>(DLI.TripCount.begin(), DLI.TripCount.end() - 1),
                                 MP->SoftPortNum, DFG->getUnroll(),
                                 DType, DMO_Read,
                                 (Padding) MP->FillMode(), DMT_DMA);

          if (DLI.TripCount.size() == 2) {
            // TODO(@were): Support stretch later.
            auto InnerN = CGC.SEE.expandCodeFor(DLI.TripCount[0]->Base);
            auto OuterN = CGC.SEE.expandCodeFor(DLI.TripCount[1]->Base);
            CGC.SS_RECURRENCE(PM->SoftPortNum, MP->SoftPortNum,
                              IB->CreateMul(IB->CreateAdd(InnerN, IB->getInt64(1)), OuterN),
                              DType);
            DSA_INFO << *InnerN << ", " << *OuterN;
            DSA_INFO << *CGC.res.back();
          } else {
            CHECK(false) << "Not supported yet";
          }

          InjectLinearStreamImpl(CGC, SCEV, /*TODO(@were): Support this later*/0,
                                 std::vector<Loop*>(DLI.LoopNest.begin(), DLI.LoopNest.end() - 1),
                                 std::vector<LinearInfo*>(DLI.TripCount.begin(), DLI.TripCount.end() - 1),
                                 PM->SoftPortNum, DFG->getUnroll(),
                                 DType, DMO_Write, (Padding) 0, DMT_DMA);
          int II = 1;
          int Conc = -1;
          if (auto CI = dyn_cast<ConstantInt>(CGC.SEE.expandCodeFor(DLI.TripCount[0]->Base))) {
            auto CII = CI->getZExtValue();
            int CanHide = CII * DType;
            if (PM->Latency - CanHide > 0) {
              II = PM->Latency - CanHide;
              DSA_INFO << "Recur II: " << II;
            } else {
              II = 1;
              DSA_WARNING << "To hide the latency " << PM->Latency << ", "
                          << CII * DType << " elements are active";
              DSA_WARNING << "This requires a " << CanHide - PM->Latency
                          << "-deep FIFO buffer";
            }
            Conc = CII;
          } else {
            errs() << "[Warning] To hide the latency " << PM->Latency
                   << ", make sure your variable recur distance will not overwhlem "
                      "the FIFO buffer!";
          }
          PM->Meta.set("dest", "localport");
          PM->Meta.set("dest", MP->Name());
          {
            std::ostringstream oss;
            oss << Conc * DType;
            PM->Meta.set("conc", oss.str());
          }
          PM->IntrinInjected = MP->IntrinInjected = true;
          return;
        }
      }
    }

    CodeGenContext &CGC;
    analysis::DFGLoopInfo DLI;
    UpdateStreamInjector(CodeGenContext &CGC_, analysis::DFGLoopInfo DLI_) :
      CGC(CGC_), DLI(DLI_.LoopNest, DLI_.TripCount) {}
  };

  struct StreamIntrinsicInjector : DFGEntryVisitor {

    void Visit(IndMemPort *IMP) override {
      if (IMP->IntrinInjected)
        return;

      auto IB = CGC.IB;

      if (!dsa::utils::ModuleContext().IND) {
        CGC.SS_CONST(IMP->SoftPortNum, IMP->Load, IB->getInt64(1),
                     IMP->Load->getType()->getScalarSizeInBits() / 8);
        IMP->IntrinInjected = true;
        IMP->Meta.set("src", "memory");
        IMP->Meta.set("cmd", "0.1");
        return;
      }

      if (IMP->duplicated()) {
        IMP->IntrinInjected = true;
        return;
      }

      std::vector<IndMemPort *> Together;
      std::vector<int> INDs;

      for (auto &IMP1 : IMP->Parent->EntryFilter<IndMemPort>()) {
        if (IMP1->IntrinInjected)
          continue;
        if (IMP->Index->Load == IMP1->Index->Load) {
          Together.push_back(IMP1);
          CHECK(IMP1->Index->SoftPortNum != -1);
          INDs.push_back(IMP1->Index->SoftPortNum);
        }
      }

      CHECK(Together[0] == IMP);

      for (size_t i = 1; i < Together.size(); ++i) {
        CGC.SS_CONFIG_PORT(INDs[i], DPF_PortBroadcast, true);
      }

      auto Index = IMP->Index->Load->getPointerOperand();
      auto Analyzed = IMP->Parent->AnalyzeDataStream(
          Index, IMP->Index->Load->getType()->getScalarSizeInBits() / 8);
      InjectLinearStreamImpl(CGC, CGC.SE.getSCEV(Index), /*TODO(@were): Support SLP*/0,
                             DLI.LoopNest, DLI.TripCount, IMP->Index->SoftPortNum,
                             IMP->Parent->getUnroll(), IMP->Index->Load->getType()->getScalarSizeInBits() / 8,
                             DMO_Read, DP_NoPadding, DMT_DMA);
      // IMP->Parent->InjectRepeat(Analyzed.AR, nullptr, IMP->SoftPortNum);
      // auto ActualSrc = dsa::inject::InjectLinearStream(
      //     IB, CGC.Regs, IMP->Index->SoftPortNum, Analyzed, DMO_Read,
      //     DP_NoPadding, DMT_DMA,
      //     IMP->Index->Load->getType()->getScalarSizeInBits() / 8);

      for (size_t i = 0; i < Together.size(); ++i) {
        auto Cur = Together[i];
        int IBits = Cur->Index->Load->getType()->getScalarSizeInBits();
        auto Num = IB->CreateUDiv(Analyzed.BytesFromMemory(IB),
                                  IB->getInt64(IBits / 8));
        auto GEP = dyn_cast<GetElementPtrInst>(Cur->Load->getPointerOperand());
        CGC.INSTANTIATE_1D_INDIRECT(
            IMP->SoftPortNum, IMP->Load->getType()->getScalarSizeInBits() / 8,
            IMP->IndexOutPort, IMP->Index->Load->getType()->getScalarSizeInBits() / 8,
            GEP->getOperand(0), Cur->Load->getType()->getScalarSizeInBits() / 8,
            Num, DMT_DMA, DMO_Read);

        // TODO: Support indirect read on the SPAD(?)
        Cur->Meta.set("src", dsa::dfg::MetaPort::DataText[(int)DMT_DMA]);
        Cur->Meta.set("dest", "memory");
        Cur->Meta.set("op", "indread");

        Cur->IntrinInjected = true;
      }
      IMP->IntrinInjected = true;
    }

    void Visit(MemPort *MP) override {
      auto IB = CGC.IB;
      IB->SetInsertPoint(MP->Parent->DefaultIP());

      if (MP->IntrinInjected)
        return;

      // Wrappers
      auto DFG = MP->Parent;
      int Belong = CMI.Belong[MP->ID];
      int MaxOffset = CMI.Clusters[Belong].back().Offset;
      int ID = CMI.Clusters[Belong].front().ID;
      auto MP0 = dyn_cast<MemPort>(DFG->Entries[ID]);
      int Port = CMI.Clusters[Belong].size() == 1 ? MP->SoftPortNum : CMI.ClusterPortNum[Belong];
      // Handle data stream with predicate
      if (MP->getPredicate()) {
        CHECK(false) << "Predicated stream should be handled in CtrlMemPort.";
      }
      auto SCEVIdx = CGC.SE.getSCEV(MP0->Load->getPointerOperand());
      int DType = MP->Load->getType()->getScalarSizeInBits() / 8;
      InjectLinearStreamImpl(CGC, SCEVIdx, MaxOffset, DLI.LoopNest, DLI.TripCount, Port,
                             DFG->getUnroll(), DType, DMO_Read, (Padding) MP->FillMode(),
                             DMT_DMA);

      for (auto &Elem : CMI.Clusters[Belong]) {
        auto PB = dyn_cast<PortBase>(DFG->Entries[Elem.ID]);
        CHECK(PB);
        PB->IntrinInjected = true;
      }
    }

    void Visit(PortMem *PM) override {
      auto DFG = PM->Parent;
      if (PM->IntrinInjected)
        return;
      int Belong = CMI.Belong[PM->ID];
      int MaxOffset = CMI.Clusters[Belong].back().Offset;
      int ID = CMI.Clusters[Belong].front().ID;
      auto PM0 = dyn_cast<MemPort>(DFG->Entries[ID]);
      int Port = CMI.Clusters[Belong].size() == 1 ? PM0->SoftPortNum : CMI.ClusterPortNum[Belong];

      CGC.IB->SetInsertPoint(DFG->DefaultIP());
      CGC.SEE.setInsertPoint(DFG->DefaultIP());
      auto SCEVIdx = CGC.SE.getSCEV(PM->Store->getPointerOperand());
      int DType = PM->Store->getValueOperand()->getType()->getScalarSizeInBits() / 8;
      InjectLinearStreamImpl(CGC, SCEVIdx, MaxOffset, DLI.LoopNest, DLI.TripCount, Port,
                             DFG->getUnroll(), DType, DMO_Write, DP_NoPadding, DMT_DMA);

      for (auto &Elem : CMI.Clusters[Belong]) {
        auto PB = dyn_cast<PortBase>(DFG->Entries[Elem.ID]);
        CHECK(PB);
        PB->IntrinInjected = true;
      }
    }

    void Visit(InputConst *IC) {
      auto &LoopNest = DLI.LoopNest;
      for (int i = 0; i < (int) LoopNest.size(); ++i) {
        CHECK(LoopNest[i]->isLoopInvariant(IC->Val))
          << *IC->Val << " is not a loop invariant under " << *LoopNest[i];
      }
      auto CR =
        InjectComputedRepeat(CGC, DLI.TripCount, DLI.TripCount.size(), IC->Parent->getUnroll());
      CHECK(!CR.second) << "Should not be stretched!";
      CGC.SS_CONST(IC->SoftPortNum, IC->Val, CR.first, IC->Val->getType()->getScalarSizeInBits() / 8);
      IC->IntrinInjected = true;
    }

    void Visit(CtrlSignal *CS) {
      auto DD = dyn_cast<DedicatedDFG>(CS->Parent);
      CHECK(DD);
      auto InsertBefore = DD->DefaultIP();
      auto One = CGC.IB->getInt64(1);
      auto Two = CGC.IB->getInt64(2);

      if (CS->Controlled->getPredicate()) {
        if (dsa::utils::ModuleContext().PRED) {
          CS->IntrinInjected = true;
        } else {
          CGC.SS_CONST(CS->SoftPortNum, Two, One,
                       Two->getType()->getScalarSizeInBits() / 8);
          CGC.SS_CONST(CS->SoftPortNum, One, One,
                       Two->getType()->getScalarSizeInBits() / 8);
        }
        CS->IntrinInjected = true;
        return;
      }

      CHECK(!CS->IntrinInjected);
      auto CR = CS->Controlled->NumValuesProduced();
      assert(CR);

      /// {
      auto IB = CGC.IB;
      auto Iters =
          IB->CreateUDiv(DD->ProdTripCount(DD->LoopNest.size(), InsertBefore),
                         CR, "const.iters");
      auto Unroll = IB->getInt64(DD->UnrollFactor);
      auto Ceil = CeilDiv(CR, Unroll, InsertBefore);
      auto RepeatTwo = IB->CreateSub(Ceil, One, "repeat.two", InsertBefore);
      CGC.SS_2D_CONST(CS->SoftPortNum, Two, RepeatTwo, One, One, Iters,
                      Two->getType()->getScalarSizeInBits() / 8);
      /// }

      CS->IntrinInjected = true;
    }

    void Visit(CtrlMemPort *CMP) {

      auto IB = CGC.IB;
      auto One = IB->getInt64(1);
      auto Zero = IB->getInt64(0);
      IB->SetInsertPoint(CMP->Parent->DefaultIP());

      if (!dsa::utils::ModuleContext().PRED) {
        // PortNum == -1 indicates this port is omitted because of no
        // predication support
        if (CMP->SoftPortNum != -1) {
          CGC.SS_CONST(CMP->SoftPortNum, CMP->Load, One,
                       CMP->Load->getType()->getScalarSizeInBits() / 8);
          auto DD = dyn_cast<DedicatedDFG>(CMP->Parent);
          CHECK(DD && "This node only exists in stream DFGs");
          CGC.SS_CONST(CMP->SoftPortNum, Zero, One);
        }
        CMP->IntrinInjected = true;
        return;
      }

      // If this stream is for the predicate, we require users to put sentinal
      // at the end the next word of the array.
      int DType = CMP->Load->getType()->getScalarSizeInBits();
      CGC.INSTANTIATE_1D_STREAM(CMP->Start, DType, CMP->TripCnt,
                                CMP->SoftPortNum, 0, DSA_Access, DMO_Read,
                                DMT_DMA, DType, 0);

      CMP->Meta.set("op", "read");
      CMP->Meta.set("src", "memory");

      // If this stream is NOT for the predicate, we pad two zero values to
      // produce the final result.
      if (!CMP->ForPredicate) {
        CGC.SS_CONST(CMP->SoftPortNum, IB->getInt64(0), IB->getInt64(1), DType);
      }

      // This is kinda ad-hoc, because we current do not support a
      // runtime-determined length stream. This assumption does not work on that
      // case.

      // FIXME: Automatically inject sentinal tail
      // if (CMP->ForPredicate) {
      //  auto Sentinal = createConstant(getCtx(), 1ull << (Bits - 1), Bits);
      //  createAssembleCall(VoidTy, "ss_const $0, $1, $2", "r,r,i",
      //                     {Sentinal, One, createConstant(getCtx(),
      //                     CMP->SoftPortNum)}, DFG->DefaultIP());
      //} else {
      //  createAssembleCall(VoidTy, "ss_const $0, $1, $2", "r,r,i",
      //                     {createConstant(getCtx(), 0), One,
      //                     createConstant(getCtx(), CMP->SoftPortNum)},
      //                     DFG->DefaultIP());
      //}

      CMP->IntrinInjected = true;
    }

    void Visit(OutputPort *OP) {
      auto Recv = CGC.SS_RECV(OP->SoftPortNum,
                              OP->Output->getType()->getScalarSizeInBits() / 8)
                      .value(CGC.IB);
      Recv = CGC.IB->CreateBitCast(Recv, OP->Output->getType());
      std::set<Instruction *> Equiv;
      FindEquivPHIs(OP->Output, Equiv);
      for (auto Iter : Equiv) {
        Iter->replaceAllUsesWith(Recv);
      }
      OP->IntrinInjected = true;
    }

    void Visit(StreamInPort *SIP) {
      auto f = [this, SIP] () -> StreamOutPort* {
        for (auto DFG : SIP->Parent->Parent->DFGFilter<DFGBase>()) {
          for (auto Elem : DFG->Entries) {
            if (auto Casted = dyn_cast<StreamOutPort>(Elem)) {
              if (SIP->DataFrom == Casted->UnderlyingInst()) {
                return Casted;
              }
            }
          }
        }
        CHECK(false);
        return nullptr;
      };
      StreamOutPort *SOP = f();
      auto IB = CGC.IB;
      auto &SEE = CGC.SEE;
      assert(!SIP->IntrinInjected);
      if (auto DD = dyn_cast<DedicatedDFG>(SIP->Parent)) {
        int Bytes =
            SOP->Output->getType()->getScalarType()->getScalarSizeInBits() / 8;
        int i = 0;
        for (i = 0; i < (int) DLI.LoopNest.size(); ++i) {
          if (!DLI.LoopNest[i]->isLoopInvariant(SOP->UnderlyingInst())) {
            break;
          }
        }
        auto Repeat = InjectComputedRepeat(CGC, DLI.TripCount, i, DD->getUnroll());
        CGC.SS_REPEAT_PORT(SIP->SoftPortNum, Repeat.first);
        if (Repeat.second) {
          CGC.SS_CONFIG_PORT(SIP->SoftPortNum, DPF_PortRepeatStretch, Repeat.second);
        }
        Value *N = IB->getInt64(1);
        for (; i < (int) DLI.LoopNest.size(); ++i) {
          N = IB->CreateMul(N, IB->CreateAdd(SEE.expandCodeFor(DLI.TripCount[i]->Base), IB->getInt64(1)));
        }
        CGC.SS_RECURRENCE(SOP->SoftPortNum, SIP->SoftPortNum, N, Bytes);
      } else if (isa<TemporalDFG>(SIP->Parent)) {
        // TODO: Support temporal stream
        CGC.SS_RECURRENCE(SOP->SoftPortNum, SIP->SoftPortNum, IB->getInt64(1),
                          SOP->Output->getType()->getScalarSizeInBits() / 8);
      }
      SIP->IntrinInjected = true;
    }

    void Visit(StreamOutPort *SOP) {
      assert(!SOP->IntrinInjected);
      SOP->IntrinInjected = true;
    }

    void Visit(AtomicPortMem *APM) {
      // Open these when bringing APM back.
      CHECK(false) << "Not implemented yet!";
      // if (!dsa::utils::ModuleContext().IND) {
      //   if (auto DFGEntry = Parent->InThisDFG(Operand)) {
      //     assert(false && "TODO: ss_recv operand from port!");
      //     (void) DFGEntry;
      //   } else {
      //     (void) DFGEntry;
      //   }
      //   IntrinInjected = true;
      //   Meta.set("cmd", "0.1");
      //   return;
      // }
      // // TODO: Support OpCode() == 3.
      // assert(OpCode == 0);

      // SoftPortNum = Parent->getNextIND();

      // auto Index = this->Index->Load->getPointerOperand();
      // auto Analyzed = Parent->AnalyzeDataStream(
      //   Index, this->Index->Load->getType()->getScalarSizeInBits() / 8);
      // Parent->InjectRepeat(Analyzed.AR, nullptr, SoftPortNum);
      // auto ActualSrc =
      //   dsa::inject::InjectLinearStream(IB, Parent->Parent->Query->DSARegs,
      //                                   SoftPortNum, Analyzed, DMO_Read,
      //                                   DP_NoPadding, DMT_DMA,
      //                                   this->Index->Load->getType()->getScalarSizeInBits()
      //                                   / 8);
      // int OperandPort = -1;
      // auto DD = dyn_cast<DedicatedDFG>(Parent);
      // assert(DD);
      // // FIXME: Is trip count a temporary solution?
      // auto TripCnt = DD->ProdTripCount(DD->LoopNest.size(), DD->DefaultIP());

      // if (auto DFGEntry = Parent->InThisDFG(Operand)) {
      //   if (auto CB = dyn_cast<ComputeBody>(DFGEntry)) {
      //     assert(CB->isImmediateAtomic());
      //     OperandPort = CB->SoftPortNum;
      //   } else if (auto IP = dyn_cast<InputPort>(DFGEntry)) {
      //     OperandPort = IP->SoftPortNum;
      //   }
      // } else {
      //   OperandPort = Parent->getNextReserved();
      //   uint64_t CI = stoul(ValueToOperandText(Operand));
      //   int DBits =
      //   this->Store->getValueOperand()->getType()->getScalarSizeInBits();
      //   createAssembleCall(IB->getVoidTy(), "ss_const $0, $1, $2", "r,r,i",
      //                      {IB->getInt64(CI), TripCnt,
      //                       IB->getInt64(OperandPort | ((TBits(DBits) + 1) <<
      //                       9))},
      //                      Parent->DefaultIP());
      // }

      // int IBits = this->Index->Load->getType()->getScalarSizeInBits();
      // int DBits =
      // this->Store->getValueOperand()->getType()->getScalarSizeInBits();

      // auto TImm = IB->getInt64((TBits(DBits) << 4) | (TBits(DBits) << 2) |
      // TBits(IBits)); auto NumUpdates = IB->getInt64(1); auto TupleLen =
      // IB->getInt64(1); createAssembleCall(IB->getVoidTy(), "ss_cfg_atom_op
      // $0, $1, $2", "r,r,i",
      //                    {TupleLen, NumUpdates, TImm},
      //                    Parent->DefaultIP());
      // Meta.set("src", dsa::dfg::MetaPort::DataText[(int) ActualSrc]);
      // Meta.set("dest", "spad");
      // Meta.set("op", "atomic");
      // // FIXME: offset is not always 0
      // uint32_t OffsetList = 0;
      // auto AddrPort = IB->getInt64(OffsetList | (SoftPortNum << 24));
      // auto ValuePort = IB->getInt64((OperandPort << 2) | ((int) OpCode));
      // createAssembleCall(IB->getVoidTy(), "ss_atom_op $0, $1, $2", "r,r,i",
      //                    {AddrPort, TripCnt, ValuePort},
      //                    Parent->DefaultIP());

      // IntrinInjected = true;
    }

    CodeGenContext &CGC;
    analysis::DFGLoopInfo &DLI;
    analysis::CoalMemoryInfo &CMI;
    StreamIntrinsicInjector(CodeGenContext &CGC_, analysis::DFGLoopInfo &DLI_, analysis::CoalMemoryInfo &CMI_) :
      CGC(CGC_), DLI(DLI_), CMI(CMI_) {}
  };

  for (int i = 0; i < (int) DFGs.size(); ++i) {
    auto DFG = DFGs[i];
    auto DLI = analysis::AnalyzeDFGLoops(DFG, CGC.SE);
    UpdateStreamInjector USI(CGC, DLI);
    StreamIntrinsicInjector SII(CGC, DLI, CMIs[i]);
    CGC.IB->SetInsertPoint(DFG->DefaultIP());
    CGC.SEE.setInsertPoint(DFG->DefaultIP());
    for (auto Elem : ReorderEntries(DFG)) {
      Elem->Accept(&USI);
      Elem->Accept(&SII);
    }
  }

  for (auto &DD : DFGs) {
    for (auto &PB : DD->EntryFilter<PortBase>()) {
      CHECK(PB->IntrinInjected);
    }
  }
}

} // namespace xform
} // namespace dsa
