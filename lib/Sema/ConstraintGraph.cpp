//===--- ConstraintGraph.cpp - Constraint Graph ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements the \c ConstraintGraph class, which describes the
// relationships among the type variables within a constraint system.
//
//===----------------------------------------------------------------------===//
#include "ConstraintGraph.h"
#include "ConstraintSystem.h"
#include "swift/Basic/Fallthrough.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <memory>
#include <numeric>

using namespace swift;
using namespace constraints;

#pragma mark Graph construction/destruction

ConstraintGraph::ConstraintGraph(ConstraintSystem &cs) : CS(cs) { }

ConstraintGraph::~ConstraintGraph() {
  for (auto node : Nodes) {
    delete node.second.NodePtr;
  }
}

#pragma mark Helper functions

/// Recursively gather the set of type variables referenced by this constraint.
static void
gatherReferencedTypeVarsRec(ConstraintSystem &cs,
                            Constraint *constraint,
                            SmallVectorImpl<TypeVariableType *> &typeVars) {
  switch (constraint->getKind()) {
  case ConstraintKind::Conjunction:
  case ConstraintKind::Disjunction:
    for (auto nested : constraint->getNestedConstraints())
      gatherReferencedTypeVarsRec(cs, nested, typeVars);
    return;

  case ConstraintKind::ApplicableFunction:
  case ConstraintKind::Bind:
  case ConstraintKind::Construction:
  case ConstraintKind::Conversion:
  case ConstraintKind::CheckedCast:
  case ConstraintKind::Equal:
  case ConstraintKind::Subtype:
  case ConstraintKind::TrivialSubtype:
  case ConstraintKind::TypeMember:
  case ConstraintKind::ValueMember:
    constraint->getSecondType()->getTypeVariables(typeVars);
    SWIFT_FALLTHROUGH;

  case ConstraintKind::Archetype:
  case ConstraintKind::BindOverload:
  case ConstraintKind::Class:
  case ConstraintKind::ConformsTo:
  case ConstraintKind::DynamicLookupValue:
  case ConstraintKind::SelfObjectOfProtocol:
    constraint->getFirstType()->getTypeVariables(typeVars);

    // Special case: the base type of an overloading binding.
    if (constraint->getKind() == ConstraintKind::BindOverload) {
      if (auto baseType = constraint->getOverloadChoice().getBaseType()) {
        baseType->getTypeVariables(typeVars);
      }
    }

    break;
  }
}

/// Gather and unique the set of type variables referenced by this constraint.
static void
gatherReferencedTypeVars(ConstraintSystem &cs,
                         Constraint *constraint,
                         SmallVectorImpl<TypeVariableType *> &typeVars) {
  // Gather all of the referenced type variables.
  gatherReferencedTypeVarsRec(cs, constraint, typeVars);

  // Remove any duplicate type variables.
  llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
  typeVars.erase(std::remove_if(typeVars.begin(), typeVars.end(),
                                [&](TypeVariableType *typeVar) {
                                  return !knownTypeVars.insert(typeVar);
                                }),
                 typeVars.end());
}

#pragma mark Graph accessors

std::pair<ConstraintGraph::Node &, unsigned>
ConstraintGraph::lookupNode(TypeVariableType *typeVar) {
  // Check whether we've already created a node for this type variable.
  auto known = Nodes.find(typeVar);
  if (known != Nodes.end()) {
    assert(known->second.NodePtr && "Missing node pointer?");
    return { *known->second.NodePtr, known->second.Index };
  }

  // Allocate the new node.
  StoredNode &stored = Nodes[typeVar];
  stored.NodePtr = new Node(typeVar);
  stored.Index = TypeVariables.size();

  // Record this type variable.
  TypeVariables.push_back(typeVar);

  // If this type variable is not the representative of its equivalence class,
  // add it to its representative's set of equivalences.
  auto typeVarRep = CS.getRepresentative(typeVar);
  if (typeVar != typeVarRep)
    (*this)[typeVarRep].addToEquivalenceClass(typeVar);
  else {
    // If this type variable has a fixed type binding that involves other
    // type variables, notify those type variables.
    if (auto fixed = CS.getFixedType(typeVarRep)) {
      if (fixed->hasTypeVariable()) {
        SmallVector<TypeVariableType *, 4> typeVars;
        llvm::SmallPtrSet<TypeVariableType *, 4> knownTypeVars;
        fixed->getTypeVariables(typeVars);
        for (auto otherTypeVar : typeVars) {
          if (knownTypeVars.insert(otherTypeVar)) {
            (*this)[otherTypeVar].addFixedBinding(typeVar);
            stored.NodePtr->addFixedBinding(otherTypeVar);
          }
        }
      }
    }
  }
  return { *stored.NodePtr, stored.Index };
}

ArrayRef<TypeVariableType *> ConstraintGraph::Node::getEquivalenceClass() const{
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't request equivalence class from non-representative type var");
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  return EquivalenceClass;
}

#pragma mark Node mutation
void ConstraintGraph::Node::addConstraint(Constraint *constraint) {
  assert(ConstraintIndex.count(constraint) == 0 && "Constraint re-insertion");
  ConstraintIndex[constraint] = Constraints.size();
  Constraints.push_back(constraint);
}

void ConstraintGraph::Node::removeConstraint(Constraint *constraint) {
  auto pos = ConstraintIndex.find(constraint);
  assert(pos != ConstraintIndex.end());

  // Remove this constraint from the constraint mapping.
  auto index = pos->second;
  ConstraintIndex.erase(pos);
  assert(Constraints[index] == constraint && "Mismatched constraint");

  // If this is the last constraint, just pop it off the list and we're done.
  unsigned lastIndex = Constraints.size()-1;
  if (index == lastIndex) {
    Constraints.pop_back();
    return;
  }

  // This constraint is somewhere in the middle; swap it with the last
  // constraint, so we can remove the constraint from the vector in O(1)
  // time rather than O(n) time.
  auto lastConstraint = Constraints[lastIndex];
  Constraints[index] = lastConstraint;
  ConstraintIndex[lastConstraint] = index;
  Constraints.pop_back();
}

ConstraintGraph::Node::Adjacency &
ConstraintGraph::Node::getAdjacency(TypeVariableType *typeVar) {
  assert(typeVar != TypeVar && "Cannot be adjacent to oneself");

  // Look for existing adjacency information.
  auto pos = AdjacencyInfo.find(typeVar);

  if (pos != AdjacencyInfo.end())
    return pos->second;

  // If we weren't already adjacent to this type variable, add it to the
  // list of adjacencies.
  pos = AdjacencyInfo.insert(
          { typeVar, { static_cast<unsigned>(Adjacencies.size()), 0, 0 } })
         .first;
  Adjacencies.push_back(typeVar);
  return pos->second;
}

void ConstraintGraph::Node::modifyAdjacency(
       TypeVariableType *typeVar,
       std::function<void(Adjacency& adj)> modify) {
   // Find the adjacency information.
  auto pos = AdjacencyInfo.find(typeVar);
  assert(pos != AdjacencyInfo.end() && "Type variables not adjacent");
  assert(Adjacencies[pos->second.Index] == typeVar && "Mismatched adjacency");

  // Perform the modification .
  modify(pos->second);

  // If the adjacency is not empty, leave the information in there.
  if (!pos->second.empty())
    return;

  // Remove this adjacency from the mapping.
  unsigned index = pos->second.Index;
  AdjacencyInfo.erase(pos);

  // If this adjacency is last in the vector, just pop it off.
  unsigned lastIndex = Adjacencies.size()-1;
  if (index == lastIndex) {
    Adjacencies.pop_back();
    return;
  }

  // This adjacency is somewhere in the middle; swap it with the last
  // adjacency so we can remove the adjacency from the vector in O(1) time
  // rather than O(n) time.
  auto lastTypeVar = Adjacencies[lastIndex];
  Adjacencies[index] = lastTypeVar;
  AdjacencyInfo[lastTypeVar].Index = index;
  Adjacencies.pop_back();
}

void ConstraintGraph::Node::addAdjacency(TypeVariableType *typeVar) {
  auto &adjacency = getAdjacency(typeVar);

  // Bump the degree of the adjacency.
  ++adjacency.NumConstraints;
}

void ConstraintGraph::Node::removeAdjacency(TypeVariableType *typeVar) {
  modifyAdjacency(typeVar, [](Adjacency &adj) {
    assert(adj.NumConstraints > 0 && "No adjacency to remove?");
    --adj.NumConstraints;
  });
}

void
ConstraintGraph::Node::addToEquivalenceClass(TypeVariableType *otherTypeVar) {
  assert(TypeVar == TypeVar->getImpl().getRepresentative(nullptr) &&
         "Can't extend equivalence class of non-representative type var");
  assert(TypeVar == otherTypeVar->getImpl().getRepresentative(nullptr) &&
         "Type variables are equivalent");
  if (EquivalenceClass.empty())
    EquivalenceClass.push_back(TypeVar);
  EquivalenceClass.push_back(otherTypeVar);
}

void ConstraintGraph::Node::addFixedBinding(TypeVariableType *typeVar) {
  auto &adjacency = getAdjacency(typeVar);

  assert(!adjacency.FixedBinding && "Already marked as a fixed binding?");
  adjacency.FixedBinding = true;
}

void ConstraintGraph::Node::removeFixedBinding(TypeVariableType *typeVar) {
  modifyAdjacency(typeVar, [](Adjacency &adj) {
    assert(adj.FixedBinding && "Not a fixed binding?");
    adj.FixedBinding = false;
  });
}


#pragma mark Graph mutation

void ConstraintGraph::addConstraint(Constraint *constraint) {
  // Gather the set of type variables referenced by this constraint.
  SmallVector<TypeVariableType *, 8> referencedTypeVars;
  gatherReferencedTypeVars(CS, constraint, referencedTypeVars);

  // For the nodes corresponding to each type variable...
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    Node &node = (*this)[typeVar];

    // Note the constraint within the node for that type variable.
    node.addConstraint(constraint);

    // Record the adjacent type variables.
    // This is O(N^2) in the number of referenced type variables, because
    // we're updating all of the adjacent type variables eagerly.
    for (auto otherTypeVar : referencedTypeVars) {
      if (typeVar == otherTypeVar)
        continue;

      node.addAdjacency(otherTypeVar);
    }
  }
}

void ConstraintGraph::removeConstraint(Constraint *constraint) {
  // Gather the set of type variables referenced by this constraint.
  SmallVector<TypeVariableType *, 8> referencedTypeVars;
  gatherReferencedTypeVars(CS, constraint, referencedTypeVars);

  // For the nodes corresponding to each type variable...
  for (auto typeVar : referencedTypeVars) {
    // Find the node for this type variable.
    Node &node = (*this)[typeVar];

    // Remove the constraint.
    node.removeConstraint(constraint);

    // Remove the adjacencies for all adjacent type variables.
    // This is O(N^2) in the number of referenced type variables, because
    // we're updating all of the adjacent type variables eagerly.
    for (auto otherTypeVar : referencedTypeVars) {
      if (typeVar == otherTypeVar)
        continue;

      node.removeAdjacency(otherTypeVar);
    }
  }  
}

#pragma mark Algorithms

/// Depth-first search for connected components
static void connectedComponentsDFS(ConstraintGraph &cg,
                                   ConstraintGraph::Node &node,
                                   unsigned component,
                                   SmallVectorImpl<unsigned> &components) {
  // Local function that recurses on the given set of type variables.
  auto visitAdjacencies = [&](ArrayRef<TypeVariableType *> typeVars) {
    for (auto adj : typeVars) {
      auto nodeAndIndex = cg.lookupNode(adj);
      // If we've already seen this node in this component, we're done.
      unsigned &curComponent = components[nodeAndIndex.second];
      if (curComponent == component)
        continue;

      // Mark this node as part of this connected component, then recurse.
      assert(curComponent == components.size() && "Already in a component?");
      curComponent = component;
      connectedComponentsDFS(cg, nodeAndIndex.first, component, components);
    }
  };

  // Recurse to mark adjacent nodes as part of this connected component.
  visitAdjacencies(node.getAdjacencies());

  // Figure out the representative for this type variable.
  auto &cs = cg.getConstraintSystem();
  auto typeVarRep = cs.getRepresentative(node.getTypeVariable());
  if (typeVarRep == node.getTypeVariable()) {
    // This type variable is the representative of its set; visit all of the
    // other type variables in the same equivalence class.
    visitAdjacencies(node.getEquivalenceClass().slice(1));
  } else {
    // Otherwise, visit the representative of the set.
    visitAdjacencies(typeVarRep);
  }
}

unsigned ConstraintGraph::computeConnectedComponents(
           SmallVectorImpl<TypeVariableType *> &typeVars,
           SmallVectorImpl<unsigned> &components) {

  // Initialize the components with component == # of type variables,
  // a sentinel value indicating
  unsigned numTypeVariables = TypeVariables.size();
  components.assign(numTypeVariables, numTypeVariables);

  // Perform a depth-first search from each type variable to identify
  // what component it is in.
  unsigned numComponents = 0;
  for (unsigned i = 0; i != numTypeVariables; ++i) {
    auto typeVar = TypeVariables[i];

    // Look up the node for this type variable.
    auto nodeAndIndex = lookupNode(typeVar);

    // If we're already assigned a component for this node, skip it.
    unsigned &curComponent = components[nodeAndIndex.second];
    if (curComponent != numTypeVariables)
      continue;

    // Record this component.
    unsigned component = numComponents++;

    // Note that this node is part of this component, then visit it.
    curComponent = component;
    connectedComponentsDFS(*this, nodeAndIndex.first, component, components);
  }

  // Figure out which components have unbound type variables; these
  // are the only components and type variables we want to report.
  SmallVector<bool, 4> componentHasUnboundTypeVar(numComponents, false);
  for (unsigned i = 0; i != numTypeVariables; ++i) {
    // If this type variable has a fixed type, skip it.
    if (CS.getFixedType(TypeVariables[i]))
      continue;

    componentHasUnboundTypeVar[components[i]] = true;
  }

  // Renumber the old components to the new components.
  SmallVector<unsigned, 4> componentRenumbering(numComponents, 0);
  numComponents = 0;
  for (unsigned i = 0, n = componentHasUnboundTypeVar.size(); i != n; ++i) {
    // Skip components that have no unbound type variables.
    if (!componentHasUnboundTypeVar[i])
      continue;

    componentRenumbering[i] = numComponents++;
  }

  // Copy over the type variables in the live components and remap
  // component numbers.
  unsigned outIndex = 0;
  for (unsigned i = 0, n = TypeVariables.size(); i != n; ++i) {
    // Skip type variables in dead components.
    if (!componentHasUnboundTypeVar[components[i]])
      continue;

    typeVars.push_back(TypeVariables[i]);
    components[outIndex] = componentRenumbering[components[i]];
    ++outIndex;
  }
  components.erase(components.begin() + outIndex, components.end());

  return numComponents;
}

#pragma mark Debugging output

void ConstraintGraph::Node::print(llvm::raw_ostream &out, unsigned indent) {
  out.indent(indent);
  TypeVar->print(out);
  out << ":\n";

  // Print constraints.
  if (!Constraints.empty()) {
    out.indent(indent + 2);
    out << "Constraints:\n";
    for (auto constraint : Constraints) {
      out.indent(indent + 4);
      constraint->print(out, /*FIXME:*/nullptr);
      out << "\n";
    }
  }

  // Print adjacencies.
  if (!Adjacencies.empty()) {
    out.indent(indent + 2);
    out << "Adjacencies:";
    for (auto adj : Adjacencies) {
      out << ' ';
      adj->print(out);

      auto &info = AdjacencyInfo[adj];
      auto degree = info.NumConstraints;
      if (degree > 1 || info.FixedBinding) {
        out << " (";
        if (degree > 1) {
          out << degree;
          if (info.FixedBinding)
            out << ", fixed";
        } else {
          out << "fixed";
        }
        out << ")";
      }
    }
    out << "\n";
  }

  // Print equivalence class.
  if (TypeVar->getImpl().getRepresentative(nullptr) == TypeVar &&
      EquivalenceClass.size() > 1) {
    out.indent(indent + 2);
    out << "Equivalence class:";
    for (unsigned i = 1, n = EquivalenceClass.size(); i != n; ++i) {
      out << ' ';
      EquivalenceClass[i]->print(out);
    }
    out << "\n";
  }
}

void ConstraintGraph::Node::dump() {
  print(llvm::dbgs(), 0);
}

void ConstraintGraph::print(llvm::raw_ostream &out) {
  for (auto typeVar : TypeVariables) {
    (*this)[typeVar].print(out, 2);
    out << "\n";
  }
}

void ConstraintGraph::dump() {
  print(llvm::dbgs());
}

#pragma mark Verification of graph invariants

/// Require that the given condition evaluate true.
///
/// If the condition is not true, complain about the problem and abort.
///
/// \param condition The actual Boolean condition.
///
/// \param complaint A string that describes the problem.
///
/// \param cg The constraint graph that failed verification.
///
/// \param node If non-null, the graph node that failed verification.
///
/// \param extraContext If provided, a function that will be called to
/// provide extra, contextual information about the failure.
static void _require(bool condition, const Twine &complaint,
                     ConstraintGraph &cg,
                     ConstraintGraph::Node *node,
                     const std::function<void()> &extraContext = nullptr) {
  if (condition)
    return;

  // Complain
  llvm::dbgs() << "Constraint graph verification failed: " << complaint << '\n';
  if (extraContext)
    extraContext();

  // Print the graph.
  // FIXME: Highlight the offending node/constraint/adjacency/etc.
  cg.print(llvm::dbgs());

  abort();
}

/// Print a type variable value.
static void printValue(llvm::raw_ostream &os, TypeVariableType *typeVar) {
  typeVar->print(os);
}

/// Print a constraint value.
static void printValue(llvm::raw_ostream &os, Constraint *constraint) {
  constraint->print(os, nullptr);
}

/// Print an unsigned value.
static void printValue(llvm::raw_ostream &os, unsigned value) {
  os << value;
}

void ConstraintGraph::Node::verify(ConstraintGraph &cg) {
#define require(condition, complaint) _require(condition, complaint, cg, this)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, cg, this, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, cg, this, [&] {         \
    llvm::dbgs() << "  ";                                       \
    printValue(llvm::dbgs(), value1);                           \
    llvm::dbgs() << " != ";                                     \
    printValue(llvm::dbgs(), value2);                           \
    llvm::dbgs() << '\n';                                       \
  })

  // Verify that the constraint map/vector haven't gotten out of sync.
  requireSameValue(Constraints.size(), ConstraintIndex.size(),
                   "constraint vector and map have different sizes");
  for (auto info : ConstraintIndex) {
    require(info.second < Constraints.size(), "constraint index out-of-range");
    requireSameValue(info.first, Constraints[info.second],
                     "constraint map provides wrong index into vector");
  }

  // Verify that the adjacency map/vector haven't gotten out of sync.
  requireSameValue(Adjacencies.size(), AdjacencyInfo.size(),
                   "adjacency vector and map have different sizes");
  for (auto info : AdjacencyInfo) {
    require(info.second.Index < Adjacencies.size(),
            "adjacency index out-of-range");
    requireSameValue(info.first, Adjacencies[info.second.Index],
                     "adjacency map provides wrong index into vector");
    require(!info.second.empty(),
            "adjacency information should have been removed");
    require(info.second.NumConstraints <= Constraints.size(),
            "adjacency information has higher degree than # of constraints");
  }

  // Based on the constraints we have, build up a representation of what
  // we expect the adjacencies to look like.
  llvm::DenseMap<TypeVariableType *, unsigned> expectedAdjacencies;
  for (auto constraint : Constraints) {
    SmallVector<TypeVariableType *, 4> referencedTypeVars;
    gatherReferencedTypeVars(cg.CS, constraint, referencedTypeVars);

    for (auto adjTypeVar : referencedTypeVars) {
      if (adjTypeVar == TypeVar)
        continue;

      ++expectedAdjacencies[adjTypeVar];
    }
  }

  // Make sure that the adjacencies we expect are the adjacencies we have.
  for (auto adj : expectedAdjacencies) {
    auto knownAdj = AdjacencyInfo.find(adj.first);
    requireWithContext(knownAdj != AdjacencyInfo.end(),
                       "missing adjacency information for type variable",
                       [&] {
      llvm::dbgs() << "  type variable=" << adj.first->getString() << 'n';
    });

    requireWithContext(adj.second == knownAdj->second.NumConstraints,
                       "wrong number of adjacencies for type variable",
                       [&] {
       llvm::dbgs() << "  type variable=" << adj.first->getString()
                    << " (" << adj.second << " vs. "
                    << knownAdj->second.NumConstraints
                    << ")\n";
     });
  }

  if (AdjacencyInfo.size() != expectedAdjacencies.size()) {
    // The adjacency information has something extra in it. Find the
    // extraneous type variable.
    for (auto adj : AdjacencyInfo) {
      requireWithContext(AdjacencyInfo.count(adj.first) > 0,
                         "extraneous adjacency info for type variable",
                         [&] {
        llvm::dbgs() << "  type variable=" << adj.first->getString() << '\n';
      });
    }
  }

#undef requireSameValue
#undef requireWithContext
#undef require
}

void ConstraintGraph::verify() {
#define require(condition, complaint) \
  _require(condition, complaint, *this, nullptr)
#define requireWithContext(condition, complaint, context) \
  _require(condition, complaint, *this, nullptr, context)
#define requireSameValue(value1, value2, complaint)             \
  _require(value1 == value2, complaint, *this, nullptr, [&] {   \
    llvm::dbgs() << "  " << value1 << " != " << value2 << '\n'; \
  })

  // Verify that the type variables are either representatives or represented
  // within their representative's equivalence class.
  // FIXME: Also check to make sure the equivalence classes aren't too large?
  for (auto typeVar : TypeVariables) {
    auto typeVarRep = CS.getRepresentative(typeVar);
    if (typeVar == typeVarRep)
      continue;

    // This type variable should be in the equivalence class of its
    // representative.
    auto &repNode = (*this)[typeVarRep];
    require(std::find(repNode.getEquivalenceClass().begin(),
                      repNode.getEquivalenceClass().end(),
                      typeVar) != repNode.getEquivalenceClass().end(),
            "type variable is not present in its representative's equiv class");
  }

  // Verify that our type variable map/vector are in sync.
  requireSameValue(TypeVariables.size(), Nodes.size(),
                   "type variables vector and node map have different sizes");
  for (auto node : Nodes) {
    require(node.second.Index < TypeVariables.size(),
            "out of bounds node index");
    requireSameValue(node.first, TypeVariables[node.second.Index],
                     "node map provides wrong index into type variable vector");
  }

  // Verify consistency of all of the nodes in the graph.
  for (auto node : Nodes) {
    node.second.NodePtr->verify(*this);
  }

  // FIXME: Verify that all of the constraints in the constraint system
  // are accounted for. This requires a better abstraction for tracking
  // the set of constraints that are live.

#undef requireSameValue
#undef requireWithContext
#undef require
}


