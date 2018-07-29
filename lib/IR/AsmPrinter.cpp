//===- AsmPrinter.cpp - MLIR Assembly Printer Implementation --------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements the MLIR AsmPrinter class, which is used to implement
// the various print() methods on the core IR objects.
//
//===----------------------------------------------------------------------===//

#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/CFGFunction.h"
#include "mlir/IR/MLFunction.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/OperationSet.h"
#include "mlir/IR/Statements.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/IR/Types.h"
#include "mlir/Support/STLExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/raw_ostream.h"
using namespace mlir;

void Identifier::print(raw_ostream &os) const { os << str(); }

void Identifier::dump() const { print(llvm::errs()); }

OpAsmPrinter::~OpAsmPrinter() {}

//===----------------------------------------------------------------------===//
// ModuleState
//===----------------------------------------------------------------------===//

namespace {
class ModuleState {
public:
  /// This is the operation set for the current context if it is knowable (a
  /// context could be determined), otherwise this is null.
  OperationSet *const operationSet;

  explicit ModuleState(MLIRContext *context)
      : operationSet(context ? &OperationSet::get(context) : nullptr) {}

  // Initializes module state, populating affine map state.
  void initialize(const Module *module);

  int getAffineMapId(const AffineMap *affineMap) const {
    auto it = affineMapIds.find(affineMap);
    if (it == affineMapIds.end()) {
      return -1;
    }
    return it->second;
  }

  ArrayRef<const AffineMap *> getAffineMapIds() const { return affineMapsById; }

private:
  void recordAffineMapReference(const AffineMap *affineMap) {
    if (affineMapIds.count(affineMap) == 0) {
      affineMapIds[affineMap] = affineMapsById.size();
      affineMapsById.push_back(affineMap);
    }
  }

  // Visit functions.
  void visitFunction(const Function *fn);
  void visitExtFunction(const ExtFunction *fn);
  void visitCFGFunction(const CFGFunction *fn);
  void visitMLFunction(const MLFunction *fn);
  void visitType(const Type *type);
  void visitAttribute(const Attribute *attr);
  void visitOperation(const Operation *op);

  DenseMap<const AffineMap *, int> affineMapIds;
  std::vector<const AffineMap *> affineMapsById;
};
} // end anonymous namespace

// TODO Support visiting other types/instructions when implemented.
void ModuleState::visitType(const Type *type) {
  if (auto *funcType = dyn_cast<FunctionType>(type)) {
    // Visit input and result types for functions.
    for (auto *input : funcType->getInputs())
      visitType(input);
    for (auto *result : funcType->getResults())
      visitType(result);
  } else if (auto *memref = dyn_cast<MemRefType>(type)) {
    // Visit affine maps in memref type.
    for (auto *map : memref->getAffineMaps()) {
      recordAffineMapReference(map);
    }
  }
}

void ModuleState::visitAttribute(const Attribute *attr) {
  if (auto *mapAttr = dyn_cast<AffineMapAttr>(attr)) {
    recordAffineMapReference(mapAttr->getValue());
  } else if (auto *array = dyn_cast<ArrayAttr>(attr)) {
    for (auto elt : array->getValue()) {
      visitAttribute(elt);
    }
  }
}

void ModuleState::visitOperation(const Operation *op) {
  for (auto elt : op->getAttrs()) {
    visitAttribute(elt.second);
  }
}

void ModuleState::visitExtFunction(const ExtFunction *fn) {
  visitType(fn->getType());
}

void ModuleState::visitCFGFunction(const CFGFunction *fn) {
  visitType(fn->getType());
  for (auto &block : *fn) {
    for (auto &op : block.getOperations()) {
      visitOperation(&op);
    }
  }
}

void ModuleState::visitMLFunction(const MLFunction *fn) {
  visitType(fn->getType());
  // TODO Visit function body statements (and attributes if required).
}

void ModuleState::visitFunction(const Function *fn) {
  switch (fn->getKind()) {
  case Function::Kind::ExtFunc:
    return visitExtFunction(cast<ExtFunction>(fn));
  case Function::Kind::CFGFunc:
    return visitCFGFunction(cast<CFGFunction>(fn));
  case Function::Kind::MLFunc:
    return visitMLFunction(cast<MLFunction>(fn));
  }
}

// Initializes module state, populating affine map state.
void ModuleState::initialize(const Module *module) {
  for (auto &fn : *module) {
    visitFunction(&fn);
  }
}

//===----------------------------------------------------------------------===//
// ModulePrinter
//===----------------------------------------------------------------------===//

namespace {
class ModulePrinter {
public:
  ModulePrinter(raw_ostream &os, ModuleState &state) : os(os), state(state) {}
  explicit ModulePrinter(const ModulePrinter &printer)
      : os(printer.os), state(printer.state) {}

  template <typename Container, typename UnaryFunctor>
  inline void interleaveComma(const Container &c, UnaryFunctor each_fn) const {
    interleave(c.begin(), c.end(), each_fn, [&]() { os << ", "; });
  }

  void print(const Module *module);
  void printAttribute(const Attribute *attr);
  void printType(const Type *type);
  void print(const Function *fn);
  void print(const ExtFunction *fn);
  void print(const CFGFunction *fn);
  void print(const MLFunction *fn);

  void printAffineMap(const AffineMap *map);
  void printAffineExpr(const AffineExpr *expr);

protected:
  raw_ostream &os;
  ModuleState &state;

  void printFunctionSignature(const Function *fn);
  void printAffineMapId(int affineMapId) const;
  void printAffineMapReference(const AffineMap *affineMap);

  void printAffineBinaryOpExpr(const AffineBinaryOpExpr *expr);
};
} // end anonymous namespace

// Prints function with initialized module state.
void ModulePrinter::print(const Function *fn) {
  switch (fn->getKind()) {
  case Function::Kind::ExtFunc:
    return print(cast<ExtFunction>(fn));
  case Function::Kind::CFGFunc:
    return print(cast<CFGFunction>(fn));
  case Function::Kind::MLFunc:
    return print(cast<MLFunction>(fn));
  }
}

// Prints affine map identifier.
void ModulePrinter::printAffineMapId(int affineMapId) const {
  os << "#map" << affineMapId;
}

void ModulePrinter::printAffineMapReference(const AffineMap *affineMap) {
  int mapId = state.getAffineMapId(affineMap);
  if (mapId >= 0) {
    // Map will be printed at top of module so print reference to its id.
    printAffineMapId(mapId);
  } else {
    // Map not in module state so print inline.
    affineMap->print(os);
  }
}

void ModulePrinter::print(const Module *module) {
  for (const auto &map : state.getAffineMapIds()) {
    printAffineMapId(state.getAffineMapId(map));
    os << " = ";
    map->print(os);
    os << '\n';
  }
  for (auto const &fn : *module)
    print(&fn);
}

void ModulePrinter::printAttribute(const Attribute *attr) {
  switch (attr->getKind()) {
  case Attribute::Kind::Bool:
    os << (cast<BoolAttr>(attr)->getValue() ? "true" : "false");
    break;
  case Attribute::Kind::Integer:
    os << cast<IntegerAttr>(attr)->getValue();
    break;
  case Attribute::Kind::Float:
    // FIXME: this isn't precise, we should print with a hex format.
    os << cast<FloatAttr>(attr)->getValue();
    break;
  case Attribute::Kind::String:
    // FIXME: should escape the string.
    os << '"' << cast<StringAttr>(attr)->getValue() << '"';
    break;
  case Attribute::Kind::Array: {
    auto elts = cast<ArrayAttr>(attr)->getValue();
    os << '[';
    interleaveComma(elts, [&](Attribute *attr) { printAttribute(attr); });
    os << ']';
    break;
  }
  case Attribute::Kind::AffineMap:
    printAffineMapReference(cast<AffineMapAttr>(attr)->getValue());
    break;
  }
}

void ModulePrinter::printType(const Type *type) {
  switch (type->getKind()) {
  case Type::Kind::AffineInt:
    os << "affineint";
    return;
  case Type::Kind::BF16:
    os << "bf16";
    return;
  case Type::Kind::F16:
    os << "f16";
    return;
  case Type::Kind::F32:
    os << "f32";
    return;
  case Type::Kind::F64:
    os << "f64";
    return;
  case Type::Kind::TFControl:
    os << "tf_control";
    return;

  case Type::Kind::Integer: {
    auto *integer = cast<IntegerType>(type);
    os << 'i' << integer->getWidth();
    return;
  }
  case Type::Kind::Function: {
    auto *func = cast<FunctionType>(type);
    os << '(';
    interleaveComma(func->getInputs(), [&](Type *type) { printType(type); });
    os << ") -> ";
    auto results = func->getResults();
    if (results.size() == 1)
      os << *results[0];
    else {
      os << '(';
      interleaveComma(results, [&](Type *type) { printType(type); });
      os << ')';
    }
    return;
  }
  case Type::Kind::Vector: {
    auto *v = cast<VectorType>(type);
    os << "vector<";
    for (auto dim : v->getShape())
      os << dim << 'x';
    os << *v->getElementType() << '>';
    return;
  }
  case Type::Kind::RankedTensor: {
    auto *v = cast<RankedTensorType>(type);
    os << "tensor<";
    for (auto dim : v->getShape()) {
      if (dim < 0)
        os << '?';
      else
        os << dim;
      os << 'x';
    }
    os << *v->getElementType() << '>';
    return;
  }
  case Type::Kind::UnrankedTensor: {
    auto *v = cast<UnrankedTensorType>(type);
    os << "tensor<??";
    printType(v->getElementType());
    os << '>';
    return;
  }
  case Type::Kind::MemRef: {
    auto *v = cast<MemRefType>(type);
    os << "memref<";
    for (auto dim : v->getShape()) {
      if (dim < 0)
        os << '?';
      else
        os << dim;
      os << 'x';
    }
    printType(v->getElementType());
    for (auto map : v->getAffineMaps()) {
      os << ", ";
      printAffineMapReference(map);
    }
    // Only print the memory space if it is the non-default one.
    if (v->getMemorySpace())
      os << ", " << v->getMemorySpace();
    os << '>';
    return;
  }
  }
}

//===----------------------------------------------------------------------===//
// Affine expressions and maps
//===----------------------------------------------------------------------===//

void ModulePrinter::printAffineExpr(const AffineExpr *expr) {
  switch (expr->getKind()) {
  case AffineExpr::Kind::SymbolId:
    os << 's' << cast<AffineSymbolExpr>(expr)->getPosition();
    return;
  case AffineExpr::Kind::DimId:
    os << 'd' << cast<AffineDimExpr>(expr)->getPosition();
    return;
  case AffineExpr::Kind::Constant:
    os << cast<AffineConstantExpr>(expr)->getValue();
    return;
  case AffineExpr::Kind::Add:
  case AffineExpr::Kind::Mul:
  case AffineExpr::Kind::FloorDiv:
  case AffineExpr::Kind::CeilDiv:
  case AffineExpr::Kind::Mod:
    return printAffineBinaryOpExpr(cast<AffineBinaryOpExpr>(expr));
  }
}

void ModulePrinter::printAffineBinaryOpExpr(const AffineBinaryOpExpr *expr) {
  if (expr->getKind() != AffineExpr::Kind::Add) {
    os << '(';
    printAffineExpr(expr->getLHS());
    switch (expr->getKind()) {
    case AffineExpr::Kind::Mul:
      os << " * ";
      break;
    case AffineExpr::Kind::FloorDiv:
      os << " floordiv ";
      break;
    case AffineExpr::Kind::CeilDiv:
      os << " ceildiv ";
      break;
    case AffineExpr::Kind::Mod:
      os << " mod ";
      break;
    default:
      llvm_unreachable("unexpected affine binary op expression");
    }

    printAffineExpr(expr->getRHS());
    os << ')';
    return;
  }

  // Print out special "pretty" forms for add.
  os << '(';
  printAffineExpr(expr->getLHS());

  // Pretty print addition to a product that has a negative operand as a
  // subtraction.
  if (auto *rhs = dyn_cast<AffineBinaryOpExpr>(expr->getRHS())) {
    if (rhs->getKind() == AffineExpr::Kind::Mul) {
      if (auto *rrhs = dyn_cast<AffineConstantExpr>(rhs->getRHS())) {
        if (rrhs->getValue() < 0) {
          os << " - (";
          printAffineExpr(rhs->getLHS());
          os << " * " << -rrhs->getValue() << "))";
          return;
        }
      }
    }
  }

  // Pretty print addition to a negative number as a subtraction.
  if (auto *rhs = dyn_cast<AffineConstantExpr>(expr->getRHS())) {
    if (rhs->getValue() < 0) {
      os << " - " << -rhs->getValue() << ")";
      return;
    }
  }

  os << " + ";
  printAffineExpr(expr->getRHS());
  os << ')';
}

void ModulePrinter::printAffineMap(const AffineMap *map) {
  // Dimension identifiers.
  os << '(';
  for (int i = 0; i < (int)map->getNumDims() - 1; ++i)
    os << 'd' << i << ", ";
  if (map->getNumDims() >= 1)
    os << 'd' << map->getNumDims() - 1;
  os << ')';

  // Symbolic identifiers.
  if (map->getNumSymbols() != 0) {
    os << '[';
    for (unsigned i = 0; i < map->getNumSymbols() - 1; ++i)
      os << 's' << i << ", ";
    if (map->getNumSymbols() >= 1)
      os << 's' << map->getNumSymbols() - 1;
    os << ']';
  }

  // AffineMap should have at least one result.
  assert(!map->getResults().empty());
  // Result affine expressions.
  os << " -> (";
  interleaveComma(map->getResults(),
                  [&](AffineExpr *expr) { printAffineExpr(expr); });
  os << ')';

  if (!map->isBounded()) {
    return;
  }

  // Print range sizes for bounded affine maps.
  os << " size (";
  interleaveComma(map->getRangeSizes(),
                  [&](AffineExpr *expr) { printAffineExpr(expr); });
  os << ')';
}

//===----------------------------------------------------------------------===//
// Function printing
//===----------------------------------------------------------------------===//

void ModulePrinter::printFunctionSignature(const Function *fn) {
  auto type = fn->getType();

  os << "@" << fn->getName() << '(';
  interleaveComma(type->getInputs(),
                  [&](Type *eltType) { printType(eltType); });
  os << ')';

  switch (type->getResults().size()) {
  case 0:
    break;
  case 1:
    os << " -> ";
    printType(type->getResults()[0]);
    break;
  default:
    os << " -> (";
    interleaveComma(type->getResults(),
                    [&](Type *eltType) { printType(eltType); });
    os << ')';
    break;
  }
}

void ModulePrinter::print(const ExtFunction *fn) {
  os << "extfunc ";
  printFunctionSignature(fn);
  os << '\n';
}

namespace {

// FunctionPrinter contains common functionality for printing
// CFG and ML functions.
class FunctionPrinter : public ModulePrinter, private OpAsmPrinter {
public:
  FunctionPrinter(const ModulePrinter &other) : ModulePrinter(other) {}

  void printOperation(const Operation *op);
  void printDefaultOp(const Operation *op);

  // Implement OpAsmPrinter.
  raw_ostream &getStream() const { return os; }
  void printType(const Type *type) { ModulePrinter::printType(type); }
  void printAttribute(const Attribute *attr) {
    ModulePrinter::printAttribute(attr);
  }
  void printAffineMap(const AffineMap *map) {
    return ModulePrinter::printAffineMapReference(map);
  }
  void printAffineExpr(const AffineExpr *expr) {
    return ModulePrinter::printAffineExpr(expr);
  }

  void printOperand(const SSAValue *value) { printValueID(value); }

protected:
  void numberValueID(const SSAValue *value) {
    assert(!valueIDs.count(value) && "Value numbered multiple times");
    valueIDs[value] = nextValueID++;
  }

  void printValueID(const SSAValue *value,
                    bool dontPrintResultNo = false) const {
    int resultNo = -1;
    auto lookupValue = value;

    // If this is a reference to the result of a multi-result instruction, print
    // out the # identifier and make sure to map our lookup to the first result
    // of the instruction.
    if (auto *result = dyn_cast<InstResult>(value)) {
      if (result->getOwner()->getNumResults() != 1) {
        resultNo = result->getResultNumber();
        lookupValue = result->getOwner()->getResult(0);
      }
    }

    auto it = valueIDs.find(lookupValue);
    if (it == valueIDs.end()) {
      os << "<<INVALID SSA VALUE>>";
      return;
    }

    os << '%' << it->getSecond();
    if (resultNo != -1 && !dontPrintResultNo)
      os << '#' << resultNo;
  }

private:
  /// This is the value ID for each SSA value in the current function.
  DenseMap<const SSAValue *, unsigned> valueIDs;
  unsigned nextValueID = 0;
};
} // end anonymous namespace

void FunctionPrinter::printOperation(const Operation *op) {
  if (op->getNumResults()) {
    printValueID(op->getResult(0), /*dontPrintResultNo*/ true);
    os << " = ";
  }

  // Check to see if this is a known operation.  If so, use the registered
  // custom printer hook.
  if (auto opInfo = state.operationSet->lookup(op->getName().str())) {
    opInfo->printAssembly(op, this);
    return;
  }

  // Otherwise use the standard verbose printing approach.
  printDefaultOp(op);
}

void FunctionPrinter::printDefaultOp(const Operation *op) {
  // TODO: escape name if necessary.
  os << "\"" << op->getName().str() << "\"(";

  interleaveComma(op->getOperands(),
                  [&](const SSAValue *value) { printValueID(value); });

  os << ')';
  auto attrs = op->getAttrs();
  if (!attrs.empty()) {
    os << '{';
    interleaveComma(attrs, [&](NamedAttribute attr) {
      os << attr.first << ": ";
      printAttribute(attr.second);
    });
    os << '}';
  }

  // Print the type signature of the operation.
  os << " : (";
  interleaveComma(op->getOperands(),
                  [&](const SSAValue *value) { printType(value->getType()); });
  os << ") -> ";

  if (op->getNumResults() == 1) {
    printType(op->getResult(0)->getType());
  } else {
    os << '(';
    interleaveComma(op->getResults(), [&](const SSAValue *result) {
      printType(result->getType());
    });
    os << ')';
  }
}

//===----------------------------------------------------------------------===//
// CFG Function printing
//===----------------------------------------------------------------------===//

namespace {
class CFGFunctionPrinter : public FunctionPrinter {
public:
  CFGFunctionPrinter(const CFGFunction *function, const ModulePrinter &other);

  const CFGFunction *getFunction() const { return function; }

  void print();
  void print(const BasicBlock *block);

  void print(const Instruction *inst);
  void print(const OperationInst *inst);
  void print(const ReturnInst *inst);
  void print(const BranchInst *inst);
  void print(const CondBranchInst *inst);

  unsigned getBBID(const BasicBlock *block) {
    auto it = basicBlockIDs.find(block);
    assert(it != basicBlockIDs.end() && "Block not in this function?");
    return it->second;
  }

private:
  const CFGFunction *function;
  DenseMap<const BasicBlock *, unsigned> basicBlockIDs;

  void numberValuesInBlock(const BasicBlock *block);
};
} // end anonymous namespace

CFGFunctionPrinter::CFGFunctionPrinter(const CFGFunction *function,
                                       const ModulePrinter &other)
    : FunctionPrinter(other), function(function) {
  // Each basic block gets a unique ID per function.
  unsigned blockID = 0;
  for (auto &block : *function) {
    basicBlockIDs[&block] = blockID++;
    numberValuesInBlock(&block);
  }
}

/// Number all of the SSA values in the specified basic block.
void CFGFunctionPrinter::numberValuesInBlock(const BasicBlock *block) {
  for (auto *arg : block->getArguments()) {
    numberValueID(arg);
  }
  for (auto &op : *block) {
    // We number instruction that have results, and we only number the first
    // result.
    if (op.getNumResults() != 0)
      numberValueID(op.getResult(0));
  }

  // Terminators do not define values.
}

void CFGFunctionPrinter::print() {
  os << "cfgfunc ";
  printFunctionSignature(getFunction());
  os << " {\n";

  for (auto &block : *function)
    print(&block);
  os << "}\n\n";
}

void CFGFunctionPrinter::print(const BasicBlock *block) {
  os << "bb" << getBBID(block);

  if (!block->args_empty()) {
    os << '(';
    interleaveComma(block->getArguments(), [&](const BBArgument *arg) {
      printValueID(arg);
      os << ": ";
      printType(arg->getType());
    });
    os << ')';
  }
  os << ':';

  // Print out some context information about the predecessors of this block.
  if (!block->getFunction()) {
    os << "\t// block is not in a function!";
  } else if (block->hasNoPredecessors()) {
    // Don't print "no predecessors" for the entry block.
    if (block != &block->getFunction()->front())
      os << "\t// no predecessors";
  } else if (auto *pred = block->getSinglePredecessor()) {
    os << "\t// pred: bb" << getBBID(pred);
  } else {
    // We want to print the predecessors in increasing numeric order, not in
    // whatever order the use-list is in, so gather and sort them.
    SmallVector<unsigned, 4> predIDs;
    for (auto *pred : block->getPredecessors())
      predIDs.push_back(getBBID(pred));
    llvm::array_pod_sort(predIDs.begin(), predIDs.end());

    os << "\t// " << predIDs.size() << " preds: ";

    interleaveComma(predIDs, [&](unsigned predID) { os << "bb" << predID; });
  }
  os << '\n';

  for (auto &inst : block->getOperations()) {
    os << "  ";
    print(&inst);
    os << '\n';
  }

  print(block->getTerminator());
  os << '\n';
}

void CFGFunctionPrinter::print(const Instruction *inst) {
  switch (inst->getKind()) {
  case Instruction::Kind::Operation:
    return print(cast<OperationInst>(inst));
  case TerminatorInst::Kind::Branch:
    return print(cast<BranchInst>(inst));
  case TerminatorInst::Kind::CondBranch:
    return print(cast<CondBranchInst>(inst));
  case TerminatorInst::Kind::Return:
    return print(cast<ReturnInst>(inst));
  }
}

void CFGFunctionPrinter::print(const OperationInst *inst) {
  printOperation(inst);
}

void CFGFunctionPrinter::print(const BranchInst *inst) {
  os << "br bb" << getBBID(inst->getDest());

  if (inst->getNumOperands() != 0) {
    os << '(';
    // TODO: Use getOperands() when we have it.
    interleaveComma(inst->getInstOperands(), [&](const InstOperand &operand) {
      printValueID(operand.get());
    });
    os << ") : ";
    interleaveComma(inst->getInstOperands(), [&](const InstOperand &operand) {
      printType(operand.get()->getType());
    });
  }
}

void CFGFunctionPrinter::print(const CondBranchInst *inst) {
  os << "cond_br ";
  printValueID(inst->getCondition());

  os << ", bb" << getBBID(inst->getTrueDest());
  if (inst->getNumTrueOperands() != 0) {
    os << '(';
    interleaveComma(inst->getTrueOperands(),
                    [&](const CFGValue *operand) { printValueID(operand); });
    os << " : ";
    interleaveComma(inst->getTrueOperands(), [&](const CFGValue *operand) {
      printType(operand->getType());
    });
    os << ")";
  }

  os << ", bb" << getBBID(inst->getFalseDest());
  if (inst->getNumFalseOperands() != 0) {
    os << '(';
    interleaveComma(inst->getFalseOperands(),
                    [&](const CFGValue *operand) { printValueID(operand); });
    os << " : ";
    interleaveComma(inst->getFalseOperands(), [&](const CFGValue *operand) {
      printType(operand->getType());
    });
    os << ")";
  }
}

void CFGFunctionPrinter::print(const ReturnInst *inst) {
  os << "return";

  if (inst->getNumOperands() != 0)
    os << ' ';

  interleaveComma(inst->getOperands(),
                  [&](const CFGValue *operand) { printValueID(operand); });
  os << " : ";
  interleaveComma(inst->getOperands(), [&](const CFGValue *operand) {
    printType(operand->getType());
  });
}

void ModulePrinter::print(const CFGFunction *fn) {
  CFGFunctionPrinter(fn, *this).print();
}

//===----------------------------------------------------------------------===//
// ML Function printing
//===----------------------------------------------------------------------===//

namespace {
class MLFunctionPrinter : public FunctionPrinter {
public:
  MLFunctionPrinter(const MLFunction *function, const ModulePrinter &other);

  const MLFunction *getFunction() const { return function; }

  // Prints ML function
  void print();

  // Methods to print ML function statements
  void print(const Statement *stmt);
  void print(const OperationStmt *stmt);
  void print(const ForStmt *stmt);
  void print(const IfStmt *stmt);
  void print(const StmtBlock *block);

  // Number of spaces used for indenting nested statements
  const static unsigned indentWidth = 2;

private:
  void numberValues();

  const MLFunction *function;
  int numSpaces;
};
} // end anonymous namespace

MLFunctionPrinter::MLFunctionPrinter(const MLFunction *function,
                                     const ModulePrinter &other)
    : FunctionPrinter(other), function(function), numSpaces(0) {
  numberValues();
}

/// Number all of the SSA values in this ML function.
void MLFunctionPrinter::numberValues() {
  // Visits all operation statements and numbers the first result.
  struct NumberValuesPass : public StmtWalker<NumberValuesPass> {
    NumberValuesPass(MLFunctionPrinter *printer) : printer(printer) {}
    void visitOperationStmt(OperationStmt *stmt) {
      if (stmt->getNumResults() != 0)
        printer->numberValueID(stmt->getResult(0));
    }
    MLFunctionPrinter *printer;
  };

  NumberValuesPass pass(this);
  // TODO: it'd be cleaner to have constant visitor istead of using const_cast.
  pass.walk(const_cast<MLFunction *>(function));
}

void MLFunctionPrinter::print() {
  os << "mlfunc ";
  // FIXME: should print argument names rather than just signature
  printFunctionSignature(function);
  os << " {\n";
  print(function);
  os << "  return\n";
  os << "}\n\n";
}

void MLFunctionPrinter::print(const StmtBlock *block) {
  numSpaces += indentWidth;
  for (auto &stmt : block->getStatements()) {
    print(&stmt);
    os << "\n";
  }
  numSpaces -= indentWidth;
}

void MLFunctionPrinter::print(const Statement *stmt) {
  switch (stmt->getKind()) {
  case Statement::Kind::Operation:
    return print(cast<OperationStmt>(stmt));
  case Statement::Kind::For:
    return print(cast<ForStmt>(stmt));
  case Statement::Kind::If:
    return print(cast<IfStmt>(stmt));
  }
}

void MLFunctionPrinter::print(const OperationStmt *stmt) {
  os.indent(numSpaces);
  printOperation(stmt);
}

void MLFunctionPrinter::print(const ForStmt *stmt) {
  os.indent(numSpaces) << "for x = " << *stmt->getLowerBound();
  os << " to " << *stmt->getUpperBound();
  if (stmt->getStep()->getValue() != 1)
    os << " step " << *stmt->getStep();

  os << " {\n";
  print(static_cast<const StmtBlock *>(stmt));
  os.indent(numSpaces) << "}";
}

void MLFunctionPrinter::print(const IfStmt *stmt) {
  os.indent(numSpaces) << "if () {\n";
  print(stmt->getThenClause());
  os.indent(numSpaces) << "}";
  if (stmt->hasElseClause()) {
    os << " else {\n";
    print(stmt->getElseClause());
    os.indent(numSpaces) << "}";
  }
}

void ModulePrinter::print(const MLFunction *fn) {
  MLFunctionPrinter(fn, *this).print();
}

//===----------------------------------------------------------------------===//
// print and dump methods
//===----------------------------------------------------------------------===//

void Attribute::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).printAttribute(this);
}

void Attribute::dump() const { print(llvm::errs()); }

void Type::print(raw_ostream &os) const {
  ModuleState state(getContext());
  ModulePrinter(os, state).printType(this);
}

void Type::dump() const { print(llvm::errs()); }

void AffineMap::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void AffineExpr::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void AffineExpr::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).printAffineExpr(this);
}

void AffineMap::print(raw_ostream &os) const {
  ModuleState state(/*no context is known*/ nullptr);
  ModulePrinter(os, state).printAffineMap(this);
}

void Instruction::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  CFGFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void Instruction::dump() const {
  print(llvm::errs());
  llvm::errs() << "\n";
}

void BasicBlock::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  CFGFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void BasicBlock::dump() const { print(llvm::errs()); }

void Statement::print(raw_ostream &os) const {
  ModuleState state(getFunction()->getContext());
  ModulePrinter modulePrinter(os, state);
  MLFunctionPrinter(getFunction(), modulePrinter).print(this);
}

void Statement::dump() const { print(llvm::errs()); }

void Function::print(raw_ostream &os) const {
  ModuleState state(getContext());
  ModulePrinter(os, state).print(this);
}

void Function::dump() const { print(llvm::errs()); }

void Module::print(raw_ostream &os) const {
  ModuleState state(getContext());
  state.initialize(this);
  ModulePrinter(os, state).print(this);
}

void Module::dump() const { print(llvm::errs()); }
