/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/xla/service/while_loop_invariant_code_motion.h"
#include "tensorflow/compiler/xla/service/tuple_util.h"
#include "tensorflow/compiler/xla/service/while_util.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/gtl/flatmap.h"
#include "tensorflow/core/lib/gtl/flatset.h"
#include "tensorflow/core/lib/gtl/inlined_vector.h"

namespace xla {

using tensorflow::gtl::FlatMap;
using tensorflow::gtl::FlatSet;
using tensorflow::gtl::InlinedVector;

// Copies `to_hoist` to the computation containing `while_instr`, hoisting its
// operands as needed.  All of its transitive operands are expected to be either
// in `hoisted_instructions` or `unhoisted_invariant_instructions`.  This
// function hoists the operands in `unhoisted_invariant_instructions` and moves
// them into `hoisted_instructions`.
static void CreateLoopInvariantCopy(
    FlatMap<HloInstruction*, HloInstruction*>* hoisted_instructions,
    FlatSet<HloInstruction*>* unhoisted_invariant_instructions,
    HloInstruction* while_instr, HloInstruction* to_hoist) {
  HloComputation* parent_of_while = while_instr->parent();
  HloComputation* while_body = while_instr->while_body();

  struct DFSFrame {
    HloInstruction* instruction;
    int64 operand_index;
  };

  InlinedVector<DFSFrame, 8> dfs_stack;
  dfs_stack.push_back({to_hoist, 0});

  HloInstruction* while_body_param = while_body->parameter_instruction(0);
  HloInstruction* while_operand = while_instr->mutable_operand(0);

  do {
    DFSFrame* frame = &dfs_stack.back();
    if (frame->operand_index == frame->instruction->operand_count()) {
      HloInstruction* old_instruction = frame->instruction;

      // All of the operands for old_instruction have been cloned, so it is
      // time to clone old_instruction itself.

      auto get_new_operand = [&](HloInstruction* old_operand) {
        return old_operand == while_body_param
                   ? while_operand
                   : FindOrDie(*hoisted_instructions, old_operand);
      };

      InlinedVector<HloInstruction*, 4> new_operands;
      c_transform(old_instruction->operands(), std::back_inserter(new_operands),
                  get_new_operand);

      HloInstruction* new_instruction =
          parent_of_while->AddInstruction(old_instruction->CloneWithNewOperands(
              old_instruction->shape(), new_operands));

      InsertOrDie(hoisted_instructions, old_instruction, new_instruction);

      // Approximately half of the instructions that would normally be present
      // in unhoisted_invariant_instructions are constants.  We save a bit of
      // compile time by not putting these in the hashtable.
      CHECK_EQ(unhoisted_invariant_instructions->erase(old_instruction),
               to_hoist != old_instruction &&
                   old_instruction->opcode() != HloOpcode::kConstant);
      dfs_stack.pop_back();
      continue;
    }

    HloInstruction* next_operand =
        frame->instruction->mutable_operand(frame->operand_index++);
    if (hoisted_instructions->count(next_operand) ||
        next_operand == while_body_param) {
      continue;
    }

    dfs_stack.push_back({next_operand, 0});
  } while (!dfs_stack.empty());
}

// Returns true if `instruction` is worth hoisting only if it lets us hoist some
// instruction using it.  The rationale is that hoisting these instructions will
// prevent simplification and fusion in the while body.
static bool NotWorthHoistingIndividually(const HloInstruction& instruction) {
  switch (instruction.opcode()) {
    default:
      return false;

    case HloOpcode::kBitcast:
    case HloOpcode::kBroadcast:
    case HloOpcode::kConstant:
    case HloOpcode::kReverse:
    case HloOpcode::kSlice:
    case HloOpcode::kTuple:
      return true;

    case HloOpcode::kTranspose:
      return ShapeUtil::TransposeIsBitcast(
          /*input_shape=*/instruction.operand(0)->shape(),
          /*output_shape=*/instruction.shape(), instruction.dimensions());

    case HloOpcode::kReshape:
      return ShapeUtil::ReshapeIsBitcast(
          /*input_shape=*/instruction.operand(0)->shape(),
          /*output_shape=*/instruction.shape());
  }
}

// Populates `gte_set` with the GetTupleElement instructions in `while_body`
// that access elements in the parameter tuple that don't change across
// iterations.  Assumes `while_body` is the body computation of the while loop
// in question.
static void GatherInvariantGTEs(HloComputation* while_body,
                                FlatSet<HloInstruction*>* gte_set) {
  const HloInstruction::InstructionVector root_operands =
      while_body->root_instruction()->operands();
  for (int i = 0; i < root_operands.size(); i++) {
    HloInstruction* instr = root_operands[i];
    if (instr->opcode() == HloOpcode::kGetTupleElement &&
        instr->tuple_index() == i &&
        instr->operand(0) == while_body->parameter_instruction(0) &&
        ShapeUtil::IsArray(instr->shape())) {
      InsertOrDie(gte_set, instr);
    }
  }
}

static StatusOr<bool> TryHoistingInvariantInstructionsFromWhileBody(
    HloInstruction* while_instr) {
  auto print_no_metadata = HloPrintOptions{}.set_print_metadata(false);

  if (!ShapeUtil::IsTuple(while_instr->shape())) {
    // This restriction leaves one interesting pattern on the table:
    //
    //  while_body(f32[1024, 1024] %param) {
    //    %value = expensive_op(%param)
    //    outfeed(%value)
    //    ROOT = %param
    //  }
    //
    // If we see that pattern in the while, instead of generalizing this
    // algorithm to work with non-tuples, we should instead add a pass that
    // canonicalizes while loops like the above to use a tuple state.
    return false;
  }

  string while_instr_name = while_instr->ToString(print_no_metadata);
  VLOG(2) << "Trying to hoist from " << while_instr_name;

  HloComputation* while_body = while_instr->while_body();

  // Maps instructions in the while body to instructions hoisted outside the
  // while that compute the same value.
  FlatMap<HloInstruction*, HloInstruction*> hoisted_instructions;

  // Contains instructions that can be legally hoisted, but were deemed to be
  // unprofitable to be hoisted alone by NotWorthHoistingIndividually.  When we
  // hoist an instruction in this set, we move it from
  // unhoisted_invariant_instructions to hoisted_instructions.
  FlatSet<HloInstruction*> unhoisted_invariant_instructions;

  // Invariant GTE's axiomatically satisfy the constraints for
  // unhoisted_invariant_instructions -- they can be legally hoisted, but there
  // is no benefit to hoisting them unless something that uses it is also
  // hoisted.
  GatherInvariantGTEs(while_body, &unhoisted_invariant_instructions);

  if (unhoisted_invariant_instructions.empty()) {
    // There are no obviously loop invariant elements in the state being
    // threaded through the while loop so give up.  In theory this precondition
    // is too strong -- we could have code that e.g. permutes the elements in
    // the while state but uses a select to pick the same value on every
    // iteration.
    return false;
  }

  // instructions_to_replace[i] is hoisted into a loop invariant instruction
  // replacement_instructions[i].
  std::vector<HloInstruction*> instructions_to_replace;
  std::vector<HloInstruction*> replacement_instructions;

  for (auto* instruction : while_body->MakeInstructionPostOrder()) {
    if (instruction->HasSideEffect() ||
        instruction->opcode() == HloOpcode::kParameter ||
        !instruction->control_predecessors().empty() ||
        !instruction->control_successors().empty()) {
      continue;
    }

    auto is_invariant = [&](HloInstruction* op) {
      return hoisted_instructions.find(op) != hoisted_instructions.end() ||
             unhoisted_invariant_instructions.count(op) ||
             op->opcode() == HloOpcode::kConstant;
    };

    if (!c_all_of(instruction->operands(), is_invariant)) {
      continue;
    }

    if (NotWorthHoistingIndividually(*instruction)) {
      VLOG(2) << "Adding " << instruction->ToString(print_no_metadata)
              << " to unhoisted invariant set.";
      // Approximately half of the instructions that reach this point are
      // constants.  We save a bit of compile time by not putting these in the
      // hashtable.
      if (instruction->opcode() != HloOpcode::kConstant) {
        InsertOrDie(&unhoisted_invariant_instructions, instruction);
      }
      continue;
    }

    VLOG(2) << "Hoisting " << instruction->ToString(print_no_metadata);

    CreateLoopInvariantCopy(&hoisted_instructions,
                            &unhoisted_invariant_instructions, while_instr,
                            instruction);

    instructions_to_replace.push_back(instruction);
    replacement_instructions.push_back(
        FindOrDie(hoisted_instructions, instruction));
  }

  if (instructions_to_replace.empty()) {
    return false;
  }

  TF_ASSIGN_OR_RETURN(
      WhileUtil::MakeInstructionsLiveInResult live_in_instructions_result,
      WhileUtil::MakeInstructionsLiveIn(while_instr, replacement_instructions));

  HloComputation* new_while_body =
      live_in_instructions_result.new_while_instr->while_body();

  for (int i = 0; i < instructions_to_replace.size(); i++) {
    HloInstruction* instruction_to_replace_in_new_while =
        FindOrDie(live_in_instructions_result.while_body_instruction_map,
                  instructions_to_replace[i]);
    TF_RETURN_IF_ERROR(new_while_body->ReplaceInstruction(
        instruction_to_replace_in_new_while,
        live_in_instructions_result.while_body_live_in_values[i]));
  }

  VLOG(1) << "Hoisted " << instructions_to_replace.size()
          << " instructions from " << while_instr_name;

  return true;
}

StatusOr<bool> WhileLoopInvariantCodeMotion::Run(HloModule* module) {
  bool changed = false;
  std::vector<HloInstruction*> while_instrs;
  for (auto* comp : module->computations()) {
    c_copy_if(comp->instructions(), std::back_inserter(while_instrs),
              [](const HloInstruction* instr) {
                return instr->opcode() == HloOpcode::kWhile;
              });
  }

  for (HloInstruction* while_instr : while_instrs) {
    // Right now we only hoist computations from the while body, but
    // TryHoistingInvariantInstructionsFromWhileBody can be generalized to
    // optimize the condition computation too, if needed.
    //
    // The transform we do here is a pessmization for while loops that execute
    // zero times*, but at this time we expect those to be rare.  If this
    // becomes a problem we can consider using the conditional HLO to avoid
    // doing extra work for while loops with zero trip count.
    //
    // * We delete while loops that have a zero trip count, so this would have
    //   to be a while loop with a somewhat opaque condition expression.

    TF_ASSIGN_OR_RETURN(
        bool result,
        TryHoistingInvariantInstructionsFromWhileBody(while_instr));
    changed |= result;
  }
  return changed;
}
}  // namespace xla