//===- Operator.cpp - Operator class --------------------------------------===//
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
// Operator wrapper to simplify using TableGen Record defining a MLIR Op.
//
//===----------------------------------------------------------------------===//

#include "mlir/TableGen/Operator.h"
#include "mlir/TableGen/OpTrait.h"
#include "mlir/TableGen/Predicate.h"
#include "mlir/TableGen/Type.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TableGen/Error.h"
#include "llvm/TableGen/Record.h"

using namespace mlir;

using llvm::DagInit;
using llvm::DefInit;
using llvm::Record;

tblgen::Operator::Operator(const llvm::Record &def) : def(def) {
  std::tie(dialectName, cppClassName) = def.getName().split('_');
  if (dialectName.empty()) {
    // Class name with a leading underscore and without dialect name
    cppClassName = def.getName();
  } else if (cppClassName.empty()) {
    // Class name without dialect name
    std::swap(dialectName, cppClassName);
  }

  populateOpStructure();
}

std::string tblgen::Operator::getOperationName() const {
  auto *dialect = def.getValueAsDef("opDialect");
  assert(dialect && "op defined without dialect");
  auto prefix = dialect->getValueAsString("name");
  if (prefix.empty())
    return def.getValueAsString("opName");
  return llvm::formatv("{0}.{1}", prefix, def.getValueAsString("opName"));
}

StringRef tblgen::Operator::getDialectName() const { return dialectName; }

StringRef tblgen::Operator::getCppClassName() const { return cppClassName; }

std::string tblgen::Operator::getQualCppClassName(StringRef name) {
  StringRef ns, cls;
  std::tie(ns, cls) = name.split('_');
  if (ns.empty() || cls.empty())
    return name;
  return (ns + "::" + cls).str();
}

std::string tblgen::Operator::getQualCppClassName() const {
  return getQualCppClassName(def.getName());
}

int tblgen::Operator::getNumResults() const {
  DagInit *results = def.getValueAsDag("results");
  return results->getNumArgs();
}

StringRef tblgen::Operator::getExtraClassDeclaration() const {
  constexpr auto attr = "extraClassDeclaration";
  if (def.isValueUnset(attr))
    return {};
  return def.getValueAsString(attr);
}

tblgen::TypeConstraint
tblgen::Operator::getResultTypeConstraint(int index) const {
  DagInit *results = def.getValueAsDag("results");
  return TypeConstraint(cast<DefInit>(results->getArg(index)));
}

StringRef tblgen::Operator::getResultName(int index) const {
  DagInit *results = def.getValueAsDag("results");
  return results->getArgNameStr(index);
}

unsigned tblgen::Operator::getNumVariadicResults() const {
  return std::count_if(
      results.begin(), results.end(),
      [](const NamedTypeConstraint &c) { return c.constraint.isVariadic(); });
}

int tblgen::Operator::getNumNativeAttributes() const {
  return numNativeAttributes;
}

int tblgen::Operator::getNumDerivedAttributes() const {
  return getNumAttributes() - getNumNativeAttributes();
}

const tblgen::NamedAttribute &tblgen::Operator::getAttribute(int index) const {
  return attributes[index];
}

unsigned tblgen::Operator::getNumVariadicOperands() const {
  return std::count_if(
      operands.begin(), operands.end(),
      [](const NamedTypeConstraint &c) { return c.constraint.isVariadic(); });
}

StringRef tblgen::Operator::getArgName(int index) const {
  DagInit *argumentValues = def.getValueAsDag("arguments");
  return argumentValues->getArgName(index)->getValue();
}

int tblgen::Operator::getNumPredOpTraits() const {
  return std::count_if(traits.begin(), traits.end(), [](const OpTrait &trait) {
    return isa<tblgen::PredOpTrait>(&trait);
  });
}

bool tblgen::Operator::hasTrait(StringRef trait) const {
  for (auto t : getTraits()) {
    if (auto opTrait = dyn_cast<tblgen::NativeOpTrait>(&t)) {
      if (opTrait->getTrait() == trait)
        return true;
    } else if (auto opTrait = dyn_cast<tblgen::InternalOpTrait>(&t)) {
      if (opTrait->getTrait() == trait)
        return true;
    }
  }
  return false;
}

auto tblgen::Operator::trait_begin() const -> const_trait_iterator {
  return traits.begin();
}
auto tblgen::Operator::trait_end() const -> const_trait_iterator {
  return traits.end();
}
auto tblgen::Operator::getTraits() const
    -> llvm::iterator_range<const_trait_iterator> {
  return {trait_begin(), trait_end()};
}

auto tblgen::Operator::attribute_begin() const -> attribute_iterator {
  return attributes.begin();
}
auto tblgen::Operator::attribute_end() const -> attribute_iterator {
  return attributes.end();
}
auto tblgen::Operator::getAttributes() const
    -> llvm::iterator_range<attribute_iterator> {
  return {attribute_begin(), attribute_end()};
}

auto tblgen::Operator::operand_begin() -> operand_iterator {
  return operands.begin();
}
auto tblgen::Operator::operand_end() -> operand_iterator {
  return operands.end();
}
auto tblgen::Operator::getOperands() -> llvm::iterator_range<operand_iterator> {
  return {operand_begin(), operand_end()};
}

auto tblgen::Operator::getArg(int index) -> Argument {
  return arguments[index];
}

void tblgen::Operator::populateOpStructure() {
  auto &recordKeeper = def.getRecords();
  auto typeConstraintClass = recordKeeper.getClass("TypeConstraint");
  auto attrClass = recordKeeper.getClass("Attr");
  auto derivedAttrClass = recordKeeper.getClass("DerivedAttr");
  numNativeAttributes = 0;

  // The argument ordering is operands, native attributes, derived
  // attributes.
  DagInit *argumentValues = def.getValueAsDag("arguments");
  unsigned i = 0;
  // Handle operands and native attributes.
  for (unsigned e = argumentValues->getNumArgs(); i != e; ++i) {
    auto arg = argumentValues->getArg(i);
    auto givenName = argumentValues->getArgNameStr(i);
    auto argDefInit = dyn_cast<DefInit>(arg);
    if (!argDefInit)
      PrintFatalError(def.getLoc(),
                      Twine("undefined type for argument #") + Twine(i));
    Record *argDef = argDefInit->getDef();

    if (argDef->isSubClassOf(typeConstraintClass)) {
      operands.push_back(
          NamedTypeConstraint{givenName, TypeConstraint(argDefInit)});
      arguments.emplace_back(&operands.back());
    } else if (argDef->isSubClassOf(attrClass)) {
      if (givenName.empty())
        PrintFatalError(argDef->getLoc(), "attributes must be named");
      if (argDef->isSubClassOf(derivedAttrClass))
        PrintFatalError(argDef->getLoc(),
                        "derived attributes not allowed in argument list");
      attributes.push_back({givenName, Attribute(argDef)});
      arguments.emplace_back(&attributes.back());
      ++numNativeAttributes;
    } else {
      PrintFatalError(def.getLoc(), "unexpected def type; only defs deriving "
                                    "from TypeConstraint or Attr are allowed");
    }
  }

  // Handle derived attributes.
  for (const auto &val : def.getValues()) {
    if (auto *record = dyn_cast<llvm::RecordRecTy>(val.getType())) {
      if (!record->isSubClassOf(attrClass))
        continue;
      if (!record->isSubClassOf(derivedAttrClass))
        PrintFatalError(def.getLoc(),
                        "unexpected Attr where only DerivedAttr is allowed");

      if (record->getClasses().size() != 1) {
        PrintFatalError(
            def.getLoc(),
            "unsupported attribute modelling, only single class expected");
      }
      attributes.push_back(
          {cast<llvm::StringInit>(val.getNameInit())->getValue(),
           Attribute(cast<DefInit>(val.getValue()))});
    }
  }

  auto *resultsDag = def.getValueAsDag("results");
  auto *outsOp = dyn_cast<DefInit>(resultsDag->getOperator());
  if (!outsOp || outsOp->getDef()->getName() != "outs") {
    PrintFatalError(def.getLoc(), "'results' must have 'outs' directive");
  }

  // Handle results.
  for (unsigned i = 0, e = resultsDag->getNumArgs(); i < e; ++i) {
    auto name = resultsDag->getArgNameStr(i);
    auto *resultDef = dyn_cast<DefInit>(resultsDag->getArg(i));
    if (!resultDef) {
      PrintFatalError(def.getLoc(),
                      Twine("undefined type for result #") + Twine(i));
    }
    results.push_back({name, TypeConstraint(resultDef)});
  }

  auto traitListInit = def.getValueAsListInit("traits");
  if (!traitListInit)
    return;
  traits.reserve(traitListInit->size());
  for (auto traitInit : *traitListInit)
    traits.push_back(OpTrait::create(traitInit));
}

ArrayRef<llvm::SMLoc> tblgen::Operator::getLoc() const { return def.getLoc(); }

bool tblgen::Operator::hasDescription() const {
  return def.getValue("description") != nullptr;
}

StringRef tblgen::Operator::getDescription() const {
  return def.getValueAsString("description");
}

bool tblgen::Operator::hasSummary() const {
  return def.getValue("summary") != nullptr;
}

StringRef tblgen::Operator::getSummary() const {
  return def.getValueAsString("summary");
}
