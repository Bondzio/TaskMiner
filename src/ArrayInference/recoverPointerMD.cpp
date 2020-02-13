//===-------------------------- recoverCode.cpp ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the Universidade Federal de Minas Gerais -
// UFMG Open Source License. See LICENSE.TXT for details.
//
// Copyright (C) 2015   Gleison Souza Diniz Mendon?a
//
//===----------------------------------------------------------------------===//
//
// RecoverCode is a class created for generate C code using  PtrRangeAnalysis
// to define the data limits used to access a pointer in some loop.
// In summary, this class translate the access expressions in LLVM's I.R. and
// use the original name of variables to write the correct parallel code.
//
// The name of variables generated by pass stay in NAME string. The 
// programmer can change this name in writeExpressions.h
//
// This class is used in the WriteExpressions optimization, and its just rewrite
// the instructions in LLVM's IR to C code. Its will optimize the code available, 
// using for example:
//
//    -> propagation of the constants
//    -> reuse of the generated code
//    -> removing of the redundant instructions
//    -> removing of the dead code
//    -> simplifications of the constans
//    -> simplifications of the generated code
//
//===----------------------------------------------------------------------===//

#include <fstream>
#include <queue>

#include "llvm/IR/DIBuilder.h" 
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/MC/MCExpr.h"
#include <llvm/Transforms/Utils/BasicBlockUtils.h>

#include "recoverPointerMD.h"

using namespace llvm;
using namespace std;
using namespace lge;

Value *RecoverPointerMD::getBasePtr(Value *V) {
  if (!isa<Instruction>(V))
    return V;
  Instruction *Inst = cast<Instruction>(V);
  if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst) || 
      isa<GetElementPtrInst>(Inst)) {
    Value *BasePtrV = getPointerOperand(Inst);
    while (isa<LoadInst>(BasePtrV) || isa<GetElementPtrInst>(BasePtrV)) {
      if (LoadInst *LD = dyn_cast<LoadInst>(BasePtrV))
        BasePtrV = LD->getPointerOperand();
      if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(BasePtrV))
        BasePtrV = GEP->getPointerOperand();
    }
    return BasePtrV;
  }
  return nullptr;
}

Value *RecoverPointerMD::getPointerOperand(Instruction *Inst) {
  if (LoadInst *Load = dyn_cast<LoadInst>(Inst))
    return Load->getPointerOperand();
  else if (StoreInst *Store = dyn_cast<StoreInst>(Inst))
    return Store->getPointerOperand();
  else if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst))
    return GEP->getPointerOperand();

  return 0;
}

std::string RecoverPointerMD::recoverBitcastOf(Value *V, std::string name, int *var,
                            const DataLayout* DT) {
  if (!isa<BitCastInst>(V))
    return std::string();
  if (rn->getOriginalName(V) != std::string())
    return rn->getOriginalName(V); 
  BitCastInst *BIT = cast<BitCastInst>(V);
  Type *ty1 = BIT->getSrcTy();
  Type *ty2 = BIT->getDestTy();
  std::string result = std::string();
  std::string result2 = std::string();
  unsigned int s1 = getSizeToType(ty1, DT);
  unsigned int s2 = getSizeToType(ty2, DT);
  double factor = s2 / s1;
  result = std::to_string(factor) + " * ";
  result2 = getAccessString(BIT->getOperand(0), name, var, DT);
  long long int num = 0;
  if ((*var == -1) && TryConvertToInteger(result2, &num)) {
    factor = num * factor;
    return std::to_string(factor);
  }
  result = std::to_string(factor) + " * ";
  result += result2 + ";\n";
  insertComputedValue(V, var, result);
  return result;
}

std::string RecoverPointerMD::recoverLoadMD (Value *V, std::string name, int *var,
                            const DataLayout* DT) {
  if (!isa<LoadInst>(V)) {
    setValidFalse();
    return std::string();
  }
  LoadInst *LD = cast<LoadInst>(V);

  std::string result = getAccessStringMD(LD->getPointerOperand(), name, var, DT); 
  if (name == result) {
    return std::string();
  }
  return result;
}

std::string RecoverPointerMD::recoverStoreMD (Value *V, std::string name, int *var,
                            const DataLayout* DT) {
  if (!isa<StoreInst>(V)) {
    setValidFalse();
    return std::string();
  }
  StoreInst *ST = cast<StoreInst>(V);
  int op1 = -1, op2 = -1;
  std::string rPtr = getAccessStringMD(ST->getPointerOperand(), name, &op1, DT);
  *var = op1;
  return rPtr;
  // Exit to TaskMiner
  std::string rDta = getAccessStringMD(ST->getValueOperand(), name, &op2, DT);
  std::string result = rPtr;

  if (op1 != -1) 
    result = NAME + "[" + std::to_string(op1) + "]";
  
  result += " = ";
  if (op2 != -1)
    result += NAME + "[" + std::to_string(op2) + "];\n";
  else
    result += rDta + ";\n";
    
  return result;
}

bool RecoverPointerMD::hasGEPasOperand(Value *V) {
  std::map<Value*, bool> KV;
  return hasGEPasOperand(V, KV);
}

bool RecoverPointerMD::hasGEPasOperand(Value *V, std::map<Value*, bool> & KV) {
  if (!isa<Instruction>(V))
    return false;
  Instruction *I = cast<Instruction>(V);
  KV[V] = true;
  if (isa<GetElementPtrInst>(I))
    return true;
  for (unsigned int i = 0; i < I->getNumOperands(); i++) {
    if (KV.count(I->getOperand(i)))
      continue;
    if (hasGEPasOperand(I->getOperand(i), KV))
      return true;
  }
  return false;
}

bool RecoverPointerMD::hasLOADasOperand(Value *V) {
  std::map<Value*, bool> KV;
  return hasLOADasOperand(V, KV);
}

bool RecoverPointerMD::hasLOADasOperand(Value *V, std::map<Value*, bool> & KV) {
  if (!isa<Instruction>(V))
    return false;
  Instruction *I = cast<Instruction>(V);
  KV[V] = true;
  if (isa<LoadInst>(I))
    return true;
  for (unsigned int i = 0; i < I->getNumOperands(); i++) {
    if (KV.count(I->getOperand(i)))
      continue;
    if (hasGEPasOperand(I->getOperand(i), KV))
      return true;
  }
  return false; 
}

bool RecoverPointerMD::hasSameTypes(Type *Ty1, Type *Ty2) {
  if (Ty1 != Ty2)
    return false;
  Type::subtype_iterator It1 = Ty1->subtype_begin();
  Type::subtype_iterator It2 = Ty2->subtype_begin();
  Type::subtype_iterator Ite1 = Ty1->subtype_end();
  Type::subtype_iterator Ite2 = Ty2->subtype_end();
  for (;(It1 != Ite1) && (It2 != Ite2); It1++, It2++)
    if (!hasSameTypes(*It1, *It2))
      return false;
  if ((It1 != Ite1) && (It2 != Ite2))
    return false;
  return true;
}

DIType *RecoverPointerMD::getDITypeElement(DIType *dity, int elem) {
  if (DIBasicType *bty = dyn_cast<DIBasicType>(dity))
    return bty;
  if (DICompositeType *bty = dyn_cast<DICompositeType>(dity))
    if (MDNode *md = dyn_cast<MDNode>(bty->getRawElements())) {
      if (md->getNumOperands() < elem)
      if (DICompositeType *bty2 = dyn_cast<DICompositeType>(bty->getBaseType()))
        return getDITypeElement(bty2, elem);
      if (DIType *ditty = dyn_cast<DIType>(md->getOperand(elem)))
        return ditty;
    }
  if (DIDerivedType *bty = dyn_cast<DIDerivedType>(dity))
    if (DIType *ditty = dyn_cast<DIType>(bty->getBaseType()))
      return ditty;
  return dity;
}

std::string RecoverPointerMD::recoverGEPMD (Value *V, std::string name, int *var,
                            const DataLayout* DT) {
  if (!isa<GetElementPtrInst>(V))
    return std::string();
  GetElementPtrInst *GEP = cast<GetElementPtrInst>(V);

  if (!GEP->hasIndices() || GEP->hasAllZeroIndices())
    return getAccessStringMD(GEP->getPointerOperand(), name, var, DT);
   
  int opPtr = -1;

  Value *BasePtrV = GEP->getPointerOperand();
  
  while (isa<LoadInst>(BasePtrV) || isa<GetElementPtrInst>(BasePtrV)) {
    if (rn->getOriginalName(V) != std::string())
      break;
    if (LoadInst *LD = dyn_cast<LoadInst>(BasePtrV))
      BasePtrV = LD->getPointerOperand();
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(BasePtrV))
      BasePtrV = GEP->getPointerOperand();
  }

  const llvm::Function *F = rn->findEnclosingFunc(BasePtrV);
  if (!F)
    return std::string();
  const DILocalVariable *DILVar = rn->findVar(BasePtrV, F);
  
  if ((!DILVar) || !isa<DIType>(DILVar->getType())) 
    return std::string();
  DIType *dity = cast<DIType>(DILVar->getType());

  std::string result = getAccessStringMD(GEP->getPointerOperand(), name, &opPtr, DT);

  if (opPtr != -1) 
    result = NAME + "[" + std::to_string(opPtr) + "]";

  Type *ty = GEP->getPointerOperandType();
  unsigned int i = 1;
  if (isa<GetElementPtrInst>(GEP->getPointerOperand())) {
    i = 2; 
    ty = getInternalType(ty,0,DT);
    dity = getDITypeElement(dity, 0); 
  }
  for (; i < GEP->getNumOperands(); i++) {
    std::string tmpRes = std::string();
    int op = -1;
    long long int value = 0;
    bool isOfStruct = false;
    if (ty->getTypeID() == Type::StructTyID) {
      return std::string();
      /*
      if(isa<DICompositeType>(dity)) {
        if (Constant *C = dyn_cast<Constant>(GEP->getOperand(i))) {
          ConstantsSimplify CS;
          value = CS.getUniqueConstantInteger(C, BasePtrV, DT);
          if (!CS.isValid())
            return std::string();
          dity = getDITypeElement(dity, value);
          ty = getInternalType(ty,value,DT); 
          tmpRes = "."; 
          tmpRes += dity->getName();
          result += tmpRes;
          isOfStruct = true;
        }
      }*/
    }
    if (!isOfStruct) {
      result += "[";
      tmpRes = getAccessStringMD(GEP->getOperand(i), name, &op, DT);
      if (op != -1)
        tmpRes = NAME + "[" + std::to_string(op) + "]";
      result += tmpRes + "]";
      dity = getDITypeElement(dity, 0); 
      ty = getInternalType(ty,0,DT); 
    }
  }
  return result;
}

// This void return the Name of some Variable for some memory access
// instruction:
std::string RecoverPointerMD::recoverNameOf (Value *V, std::string name, int *var,
                                              const DataLayout *DT) {
  if (((!isa<LoadInst>(V) && !isa<StoreInst>(V)) 
        && (!isa<AllocaInst>(V) && !isa<GlobalVariable>(V)))
        && ((!isa<GetElementPtrInst>(V) && !isa<Argument>(V))
        && !isa<CallInst>(V)) && !isa<PHINode>(V))
    return std::string();

  *var = -1;
  RecoverNames::VarNames nameF = rn->getNameofValue(V);

  if (isa<AllocaInst>(V) || isa<GlobalVariable>(V) || isa<Argument>(V)
        || isa<CallInst>(V) || isa<PHINode>(V)) {
    if (name == nameF.nameInFile)
      return "0";
   
    if (nameF.nameInFile == std::string()) {
      setValidFalse();     
      return std::string();
    }
    
    return nameF.nameInFile;
  }
  if (isa<LoadInst>(V)) {
    return recoverLoadMD(V, name, var, DT);
  }
  else if (isa<StoreInst>(V)) {
     return recoverStoreMD(V, name, var, DT);
  }
  else if (isa<GetElementPtrInst>(V)) {
    if (rn->getOriginalName(V) != std::string())
      return rn->getOriginalName(V);
    return recoverGEPMD(V, name, var, DT);
  }
}

// Return the correct string to PtrToInt Instruction
std::string RecoverPointerMD::getPtrToIntExpMD (PtrToIntInst *I,
                                       std::string name, int *var,
                                       const DataLayout *DT) {
  std::string expression = std::string();
  int op1,op2;

  expression += getAccessStringMD(&(*I->getOperand(0)), name, &op1, DT);
  if (op1 == -1)
    return expression;

  expression += "(long long int) " + NAME + "[" + std::to_string(op1) + "]"; 
  expression += ";\n";
  insertCommand(var, expression);
  return std::string();
}

// Return the correct string to IntToPtr Instruction
std::string RecoverPointerMD::getIntToPtrExpMD (IntToPtrInst *I,
                                       std::string name, int *var,
                                       const DataLayout *DT) {
  std::string expression = std::string();
  int op1,op2;

  expression += getAccessStringMD(&(*I->getOperand(0)), name, &op1, DT);
  
  if (op1 == -1)
    return expression;
 
  expression += "(long long int) " + NAME + "[" + std::to_string(op1) + "]";
   
  expression += ";\n";
  insertCommand(var, expression);
  return std::string();
}

/// Return the result of Analysis for SextExp Instruction:
std::string RecoverPointerMD::getSextExpMD (SExtInst *I, std::string name,
                                          int *var, const DataLayout *DT) {
  return getAccessStringMD(I->getOperand(0),name,var, DT);
}

std::string RecoverPointerMD::getZextExpMD (ZExtInst *I, std::string name,  
                                              int *var, const DataLayout *DT) {                    
  return getAccessStringMD(I->getOperand(0),name,var, DT);
} 

// This method decide how the value V will be treated.
std::string RecoverPointerMD::getAccessStringMD (Value *V, std::string ptrName,
                                                  int *var,
                                                  const DataLayout *DT) {
  if (!isValid()) {
    return std::string();
  }
  // Default Value is "-1", to identify empty values in pass.
  *var = -1;
  std::string name = std::string();
  
  // Return the compute value, if exists.
  if (selectComputedValue(V, var, &name)) {
    return name;
  }

  if (rn->getOriginalName(V) != std::string())
    return rn->getOriginalName(V);
 
  // Return to PHINode it's name, if is knowed.
  name = getPHINode(V, ptrName, var, DT);
  if (name != std::string()) {
    insertComputedValue(V, var, name);
    return name;
  }

  //To memory access instruction, return the name.
  name = recoverNameOf(V, ptrName, var, DT);

  if (name != std::string()) {
    insertComputedValue(V, var, name);
    return name;
  }

  if (Constant *C = dyn_cast<Constant>(V)) {
    ConstantsSimplify CS;
    long long int value = CS.getUniqueConstantInteger(C, getPointer() ,DT);
    if (CS.isValid()) {
      insertComputedValue(V, var, std::to_string(value));
      return std::to_string(value);
    }
    setValidFalse();
    return "0";
  }

  // Return a empty string if the value isn't a Instruction.
  if (!isa<Instruction>(V) || isa<PHINode>(V)) {
    setValidFalse();
    return std::string();
  }

  if (CallInst* CI = dyn_cast<CallInst>(V)) {
    if (!isMallocCall(CI)) {
      setValidFalse();
      return std::string();
    }
    RecoverNames::VarNames nameF = rn->getNameofValue(V);

    if (nameF.nameInFile == std::string())
      setValidFalse();
    if (nameF.nameInFile != ptrName) {
      insertComputedValue(V, var, nameF.nameInFile);
      return nameF.nameInFile;
    }
    insertComputedValue(V, var, "0");
    return "0";
  } 

  Instruction *I = cast<Instruction>(V);
  if (!I || isa<ICmpInst>(I))
    return std::string(); 

  if (ZExtInst *ZInst = dyn_cast<ZExtInst>(I)) {
    std::string result = getZextExpMD(ZInst, ptrName, var, DT);
    insertComputedValue(V, var, result);
    return result;
  }

  if (SExtInst *SInst = dyn_cast<SExtInst>(I)) {
    std::string result = getSextExpMD(SInst, ptrName, var, DT);
    insertComputedValue(V, var, result);
    return result;
  }

  if (BitCastInst *BIT = dyn_cast<BitCastInst>(V)) {
    std::string result =  getAccessString(BIT->getOperand(0), ptrName, var,DT);
    insertComputedValue(V, var, result);
    return result;
  }

  if (SelectInst *SI = dyn_cast<SelectInst>(I)) {
    std::string result =  getSelExp(SI, ptrName, var, DT);
    insertComputedValue(V, var, result);
    return result;
  }

  if (PtrToIntInst *PtrInst = dyn_cast<PtrToIntInst>(I)) {
    std::string result =  getPtrToIntExpMD(PtrInst, ptrName, var, DT);
    insertComputedValue(V, var, result);
    return result;
  }

  if (IntToPtrInst *PtrInst = dyn_cast<IntToPtrInst>(I)) {
    std::string result =  getIntToPtrExpMD(PtrInst, ptrName, var, DT);
    insertComputedValue(V, var, result);
    return result;
  }

  std::string result = getGenericExp(I, ptrName,var, DT);
  insertComputedValue(V, var, result);
  return result;
}

//===---------------------------- RecoverCode.cpp -------------------------===//
