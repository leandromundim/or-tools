// Copyright 2010-2012 Google
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <math.h>
#include <string.h>
#include <algorithm>
#include "base/hash.h"
#include <string>
#include <utility>
#include <vector>

#include "base/commandlineflags.h"
#include "base/hash.h"
#include "base/int-type-indexed-vector.h"
#include "base/int-type.h"
#include "base/integral_types.h"
#include "base/logging.h"
#include "base/map-util.h"
#include "base/scoped_ptr.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "constraint_solver/constraint_solver.h"
#include "constraint_solver/constraint_solveri.h"
#include "util/bitset.h"
#include "util/const_int_array.h"

namespace operations_research {
namespace {
DEFINE_INT_TYPE(AtomIndex, int);

class Store;
class SumLessConstant;
class SumTriggerAction;

const static AtomIndex kFailAtom = AtomIndex(0);

template <class T> class UnorderedRevArray {
 public:
  UnorderedRevArray() : elements_(), num_elements_(0) {}

  UnorderedRevArray(const std::vector<T>& elements)
      : elements_(elements), num_elements_(elements.size()) {}

  ~UnorderedRevArray() {}

  int size() const { return num_elements_.Value(); }

  const T& operator[](int index) const {
    return Element(index);
  }

  const T& Element(int i) const {
    DCHECK_GE(i, 0);
    DCHECK_LT(i, num_elements_.Value());
    return elements_[i];
  }

  void Insert(Solver* const solver, T elt) {
    elements_.push_back(elt);
    num_elements_.Incr(solver);
  }

  void Remove(Solver* const solver, int position_of_element_to_remove) {
    num_elements_.Decr(solver);
    SwapTo(position_of_element_to_remove, num_elements_.Value());
  }

  void RemoveElement(Solver* const solver, const T& element) {
    for (int i = 0; i < num_elements_.Value(); ++i) {
      if (elements_[i] == element) {
        Remove(solver, i);
        return;
      }
    }
  }

  void clear(Solver* const solver) {
    num_elements_.SetValue(solver, 0);
  }

 private:
  void SwapTo(int current_position, int next_position) {
    if (current_position != next_position) {
      const T next_value = elements_[next_position];
      elements_[next_position] = elements_[current_position];
      elements_[current_position] = next_value;
    }
  }

  std::vector<T> elements_; // set of elements.
  NumericalRev<int> num_elements_; // number of elements in the set.
};

class Atom {
 public:
  Atom(AtomIndex index) : atom_index_(index) {}
  ~Atom() {}

  void Listen(SumLessConstant* const ct) {
    sum_less_constant_constraints_.push_back(ct);
  }

  void Listen(Solver* const solver, SumTriggerAction* const ct) {
    sum_trigger_actions_constraints_.Insert(solver, ct);
  }

  void StopListening(Solver* const solver, SumTriggerAction* const ct) {
    sum_trigger_actions_constraints_.RemoveElement(solver, ct);
  }

  void AddFlipAction(AtomIndex action) {
    actions_.push_back(action);
  }

  void Flip(Store* const store);

  bool IsFlipped() const {
    return flipped_.Switched();
  }

 private:
  const AtomIndex atom_index_;
  std::vector<SumLessConstant*> sum_less_constant_constraints_;
  UnorderedRevArray<SumTriggerAction*> sum_trigger_actions_constraints_;
  std::vector<AtomIndex> actions_;
  RevSwitch flipped_;
};

class Store : public Constraint {
 public:
  Store(Solver* const solver) : Constraint(solver) {}

  ~Store();

  AtomIndex TrueIndex(IntVar* const var) {
    int raw_index = indices_.Add(var);
    if (raw_index >= true_atoms_.size()) {
      true_atoms_.resize(raw_index);
      false_atoms_.resize(raw_index);
    }
    return 1 + AtomIndex(raw_index);
  }

  AtomIndex FalseIndex(IntVar* const var) {
    return -TrueIndex(var);
  }

  AtomIndex Index(IntVar* const var, bool negated) {
    return negated ? FalseIndex(var) : TrueIndex(var);
  }

  void VariableBound(int index) {
    if (indices_[index]->Min() == 0) {
      Flip(AtomIndex(-1 - index));
    } else {
      Flip(AtomIndex(1 + index));
    }
  }

  void Listen(AtomIndex atom, SumLessConstant* const ct) {
    FindAtom(atom)->Listen(ct);
  }

  void Listen(AtomIndex atom, SumTriggerAction* const ct) {
    FindAtom(atom)->Listen(solver(), ct);
  }

  void StopListening(AtomIndex atom, SumTriggerAction* const ct) {
    FindAtom(atom)->StopListening(solver(), ct);
  }

  void AddFlipAction(AtomIndex source, AtomIndex destination) {
    FindAtom(source)->AddFlipAction(destination);
  }

  void Flip(AtomIndex atom) {
    if (atom == kFailAtom || IsFlipped(-atom)) {
      solver()->Fail();
    } else {
      FindAtom(atom)->Flip(this);
    }
  }

  bool IsFlipped(AtomIndex atom) {
    if (atom == kFailAtom) {
      return false;
    } else {
      return FindAtom(atom)->IsFlipped();
    }
  }

  void Register(SumLessConstant* const ct) {
    sum_less_constant_constraints_.push_back(ct);
  }

  void Register(SumTriggerAction* const ct) {
    sum_trigger_actions_constraints_.push_back(ct);
  }

  virtual void Post() {
    for (int i = 0; i < indices_.size(); ++i) {
      Demon* const d = MakeConstraintDemon1(solver(),
                                            this,
                                            &Store::VariableBound,
                                            "VariableBound",
                                            i);
      indices_[i]->WhenDomain(d);
    }
  }

  virtual void InitialPropagate() {
    for (int i = 0; i < indices_.size(); ++i) {
      if (indices_[i]->Bound()) {
        VariableBound(i);
      }
    }
  }

 private:
  Atom* FindAtom(AtomIndex atom) const {
    CHECK_NE(atom, kFailAtom);
    if (atom > kFailAtom) {
      return true_atoms_[atom.value() - 1];
    } else {
      return false_atoms_[-1 - atom.value()];
    }
  }

  VectorMap<IntVar*> indices_;
  std::vector<Atom*> true_atoms_;
  std::vector<Atom*> false_atoms_;
  std::vector<SumLessConstant*> sum_less_constant_constraints_;
  std::vector<SumTriggerAction*> sum_trigger_actions_constraints_;
};

class SumLessConstant {
 public:
  SumLessConstant(std::vector<AtomIndex>& vars, int constant)
      : vars_(vars),
        constant_(constant),
        sum_(0) {}

  ~SumLessConstant() {}

  void Post(Store* const store) {
    store->Register(this);
    for (int i = 0; i < vars_.size(); ++i) {
      store->Listen(vars_[i], this);
    }
  }

  void Flip(Store* const store, AtomIndex index) {
    sum_.Incr(store->solver());
    if (sum_.Value() > constant_) {
      store->solver()->Fail();
    } else if (sum_.Value() == constant_) {
      UnflipAllPending(store);
    }
  }

  void UnflipAllPending(Store* const store) {
    int count = 0;
    for (int i = 0; i < vars_.size(); ++i) {
      if (!store->IsFlipped(vars_[i])) {
        count++;
        store->Flip(-vars_[i]);
      }
    }
    // Check count.
  }

 private:
  std::vector<AtomIndex> vars_;
  const int constant_;
  NumericalRev<int> sum_;
};

class SumTriggerAction {
 public:
  SumTriggerAction(std::vector<AtomIndex>& vars,
                   int constant,
                   std::vector<AtomIndex>& actions)
      : vars_(vars),
        constant_(constant),
        actions_(actions),
        sum_(0) {}

  ~SumTriggerAction() {}

  void Post(Store* const store) {
    store->Register(this);
    for (int i = 0; i < vars_.size(); ++i) {
      store->Listen(vars_[i], this);
    }
  }

  void Flip(Store* const store, AtomIndex index) {
    sum_.Incr(store->solver());
    if (sum_.Value() >= constant_) {
      StopListening(store);
      FlipAllAction(store);
    }
  }

  void StopListening(Store* const store) {
    for (int i = 0; i < vars_.size(); ++i) {
      store->StopListening(vars_[i], this);
    }
  }

  void FlipAllAction(Store* const store) {
    for (int i = 0; i < actions_.size(); ++i) {
      store->Flip(actions_[i]);
    }
  }

 private:
  std::vector<AtomIndex> vars_;
  const int constant_;
  std::vector<AtomIndex> actions_;
  NumericalRev<int> sum_;
};

void Atom::Flip(Store* const store) {
  CHECK(!flipped_.Switched());
  flipped_.Switch(store->solver());
  for (int i = 0; i < actions_.size(); ++i) {
    store->Flip(actions_[i]);
  }
  for (int i = 0; i < sum_less_constant_constraints_.size(); ++i) {
    sum_less_constant_constraints_[i]->Flip(store, atom_index_);
  }
  for (int i = 0; i < sum_trigger_actions_constraints_.size(); ++i) {
    sum_trigger_actions_constraints_[i]->Flip(store, atom_index_);
  }
}

Store::~Store() {
  STLDeleteElements(&sum_less_constant_constraints_);
  STLDeleteElements(&sum_trigger_actions_constraints_);
}
}  // namespace

bool AddBoolEq(Store* const store, IntVar* const left, IntVar* const right) {
  IntVar* left_var = NULL;
  bool left_negated = false;
  if (!store->solver()->IsBooleanVar(left, &left_var, &left_negated)) {
    return false;
  }
  IntVar* right_var = NULL;
  bool right_negated = false;
  if (!store->solver()->IsBooleanVar(right, &right_var, &right_negated)) {
    return false;
  }
  AtomIndex left_atom = store->Index(left_var, left_negated);
  AtomIndex right_atom = store->Index(right_var, right_negated);
  store->AddFlipAction(left_atom, right_atom);
  store->AddFlipAction(right_atom, left_atom);
  store->AddFlipAction(-left_atom, -right_atom);
  store->AddFlipAction(-right_atom, -left_atom);
  return true;
}

bool AddBoolLe(Store* const store, IntVar* const left, IntVar* const right) {
  IntVar* left_var = NULL;
  bool left_negated = false;
  if (!store->solver()->IsBooleanVar(left, &left_var, &left_negated)) {
    return false;
  }
  IntVar* right_var = NULL;
  bool right_negated = false;
  if (!store->solver()->IsBooleanVar(right, &right_var, &right_negated)) {
    return false;
  }
  AtomIndex left_atom = store->Index(left_var, left_negated);
  AtomIndex right_atom = store->Index(right_var, right_negated);
  store->AddFlipAction(left_atom, right_atom);
  store->AddFlipAction(-right_atom, -left_atom);
  return true;
}

bool AddBoolNot(Store* const store, IntVar* const left, IntVar* const right) {
  IntVar* left_var = NULL;
  bool left_negated = false;
  if (!store->solver()->IsBooleanVar(left, &left_var, &left_negated)) {
    return false;
  }
  IntVar* right_var = NULL;
  bool right_negated = false;
  if (!store->solver()->IsBooleanVar(right, &right_var, &right_negated)) {
    return false;
  }
  AtomIndex left_atom = store->Index(left_var, left_negated);
  AtomIndex right_atom = store->Index(right_var, right_negated);
  store->AddFlipAction(left_atom, -right_atom);
  store->AddFlipAction(right_atom, -left_atom);
  store->AddFlipAction(-left_atom, right_atom);
  store->AddFlipAction(-right_atom, left_atom);
  return true;
}
}  // namespace operations_research