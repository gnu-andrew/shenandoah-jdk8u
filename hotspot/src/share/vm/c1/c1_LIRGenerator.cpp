/*
 * Copyright (c) 2005, 2016, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "c1/c1_Defs.hpp"
#include "c1/c1_Compilation.hpp"
#include "c1/c1_FrameMap.hpp"
#include "c1/c1_Instruction.hpp"
#include "c1/c1_LIRAssembler.hpp"
#include "c1/c1_LIRGenerator.hpp"
#include "c1/c1_ValueStack.hpp"
#include "ci/ciArrayKlass.hpp"
#include "ci/ciInstance.hpp"
#include "ci/ciObjArray.hpp"
#include "gc_implementation/shenandoah/shenandoahHeap.hpp"
#include "gc_implementation/shenandoah/c1/shenandoahBarrierSetC1.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/macros.hpp"
#if INCLUDE_ALL_GCS
#include "gc_implementation/g1/heapRegion.hpp"
#endif // INCLUDE_ALL_GCS

#ifdef ASSERT
#define __ gen()->lir(__FILE__, __LINE__)->
#else
#define __ gen()->lir()->
#endif

#ifndef PATCHED_ADDR
#define PATCHED_ADDR  (max_jint)
#endif

void PhiResolverState::reset(int max_vregs) {
  // Initialize array sizes
  _virtual_operands.at_put_grow(max_vregs - 1, NULL, NULL);
  _virtual_operands.trunc_to(0);
  _other_operands.at_put_grow(max_vregs - 1, NULL, NULL);
  _other_operands.trunc_to(0);
  _vreg_table.at_put_grow(max_vregs - 1, NULL, NULL);
  _vreg_table.trunc_to(0);
}



//--------------------------------------------------------------
// PhiResolver

// Resolves cycles:
//
//  r1 := r2  becomes  temp := r1
//  r2 := r1           r1 := r2
//                     r2 := temp
// and orders moves:
//
//  r2 := r3  becomes  r1 := r2
//  r1 := r2           r2 := r3

PhiResolver::PhiResolver(LIRGenerator* gen, int max_vregs)
 : _gen(gen)
 , _state(gen->resolver_state())
 , _loop(NULL)
 , _temp(LIR_OprFact::illegalOpr)
{
  // reinitialize the shared state arrays
  _state.reset(max_vregs);
}


void PhiResolver::emit_move(LIR_Opr src, LIR_Opr dest) {
  assert(src->is_valid(), "");
  assert(dest->is_valid(), "");
  __ move(src, dest);
}


void PhiResolver::move_temp_to(LIR_Opr dest) {
  assert(_temp->is_valid(), "");
  emit_move(_temp, dest);
  NOT_PRODUCT(_temp = LIR_OprFact::illegalOpr);
}


void PhiResolver::move_to_temp(LIR_Opr src) {
  assert(_temp->is_illegal(), "");
  _temp = _gen->new_register(src->type());
  emit_move(src, _temp);
}


// Traverse assignment graph in depth first order and generate moves in post order
// ie. two assignments: b := c, a := b start with node c:
// Call graph: move(NULL, c) -> move(c, b) -> move(b, a)
// Generates moves in this order: move b to a and move c to b
// ie. cycle a := b, b := a start with node a
// Call graph: move(NULL, a) -> move(a, b) -> move(b, a)
// Generates moves in this order: move b to temp, move a to b, move temp to a
void PhiResolver::move(ResolveNode* src, ResolveNode* dest) {
  if (!dest->visited()) {
    dest->set_visited();
    for (int i = dest->no_of_destinations()-1; i >= 0; i --) {
      move(dest, dest->destination_at(i));
    }
  } else if (!dest->start_node()) {
    // cylce in graph detected
    assert(_loop == NULL, "only one loop valid!");
    _loop = dest;
    move_to_temp(src->operand());
    return;
  } // else dest is a start node

  if (!dest->assigned()) {
    if (_loop == dest) {
      move_temp_to(dest->operand());
      dest->set_assigned();
    } else if (src != NULL) {
      emit_move(src->operand(), dest->operand());
      dest->set_assigned();
    }
  }
}


PhiResolver::~PhiResolver() {
  int i;
  // resolve any cycles in moves from and to virtual registers
  for (i = virtual_operands().length() - 1; i >= 0; i --) {
    ResolveNode* node = virtual_operands()[i];
    if (!node->visited()) {
      _loop = NULL;
      move(NULL, node);
      node->set_start_node();
      assert(_temp->is_illegal(), "move_temp_to() call missing");
    }
  }

  // generate move for move from non virtual register to abitrary destination
  for (i = other_operands().length() - 1; i >= 0; i --) {
    ResolveNode* node = other_operands()[i];
    for (int j = node->no_of_destinations() - 1; j >= 0; j --) {
      emit_move(node->operand(), node->destination_at(j)->operand());
    }
  }
}


ResolveNode* PhiResolver::create_node(LIR_Opr opr, bool source) {
  ResolveNode* node;
  if (opr->is_virtual()) {
    int vreg_num = opr->vreg_number();
    node = vreg_table().at_grow(vreg_num, NULL);
    assert(node == NULL || node->operand() == opr, "");
    if (node == NULL) {
      node = new ResolveNode(opr);
      vreg_table()[vreg_num] = node;
    }
    // Make sure that all virtual operands show up in the list when
    // they are used as the source of a move.
    if (source && !virtual_operands().contains(node)) {
      virtual_operands().append(node);
    }
  } else {
    assert(source, "");
    node = new ResolveNode(opr);
    other_operands().append(node);
  }
  return node;
}


void PhiResolver::move(LIR_Opr src, LIR_Opr dest) {
  assert(dest->is_virtual(), "");
  // tty->print("move "); src->print(); tty->print(" to "); dest->print(); tty->cr();
  assert(src->is_valid(), "");
  assert(dest->is_valid(), "");
  ResolveNode* source = source_node(src);
  source->append(destination_node(dest));
}


//--------------------------------------------------------------
// LIRItem

void LIRItem::set_result(LIR_Opr opr) {
  assert(value()->operand()->is_illegal() || value()->operand()->is_constant(), "operand should never change");
  value()->set_operand(opr);

  if (opr->is_virtual()) {
    _gen->_instruction_for_operand.at_put_grow(opr->vreg_number(), value(), NULL);
  }

  _result = opr;
}

void LIRItem::load_item() {
  if (result()->is_illegal()) {
    // update the items result
    _result = value()->operand();
  }
  if (!result()->is_register()) {
    LIR_Opr reg = _gen->new_register(value()->type());
    __ move(result(), reg);
    if (result()->is_constant()) {
      _result = reg;
    } else {
      set_result(reg);
    }
  }
}


void LIRItem::load_for_store(BasicType type) {
  if (_gen->can_store_as_constant(value(), type)) {
    _result = value()->operand();
    if (!_result->is_constant()) {
      _result = LIR_OprFact::value_type(value()->type());
    }
  } else if (type == T_BYTE || type == T_BOOLEAN) {
    load_byte_item();
  } else {
    load_item();
  }
}

void LIRItem::load_item_force(LIR_Opr reg) {
  LIR_Opr r = result();
  if (r != reg) {
#if !defined(ARM) && !defined(E500V2)
    if (r->type() != reg->type()) {
      // moves between different types need an intervening spill slot
      r = _gen->force_to_spill(r, reg->type());
    }
#endif
    __ move(r, reg);
    _result = reg;
  }
}

ciObject* LIRItem::get_jobject_constant() const {
  ObjectType* oc = type()->as_ObjectType();
  if (oc) {
    return oc->constant_value();
  }
  return NULL;
}


jint LIRItem::get_jint_constant() const {
  assert(is_constant() && value() != NULL, "");
  assert(type()->as_IntConstant() != NULL, "type check");
  return type()->as_IntConstant()->value();
}


jint LIRItem::get_address_constant() const {
  assert(is_constant() && value() != NULL, "");
  assert(type()->as_AddressConstant() != NULL, "type check");
  return type()->as_AddressConstant()->value();
}


jfloat LIRItem::get_jfloat_constant() const {
  assert(is_constant() && value() != NULL, "");
  assert(type()->as_FloatConstant() != NULL, "type check");
  return type()->as_FloatConstant()->value();
}


jdouble LIRItem::get_jdouble_constant() const {
  assert(is_constant() && value() != NULL, "");
  assert(type()->as_DoubleConstant() != NULL, "type check");
  return type()->as_DoubleConstant()->value();
}


jlong LIRItem::get_jlong_constant() const {
  assert(is_constant() && value() != NULL, "");
  assert(type()->as_LongConstant() != NULL, "type check");
  return type()->as_LongConstant()->value();
}



//--------------------------------------------------------------


void LIRGenerator::init() {
  _bs = Universe::heap()->barrier_set();
}


void LIRGenerator::block_do_prolog(BlockBegin* block) {
#ifndef PRODUCT
  if (PrintIRWithLIR) {
    block->print();
  }
#endif

  // set up the list of LIR instructions
  assert(block->lir() == NULL, "LIR list already computed for this block");
  _lir = new LIR_List(compilation(), block);
  block->set_lir(_lir);

  __ branch_destination(block->label());

  if (LIRTraceExecution &&
      Compilation::current()->hir()->start()->block_id() != block->block_id() &&
      !block->is_set(BlockBegin::exception_entry_flag)) {
    assert(block->lir()->instructions_list()->length() == 1, "should come right after br_dst");
    trace_block_entry(block);
  }
}


void LIRGenerator::block_do_epilog(BlockBegin* block) {
#ifndef PRODUCT
  if (PrintIRWithLIR) {
    tty->cr();
  }
#endif

  // LIR_Opr for unpinned constants shouldn't be referenced by other
  // blocks so clear them out after processing the block.
  for (int i = 0; i < _unpinned_constants.length(); i++) {
    _unpinned_constants.at(i)->clear_operand();
  }
  _unpinned_constants.trunc_to(0);

  // clear our any registers for other local constants
  _constants.trunc_to(0);
  _reg_for_constants.trunc_to(0);
}


void LIRGenerator::block_do(BlockBegin* block) {
  CHECK_BAILOUT();

  block_do_prolog(block);
  set_block(block);

  for (Instruction* instr = block; instr != NULL; instr = instr->next()) {
    if (instr->is_pinned()) do_root(instr);
  }

  set_block(NULL);
  block_do_epilog(block);
}


//-------------------------LIRGenerator-----------------------------

// This is where the tree-walk starts; instr must be root;
void LIRGenerator::do_root(Value instr) {
  CHECK_BAILOUT();

  InstructionMark im(compilation(), instr);

  assert(instr->is_pinned(), "use only with roots");
  assert(instr->subst() == instr, "shouldn't have missed substitution");

  instr->visit(this);

  assert(!instr->has_uses() || instr->operand()->is_valid() ||
         instr->as_Constant() != NULL || bailed_out(), "invalid item set");
}


// This is called for each node in tree; the walk stops if a root is reached
void LIRGenerator::walk(Value instr) {
  InstructionMark im(compilation(), instr);
  //stop walk when encounter a root
  if (instr->is_pinned() && instr->as_Phi() == NULL || instr->operand()->is_valid()) {
    assert(instr->operand() != LIR_OprFact::illegalOpr || instr->as_Constant() != NULL, "this root has not yet been visited");
  } else {
    assert(instr->subst() == instr, "shouldn't have missed substitution");
    instr->visit(this);
    // assert(instr->use_count() > 0 || instr->as_Phi() != NULL, "leaf instruction must have a use");
  }
}


CodeEmitInfo* LIRGenerator::state_for(Instruction* x, ValueStack* state, bool ignore_xhandler) {
  assert(state != NULL, "state must be defined");

#ifndef PRODUCT
  state->verify();
#endif

  ValueStack* s = state;
  for_each_state(s) {
    if (s->kind() == ValueStack::EmptyExceptionState) {
      assert(s->stack_size() == 0 && s->locals_size() == 0 && (s->locks_size() == 0 || s->locks_size() == 1), "state must be empty");
      continue;
    }

    int index;
    Value value;
    for_each_stack_value(s, index, value) {
      assert(value->subst() == value, "missed substitution");
      if (!value->is_pinned() && value->as_Constant() == NULL && value->as_Local() == NULL) {
        walk(value);
        assert(value->operand()->is_valid(), "must be evaluated now");
      }
    }

    int bci = s->bci();
    IRScope* scope = s->scope();
    ciMethod* method = scope->method();

    MethodLivenessResult liveness = method->liveness_at_bci(bci);
    if (bci == SynchronizationEntryBCI) {
      if (x->as_ExceptionObject() || x->as_Throw()) {
        // all locals are dead on exit from the synthetic unlocker
        liveness.clear();
      } else {
        assert(x->as_MonitorEnter() || x->as_ProfileInvoke(), "only other cases are MonitorEnter and ProfileInvoke");
      }
    }
    if (!liveness.is_valid()) {
      // Degenerate or breakpointed method.
      bailout("Degenerate or breakpointed method");
    } else {
      assert((int)liveness.size() == s->locals_size(), "error in use of liveness");
      for_each_local_value(s, index, value) {
        assert(value->subst() == value, "missed substition");
        if (liveness.at(index) && !value->type()->is_illegal()) {
          if (!value->is_pinned() && value->as_Constant() == NULL && value->as_Local() == NULL) {
            walk(value);
            assert(value->operand()->is_valid(), "must be evaluated now");
          }
        } else {
          // NULL out this local so that linear scan can assume that all non-NULL values are live.
          s->invalidate_local(index);
        }
      }
    }
  }

  return new CodeEmitInfo(state, ignore_xhandler ? NULL : x->exception_handlers(), x->check_flag(Instruction::DeoptimizeOnException));
}


CodeEmitInfo* LIRGenerator::state_for(Instruction* x) {
  return state_for(x, x->exception_state());
}


void LIRGenerator::klass2reg_with_patching(LIR_Opr r, ciMetadata* obj, CodeEmitInfo* info, bool need_resolve) {
  /* C2 relies on constant pool entries being resolved (ciTypeFlow), so if TieredCompilation
   * is active and the class hasn't yet been resolved we need to emit a patch that resolves
   * the class. */
  if ((TieredCompilation && need_resolve) || !obj->is_loaded() || PatchALot) {
    assert(info != NULL, "info must be set if class is not loaded");
    __ klass2reg_patch(NULL, r, info);
  } else {
    // no patching needed
    __ metadata2reg(obj->constant_encoding(), r);
  }
}


void LIRGenerator::array_range_check(LIR_Opr array, LIR_Opr index,
                                    CodeEmitInfo* null_check_info, CodeEmitInfo* range_check_info) {
  CodeStub* stub = new RangeCheckStub(range_check_info, index);
  if (index->is_constant()) {
    cmp_mem_int(lir_cond_belowEqual, array, arrayOopDesc::length_offset_in_bytes(),
                index->as_jint(), null_check_info);
    __ branch(lir_cond_belowEqual, T_INT, stub); // forward branch
  } else {
    cmp_reg_mem(lir_cond_aboveEqual, index, array,
                arrayOopDesc::length_offset_in_bytes(), T_INT, null_check_info);
    __ branch(lir_cond_aboveEqual, T_INT, stub); // forward branch
  }
}


void LIRGenerator::nio_range_check(LIR_Opr buffer, LIR_Opr index, LIR_Opr result, CodeEmitInfo* info) {
  CodeStub* stub = new RangeCheckStub(info, index, true);
  if (index->is_constant()) {
    cmp_mem_int(lir_cond_belowEqual, buffer, java_nio_Buffer::limit_offset(), index->as_jint(), info);
    __ branch(lir_cond_belowEqual, T_INT, stub); // forward branch
  } else {
    cmp_reg_mem(lir_cond_aboveEqual, index, buffer,
                java_nio_Buffer::limit_offset(), T_INT, info);
    __ branch(lir_cond_aboveEqual, T_INT, stub); // forward branch
  }
  __ move(index, result);
}



void LIRGenerator::arithmetic_op(Bytecodes::Code code, LIR_Opr result, LIR_Opr left, LIR_Opr right, bool is_strictfp, LIR_Opr tmp_op, CodeEmitInfo* info) {
  LIR_Opr result_op = result;
  LIR_Opr left_op   = left;
  LIR_Opr right_op  = right;

  if (TwoOperandLIRForm && left_op != result_op) {
    assert(right_op != result_op, "malformed");
    __ move(left_op, result_op);
    left_op = result_op;
  }

  switch(code) {
    case Bytecodes::_dadd:
    case Bytecodes::_fadd:
    case Bytecodes::_ladd:
    case Bytecodes::_iadd:  __ add(left_op, right_op, result_op); break;
    case Bytecodes::_fmul:
    case Bytecodes::_lmul:  __ mul(left_op, right_op, result_op); break;

    case Bytecodes::_dmul:
      {
        if (is_strictfp) {
          __ mul_strictfp(left_op, right_op, result_op, tmp_op); break;
        } else {
          __ mul(left_op, right_op, result_op); break;
        }
      }
      break;

    case Bytecodes::_imul:
      {
        bool did_strength_reduce = false;

        if (right->is_constant()) {
          jint c = right->as_jint();
          if (c > 0 && is_power_of_2(c)) {
            // do not need tmp here
            __ shift_left(left_op, exact_log2(c), result_op);
            did_strength_reduce = true;
          } else {
            did_strength_reduce = strength_reduce_multiply(left_op, c, result_op, tmp_op);
          }
        }
        // we couldn't strength reduce so just emit the multiply
        if (!did_strength_reduce) {
          __ mul(left_op, right_op, result_op);
        }
      }
      break;

    case Bytecodes::_dsub:
    case Bytecodes::_fsub:
    case Bytecodes::_lsub:
    case Bytecodes::_isub: __ sub(left_op, right_op, result_op); break;

    case Bytecodes::_fdiv: __ div (left_op, right_op, result_op); break;
    // ldiv and lrem are implemented with a direct runtime call

    case Bytecodes::_ddiv:
      {
        if (is_strictfp) {
          __ div_strictfp (left_op, right_op, result_op, tmp_op); break;
        } else {
          __ div (left_op, right_op, result_op); break;
        }
      }
      break;

    case Bytecodes::_drem:
    case Bytecodes::_frem: __ rem (left_op, right_op, result_op); break;

    default: ShouldNotReachHere();
  }
}


void LIRGenerator::arithmetic_op_int(Bytecodes::Code code, LIR_Opr result, LIR_Opr left, LIR_Opr right, LIR_Opr tmp) {
  arithmetic_op(code, result, left, right, false, tmp);
}


void LIRGenerator::arithmetic_op_long(Bytecodes::Code code, LIR_Opr result, LIR_Opr left, LIR_Opr right, CodeEmitInfo* info) {
  arithmetic_op(code, result, left, right, false, LIR_OprFact::illegalOpr, info);
}


void LIRGenerator::arithmetic_op_fpu(Bytecodes::Code code, LIR_Opr result, LIR_Opr left, LIR_Opr right, bool is_strictfp, LIR_Opr tmp) {
  arithmetic_op(code, result, left, right, is_strictfp, tmp);
}


void LIRGenerator::shift_op(Bytecodes::Code code, LIR_Opr result_op, LIR_Opr value, LIR_Opr count, LIR_Opr tmp) {
  if (TwoOperandLIRForm && value != result_op) {
    assert(count != result_op, "malformed");
    __ move(value, result_op);
    value = result_op;
  }

  assert(count->is_constant() || count->is_register(), "must be");
  switch(code) {
  case Bytecodes::_ishl:
  case Bytecodes::_lshl: __ shift_left(value, count, result_op, tmp); break;
  case Bytecodes::_ishr:
  case Bytecodes::_lshr: __ shift_right(value, count, result_op, tmp); break;
  case Bytecodes::_iushr:
  case Bytecodes::_lushr: __ unsigned_shift_right(value, count, result_op, tmp); break;
  default: ShouldNotReachHere();
  }
}


void LIRGenerator::logic_op (Bytecodes::Code code, LIR_Opr result_op, LIR_Opr left_op, LIR_Opr right_op) {
  if (TwoOperandLIRForm && left_op != result_op) {
    assert(right_op != result_op, "malformed");
    __ move(left_op, result_op);
    left_op = result_op;
  }

  switch(code) {
    case Bytecodes::_iand:
    case Bytecodes::_land:  __ logical_and(left_op, right_op, result_op); break;

    case Bytecodes::_ior:
    case Bytecodes::_lor:   __ logical_or(left_op, right_op, result_op);  break;

    case Bytecodes::_ixor:
    case Bytecodes::_lxor:  __ logical_xor(left_op, right_op, result_op); break;

    default: ShouldNotReachHere();
  }
}


void LIRGenerator::monitor_enter(LIR_Opr object, LIR_Opr lock, LIR_Opr hdr, LIR_Opr scratch, int monitor_no, CodeEmitInfo* info_for_exception, CodeEmitInfo* info) {
  if (!GenerateSynchronizationCode) return;
  // for slow path, use debug info for state after successful locking
  CodeStub* slow_path = new MonitorEnterStub(object, lock, info);
  __ load_stack_address_monitor(monitor_no, lock);
  // for handling NullPointerException, use debug info representing just the lock stack before this monitorenter
  __ lock_object(hdr, object, lock, scratch, slow_path, info_for_exception);
}


void LIRGenerator::monitor_exit(LIR_Opr object, LIR_Opr lock, LIR_Opr new_hdr, LIR_Opr scratch, int monitor_no) {
  if (!GenerateSynchronizationCode) return;
  // setup registers
  LIR_Opr hdr = lock;
  lock = new_hdr;
  CodeStub* slow_path = new MonitorExitStub(lock, UseFastLocking, monitor_no);
  __ load_stack_address_monitor(monitor_no, lock);
  __ unlock_object(hdr, object, lock, scratch, slow_path);
}

#ifndef PRODUCT
void LIRGenerator::print_if_not_loaded(const NewInstance* new_instance) {
  if (PrintNotLoaded && !new_instance->klass()->is_loaded()) {
    tty->print_cr("   ###class not loaded at new bci %d", new_instance->printable_bci());
  } else if (PrintNotLoaded && (TieredCompilation && new_instance->is_unresolved())) {
    tty->print_cr("   ###class not resolved at new bci %d", new_instance->printable_bci());
  }
}
#endif

void LIRGenerator::new_instance(LIR_Opr dst, ciInstanceKlass* klass, bool is_unresolved, LIR_Opr scratch1, LIR_Opr scratch2, LIR_Opr scratch3, LIR_Opr scratch4, LIR_Opr klass_reg, CodeEmitInfo* info) {
  klass2reg_with_patching(klass_reg, klass, info, is_unresolved);
  // If klass is not loaded we do not know if the klass has finalizers:
  if (UseFastNewInstance && klass->is_loaded()
      && !Klass::layout_helper_needs_slow_path(klass->layout_helper())) {

    Runtime1::StubID stub_id = klass->is_initialized() ? Runtime1::fast_new_instance_id : Runtime1::fast_new_instance_init_check_id;

    CodeStub* slow_path = new NewInstanceStub(klass_reg, dst, klass, info, stub_id);

    assert(klass->is_loaded(), "must be loaded");
    // allocate space for instance
    assert(klass->size_helper() >= 0, "illegal instance size");
    const int instance_size = align_object_size(klass->size_helper());
    __ allocate_object(dst, scratch1, scratch2, scratch3, scratch4,
                       oopDesc::header_size(), instance_size, klass_reg, !klass->is_initialized(), slow_path);
  } else {
    CodeStub* slow_path = new NewInstanceStub(klass_reg, dst, klass, info, Runtime1::new_instance_id);
    __ branch(lir_cond_always, T_ILLEGAL, slow_path);
    __ branch_destination(slow_path->continuation());
  }
}


static bool is_constant_zero(Instruction* inst) {
  IntConstant* c = inst->type()->as_IntConstant();
  if (c) {
    return (c->value() == 0);
  }
  return false;
}


static bool positive_constant(Instruction* inst) {
  IntConstant* c = inst->type()->as_IntConstant();
  if (c) {
    return (c->value() >= 0);
  }
  return false;
}


static ciArrayKlass* as_array_klass(ciType* type) {
  if (type != NULL && type->is_array_klass() && type->is_loaded()) {
    return (ciArrayKlass*)type;
  } else {
    return NULL;
  }
}

static ciType* phi_declared_type(Phi* phi) {
  ciType* t = phi->operand_at(0)->declared_type();
  if (t == NULL) {
    return NULL;
  }
  for(int i = 1; i < phi->operand_count(); i++) {
    if (t != phi->operand_at(i)->declared_type()) {
      return NULL;
    }
  }
  return t;
}

void LIRGenerator::arraycopy_helper(Intrinsic* x, int* flagsp, ciArrayKlass** expected_typep) {
  Instruction* src     = x->argument_at(0);
  Instruction* src_pos = x->argument_at(1);
  Instruction* dst     = x->argument_at(2);
  Instruction* dst_pos = x->argument_at(3);
  Instruction* length  = x->argument_at(4);

  // first try to identify the likely type of the arrays involved
  ciArrayKlass* expected_type = NULL;
  bool is_exact = false, src_objarray = false, dst_objarray = false;
  {
    ciArrayKlass* src_exact_type    = as_array_klass(src->exact_type());
    ciArrayKlass* src_declared_type = as_array_klass(src->declared_type());
    Phi* phi;
    if (src_declared_type == NULL && (phi = src->as_Phi()) != NULL) {
      src_declared_type = as_array_klass(phi_declared_type(phi));
    }
    ciArrayKlass* dst_exact_type    = as_array_klass(dst->exact_type());
    ciArrayKlass* dst_declared_type = as_array_klass(dst->declared_type());
    if (dst_declared_type == NULL && (phi = dst->as_Phi()) != NULL) {
      dst_declared_type = as_array_klass(phi_declared_type(phi));
    }

    if (src_exact_type != NULL && src_exact_type == dst_exact_type) {
      // the types exactly match so the type is fully known
      is_exact = true;
      expected_type = src_exact_type;
    } else if (dst_exact_type != NULL && dst_exact_type->is_obj_array_klass()) {
      ciArrayKlass* dst_type = (ciArrayKlass*) dst_exact_type;
      ciArrayKlass* src_type = NULL;
      if (src_exact_type != NULL && src_exact_type->is_obj_array_klass()) {
        src_type = (ciArrayKlass*) src_exact_type;
      } else if (src_declared_type != NULL && src_declared_type->is_obj_array_klass()) {
        src_type = (ciArrayKlass*) src_declared_type;
      }
      if (src_type != NULL) {
        if (src_type->element_type()->is_subtype_of(dst_type->element_type())) {
          is_exact = true;
          expected_type = dst_type;
        }
      }
    }
    // at least pass along a good guess
    if (expected_type == NULL) expected_type = dst_exact_type;
    if (expected_type == NULL) expected_type = src_declared_type;
    if (expected_type == NULL) expected_type = dst_declared_type;

    src_objarray = (src_exact_type && src_exact_type->is_obj_array_klass()) || (src_declared_type && src_declared_type->is_obj_array_klass());
    dst_objarray = (dst_exact_type && dst_exact_type->is_obj_array_klass()) || (dst_declared_type && dst_declared_type->is_obj_array_klass());
  }

  // if a probable array type has been identified, figure out if any
  // of the required checks for a fast case can be elided.
  int flags = LIR_OpArrayCopy::all_flags;

  if (!src_objarray)
    flags &= ~LIR_OpArrayCopy::src_objarray;
  if (!dst_objarray)
    flags &= ~LIR_OpArrayCopy::dst_objarray;

  if (!x->arg_needs_null_check(0))
    flags &= ~LIR_OpArrayCopy::src_null_check;
  if (!x->arg_needs_null_check(2))
    flags &= ~LIR_OpArrayCopy::dst_null_check;


  if (expected_type != NULL) {
    Value length_limit = NULL;

    IfOp* ifop = length->as_IfOp();
    if (ifop != NULL) {
      // look for expressions like min(v, a.length) which ends up as
      //   x > y ? y : x  or  x >= y ? y : x
      if ((ifop->cond() == If::gtr || ifop->cond() == If::geq) &&
          ifop->x() == ifop->fval() &&
          ifop->y() == ifop->tval()) {
        length_limit = ifop->y();
      }
    }

    // try to skip null checks and range checks
    NewArray* src_array = src->as_NewArray();
    if (src_array != NULL) {
      flags &= ~LIR_OpArrayCopy::src_null_check;
      if (length_limit != NULL &&
          src_array->length() == length_limit &&
          is_constant_zero(src_pos)) {
        flags &= ~LIR_OpArrayCopy::src_range_check;
      }
    }

    NewArray* dst_array = dst->as_NewArray();
    if (dst_array != NULL) {
      flags &= ~LIR_OpArrayCopy::dst_null_check;
      if (length_limit != NULL &&
          dst_array->length() == length_limit &&
          is_constant_zero(dst_pos)) {
        flags &= ~LIR_OpArrayCopy::dst_range_check;
      }
    }

    // check from incoming constant values
    if (positive_constant(src_pos))
      flags &= ~LIR_OpArrayCopy::src_pos_positive_check;
    if (positive_constant(dst_pos))
      flags &= ~LIR_OpArrayCopy::dst_pos_positive_check;
    if (positive_constant(length))
      flags &= ~LIR_OpArrayCopy::length_positive_check;

    // see if the range check can be elided, which might also imply
    // that src or dst is non-null.
    ArrayLength* al = length->as_ArrayLength();
    if (al != NULL) {
      if (al->array() == src) {
        // it's the length of the source array
        flags &= ~LIR_OpArrayCopy::length_positive_check;
        flags &= ~LIR_OpArrayCopy::src_null_check;
        if (is_constant_zero(src_pos))
          flags &= ~LIR_OpArrayCopy::src_range_check;
      }
      if (al->array() == dst) {
        // it's the length of the destination array
        flags &= ~LIR_OpArrayCopy::length_positive_check;
        flags &= ~LIR_OpArrayCopy::dst_null_check;
        if (is_constant_zero(dst_pos))
          flags &= ~LIR_OpArrayCopy::dst_range_check;
      }
    }
    if (is_exact) {
      flags &= ~LIR_OpArrayCopy::type_check;
    }
  }

  IntConstant* src_int = src_pos->type()->as_IntConstant();
  IntConstant* dst_int = dst_pos->type()->as_IntConstant();
  if (src_int && dst_int) {
    int s_offs = src_int->value();
    int d_offs = dst_int->value();
    if (src_int->value() >= dst_int->value()) {
      flags &= ~LIR_OpArrayCopy::overlapping;
    }
    if (expected_type != NULL) {
      BasicType t = expected_type->element_type()->basic_type();
      int element_size = type2aelembytes(t);
      if (((arrayOopDesc::base_offset_in_bytes(t) + s_offs * element_size) % HeapWordSize == 0) &&
          ((arrayOopDesc::base_offset_in_bytes(t) + d_offs * element_size) % HeapWordSize == 0)) {
        flags &= ~LIR_OpArrayCopy::unaligned;
      }
    }
  } else if (src_pos == dst_pos || is_constant_zero(dst_pos)) {
    // src and dest positions are the same, or dst is zero so assume
    // nonoverlapping copy.
    flags &= ~LIR_OpArrayCopy::overlapping;
  }

  if (src == dst) {
    // moving within a single array so no type checks are needed
    if (flags & LIR_OpArrayCopy::type_check) {
      flags &= ~LIR_OpArrayCopy::type_check;
    }
  }
  *flagsp = flags;
  *expected_typep = (ciArrayKlass*)expected_type;
}


LIR_Opr LIRGenerator::round_item(LIR_Opr opr) {
  assert(opr->is_register(), "why spill if item is not register?");

  if (RoundFPResults && UseSSE < 1 && opr->is_single_fpu()) {
    LIR_Opr result = new_register(T_FLOAT);
    set_vreg_flag(result, must_start_in_memory);
    assert(opr->is_register(), "only a register can be spilled");
    assert(opr->value_type()->is_float(), "rounding only for floats available");
    __ roundfp(opr, LIR_OprFact::illegalOpr, result);
    return result;
  }
  return opr;
}


LIR_Opr LIRGenerator::force_to_spill(LIR_Opr value, BasicType t) {
  assert(type2size[t] == type2size[value->type()],
         err_msg_res("size mismatch: t=%s, value->type()=%s", type2name(t), type2name(value->type())));
  if (!value->is_register()) {
    // force into a register
    LIR_Opr r = new_register(value->type());
    __ move(value, r);
    value = r;
  }

  // create a spill location
  LIR_Opr tmp = new_register(t);
  set_vreg_flag(tmp, LIRGenerator::must_start_in_memory);

  // move from register to spill
  __ move(value, tmp);
  return tmp;
}

void LIRGenerator::profile_branch(If* if_instr, If::Condition cond) {
  if (if_instr->should_profile()) {
    ciMethod* method = if_instr->profiled_method();
    assert(method != NULL, "method should be set if branch is profiled");
    ciMethodData* md = method->method_data_or_null();
    assert(md != NULL, "Sanity");
    ciProfileData* data = md->bci_to_data(if_instr->profiled_bci());
    assert(data != NULL, "must have profiling data");
    assert(data->is_BranchData(), "need BranchData for two-way branches");
    int taken_count_offset     = md->byte_offset_of_slot(data, BranchData::taken_offset());
    int not_taken_count_offset = md->byte_offset_of_slot(data, BranchData::not_taken_offset());
    if (if_instr->is_swapped()) {
      int t = taken_count_offset;
      taken_count_offset = not_taken_count_offset;
      not_taken_count_offset = t;
    }

    LIR_Opr md_reg = new_register(T_METADATA);
    __ metadata2reg(md->constant_encoding(), md_reg);

    LIR_Opr data_offset_reg = new_pointer_register();
    __ cmove(lir_cond(cond),
             LIR_OprFact::intptrConst(taken_count_offset),
             LIR_OprFact::intptrConst(not_taken_count_offset),
             data_offset_reg, as_BasicType(if_instr->x()->type()));

    // MDO cells are intptr_t, so the data_reg width is arch-dependent.
    LIR_Opr data_reg = new_pointer_register();
    LIR_Address* data_addr = new LIR_Address(md_reg, data_offset_reg, data_reg->type());
    __ move(data_addr, data_reg);
    // Use leal instead of add to avoid destroying condition codes on x86
    LIR_Address* fake_incr_value = new LIR_Address(data_reg, DataLayout::counter_increment, T_INT);
    __ leal(LIR_OprFact::address(fake_incr_value), data_reg);
    __ move(data_reg, data_addr);
  }
}

// Phi technique:
// This is about passing live values from one basic block to the other.
// In code generated with Java it is rather rare that more than one
// value is on the stack from one basic block to the other.
// We optimize our technique for efficient passing of one value
// (of type long, int, double..) but it can be extended.
// When entering or leaving a basic block, all registers and all spill
// slots are release and empty. We use the released registers
// and spill slots to pass the live values from one block
// to the other. The topmost value, i.e., the value on TOS of expression
// stack is passed in registers. All other values are stored in spilling
// area. Every Phi has an index which designates its spill slot
// At exit of a basic block, we fill the register(s) and spill slots.
// At entry of a basic block, the block_prolog sets up the content of phi nodes
// and locks necessary registers and spilling slots.


// move current value to referenced phi function
void LIRGenerator::move_to_phi(PhiResolver* resolver, Value cur_val, Value sux_val) {
  Phi* phi = sux_val->as_Phi();
  // cur_val can be null without phi being null in conjunction with inlining
  if (phi != NULL && cur_val != NULL && cur_val != phi && !phi->is_illegal()) {
    LIR_Opr operand = cur_val->operand();
    if (cur_val->operand()->is_illegal()) {
      assert(cur_val->as_Constant() != NULL || cur_val->as_Local() != NULL,
             "these can be produced lazily");
      operand = operand_for_instruction(cur_val);
    }
    resolver->move(operand, operand_for_instruction(phi));
  }
}


// Moves all stack values into their PHI position
void LIRGenerator::move_to_phi(ValueStack* cur_state) {
  BlockBegin* bb = block();
  if (bb->number_of_sux() == 1) {
    BlockBegin* sux = bb->sux_at(0);
    assert(sux->number_of_preds() > 0, "invalid CFG");

    // a block with only one predecessor never has phi functions
    if (sux->number_of_preds() > 1) {
      int max_phis = cur_state->stack_size() + cur_state->locals_size();
      PhiResolver resolver(this, _virtual_register_number + max_phis * 2);

      ValueStack* sux_state = sux->state();
      Value sux_value;
      int index;

      assert(cur_state->scope() == sux_state->scope(), "not matching");
      assert(cur_state->locals_size() == sux_state->locals_size(), "not matching");
      assert(cur_state->stack_size() == sux_state->stack_size(), "not matching");

      for_each_stack_value(sux_state, index, sux_value) {
        move_to_phi(&resolver, cur_state->stack_at(index), sux_value);
      }

      for_each_local_value(sux_state, index, sux_value) {
        move_to_phi(&resolver, cur_state->local_at(index), sux_value);
      }

      assert(cur_state->caller_state() == sux_state->caller_state(), "caller states must be equal");
    }
  }
}


LIR_Opr LIRGenerator::new_register(BasicType type) {
  int vreg = _virtual_register_number;
  // add a little fudge factor for the bailout, since the bailout is
  // only checked periodically.  This gives a few extra registers to
  // hand out before we really run out, which helps us keep from
  // tripping over assertions.
  if (vreg + 20 >= LIR_OprDesc::vreg_max) {
    bailout("out of virtual registers");
    if (vreg + 2 >= LIR_OprDesc::vreg_max) {
      // wrap it around
      _virtual_register_number = LIR_OprDesc::vreg_base;
    }
  }
  _virtual_register_number += 1;
  return LIR_OprFact::virtual_register(vreg, type);
}


// Try to lock using register in hint
LIR_Opr LIRGenerator::rlock(Value instr) {
  return new_register(instr->type());
}


// does an rlock and sets result
LIR_Opr LIRGenerator::rlock_result(Value x) {
  LIR_Opr reg = rlock(x);
  set_result(x, reg);
  return reg;
}


// does an rlock and sets result
LIR_Opr LIRGenerator::rlock_result(Value x, BasicType type) {
  LIR_Opr reg;
  switch (type) {
  case T_BYTE:
  case T_BOOLEAN:
    reg = rlock_byte(type);
    break;
  default:
    reg = rlock(x);
    break;
  }

  set_result(x, reg);
  return reg;
}


//---------------------------------------------------------------------
ciObject* LIRGenerator::get_jobject_constant(Value value) {
  ObjectType* oc = value->type()->as_ObjectType();
  if (oc) {
    return oc->constant_value();
  }
  return NULL;
}


void LIRGenerator::do_ExceptionObject(ExceptionObject* x) {
  assert(block()->is_set(BlockBegin::exception_entry_flag), "ExceptionObject only allowed in exception handler block");
  assert(block()->next() == x, "ExceptionObject must be first instruction of block");

  // no moves are created for phi functions at the begin of exception
  // handlers, so assign operands manually here
  for_each_phi_fun(block(), phi,
                   operand_for_instruction(phi));

  LIR_Opr thread_reg = getThreadPointer();
  __ move_wide(new LIR_Address(thread_reg, in_bytes(JavaThread::exception_oop_offset()), T_OBJECT),
               exceptionOopOpr());
  __ move_wide(LIR_OprFact::oopConst(NULL),
               new LIR_Address(thread_reg, in_bytes(JavaThread::exception_oop_offset()), T_OBJECT));
  __ move_wide(LIR_OprFact::oopConst(NULL),
               new LIR_Address(thread_reg, in_bytes(JavaThread::exception_pc_offset()), T_OBJECT));

  LIR_Opr result = new_register(T_OBJECT);
  __ move(exceptionOopOpr(), result);
  set_result(x, result);
}


//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//                        visitor functions
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------
//----------------------------------------------------------------------

void LIRGenerator::do_Phi(Phi* x) {
  // phi functions are never visited directly
  ShouldNotReachHere();
}


// Code for a constant is generated lazily unless the constant is frequently used and can't be inlined.
void LIRGenerator::do_Constant(Constant* x) {
  if (x->state_before() != NULL) {
    // Any constant with a ValueStack requires patching so emit the patch here
    LIR_Opr reg = rlock_result(x);
    CodeEmitInfo* info = state_for(x, x->state_before());
    __ oop2reg_patch(NULL, reg, info);
  } else if (x->use_count() > 1 && !can_inline_as_constant(x)) {
    if (!x->is_pinned()) {
      // unpinned constants are handled specially so that they can be
      // put into registers when they are used multiple times within a
      // block.  After the block completes their operand will be
      // cleared so that other blocks can't refer to that register.
      set_result(x, load_constant(x));
    } else {
      LIR_Opr res = x->operand();
      if (!res->is_valid()) {
        res = LIR_OprFact::value_type(x->type());
      }
      if (res->is_constant()) {
        LIR_Opr reg = rlock_result(x);
        __ move(res, reg);
      } else {
        set_result(x, res);
      }
    }
  } else {
    set_result(x, LIR_OprFact::value_type(x->type()));
  }
}


void LIRGenerator::do_Local(Local* x) {
  // operand_for_instruction has the side effect of setting the result
  // so there's no need to do it here.
  operand_for_instruction(x);
}


void LIRGenerator::do_IfInstanceOf(IfInstanceOf* x) {
  Unimplemented();
}


void LIRGenerator::do_Return(Return* x) {
  if (compilation()->env()->dtrace_method_probes()) {
    BasicTypeList signature;
    signature.append(LP64_ONLY(T_LONG) NOT_LP64(T_INT));    // thread
    signature.append(T_METADATA); // Method*
    LIR_OprList* args = new LIR_OprList();
    args->append(getThreadPointer());
    LIR_Opr meth = new_register(T_METADATA);
    __ metadata2reg(method()->constant_encoding(), meth);
    args->append(meth);
    call_runtime(&signature, args, CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_exit), voidType, NULL);
  }

  if (x->type()->is_void()) {
    __ return_op(LIR_OprFact::illegalOpr);
  } else {
    LIR_Opr reg = result_register_for(x->type(), /*callee=*/true);
    LIRItem result(x->result(), this);

    result.load_item_force(reg);
    __ return_op(result.result());
  }
  set_no_result(x);
}

// Examble: ref.get()
// Combination of LoadField and g1 pre-write barrier
void LIRGenerator::do_Reference_get(Intrinsic* x) {

  const int referent_offset = java_lang_ref_Reference::referent_offset;
  guarantee(referent_offset > 0, "referent offset not initialized");

  assert(x->number_of_arguments() == 1, "wrong type");

  LIRItem reference(x->argument_at(0), this);
  reference.load_item();

  // need to perform the null check on the reference objecy
  CodeEmitInfo* info = NULL;
  if (x->needs_null_check()) {
    info = state_for(x);
  }

  LIR_Address* referent_field_adr =
    new LIR_Address(reference.result(), referent_offset, T_OBJECT);

  LIR_Opr result = rlock_result(x);

#if INCLUDE_ALL_GCS
  if (UseShenandoahGC) {
    LIR_Opr tmp = new_register(T_OBJECT);
    LIR_Opr addr = ShenandoahBarrierSet::barrier_set()->bsc1()->resolve_address(this, referent_field_adr, T_OBJECT, NULL);
    __ load(addr->as_address_ptr(), tmp, info);
    tmp = ShenandoahBarrierSet::barrier_set()->bsc1()->load_reference_barrier(this, tmp, addr);
    __ move(tmp, result);
  } else
#endif
  __ load(referent_field_adr, result, info);

  // Register the value in the referent field with the pre-barrier
  pre_barrier(LIR_OprFact::illegalOpr /* addr_opr */,
              result /* pre_val */,
              false  /* do_load */,
              false  /* patch */,
              NULL   /* info */);
}

// Example: clazz.isInstance(object)
void LIRGenerator::do_isInstance(Intrinsic* x) {
  assert(x->number_of_arguments() == 2, "wrong type");

  // TODO could try to substitute this node with an equivalent InstanceOf
  // if clazz is known to be a constant Class. This will pick up newly found
  // constants after HIR construction. I'll leave this to a future change.

  // as a first cut, make a simple leaf call to runtime to stay platform independent.
  // could follow the aastore example in a future change.

  LIRItem clazz(x->argument_at(0), this);
  LIRItem object(x->argument_at(1), this);
  clazz.load_item();
  object.load_item();
  LIR_Opr result = rlock_result(x);

  // need to perform null check on clazz
  if (x->needs_null_check()) {
    CodeEmitInfo* info = state_for(x);
    __ null_check(clazz.result(), info);
  }

  LIR_Opr call_result = call_runtime(clazz.value(), object.value(),
                                     CAST_FROM_FN_PTR(address, Runtime1::is_instance_of),
                                     x->type(),
                                     NULL); // NULL CodeEmitInfo results in a leaf call
  __ move(call_result, result);
}

// Example: object.getClass ()
void LIRGenerator::do_getClass(Intrinsic* x) {
  assert(x->number_of_arguments() == 1, "wrong type");

  LIRItem rcvr(x->argument_at(0), this);
  rcvr.load_item();
  LIR_Opr temp = new_register(T_METADATA);
  LIR_Opr result = rlock_result(x);

  // need to perform the null check on the rcvr
  CodeEmitInfo* info = NULL;
  if (x->needs_null_check()) {
    info = state_for(x);
  }

  // FIXME T_ADDRESS should actually be T_METADATA but it can't because the
  // meaning of these two is mixed up (see JDK-8026837).
  __ move(new LIR_Address(rcvr.result(), oopDesc::klass_offset_in_bytes(), T_ADDRESS), temp, info);
  __ move_wide(new LIR_Address(temp, in_bytes(Klass::java_mirror_offset()), T_OBJECT), result);
}

// java.lang.Class::isPrimitive()
void LIRGenerator::do_isPrimitive(Intrinsic* x) {
  assert(x->number_of_arguments() == 1, "wrong type");

  LIRItem rcvr(x->argument_at(0), this);
  rcvr.load_item();
  LIR_Opr temp = new_register(T_METADATA);
  LIR_Opr result = rlock_result(x);

  CodeEmitInfo* info = NULL;
  if (x->needs_null_check()) {
    info = state_for(x);
  }

  __ move(new LIR_Address(rcvr.result(), java_lang_Class::klass_offset_in_bytes(), T_ADDRESS), temp, info);
  __ cmp(lir_cond_notEqual, temp, LIR_OprFact::metadataConst(0));
  __ cmove(lir_cond_notEqual, LIR_OprFact::intConst(0), LIR_OprFact::intConst(1), result, T_BOOLEAN);
}

// Example: Thread.currentThread()
void LIRGenerator::do_currentThread(Intrinsic* x) {
  assert(x->number_of_arguments() == 0, "wrong type");
  LIR_Opr reg = rlock_result(x);
  __ move_wide(new LIR_Address(getThreadPointer(), in_bytes(JavaThread::threadObj_offset()), T_OBJECT), reg);
}


void LIRGenerator::do_RegisterFinalizer(Intrinsic* x) {
  assert(x->number_of_arguments() == 1, "wrong type");
  LIRItem receiver(x->argument_at(0), this);

  receiver.load_item();
  BasicTypeList signature;
  signature.append(T_OBJECT); // receiver
  LIR_OprList* args = new LIR_OprList();
  args->append(receiver.result());
  CodeEmitInfo* info = state_for(x, x->state());
  call_runtime(&signature, args,
               CAST_FROM_FN_PTR(address, Runtime1::entry_for(Runtime1::register_finalizer_id)),
               voidType, info);

  set_no_result(x);
}


//------------------------local access--------------------------------------

LIR_Opr LIRGenerator::operand_for_instruction(Instruction* x) {
  if (x->operand()->is_illegal()) {
    Constant* c = x->as_Constant();
    if (c != NULL) {
      x->set_operand(LIR_OprFact::value_type(c->type()));
    } else {
      assert(x->as_Phi() || x->as_Local() != NULL, "only for Phi and Local");
      // allocate a virtual register for this local or phi
      x->set_operand(rlock(x));
      _instruction_for_operand.at_put_grow(x->operand()->vreg_number(), x, NULL);
    }
  }
  return x->operand();
}


Instruction* LIRGenerator::instruction_for_opr(LIR_Opr opr) {
  if (opr->is_virtual()) {
    return instruction_for_vreg(opr->vreg_number());
  }
  return NULL;
}


Instruction* LIRGenerator::instruction_for_vreg(int reg_num) {
  if (reg_num < _instruction_for_operand.length()) {
    return _instruction_for_operand.at(reg_num);
  }
  return NULL;
}


void LIRGenerator::set_vreg_flag(int vreg_num, VregFlag f) {
  if (_vreg_flags.size_in_bits() == 0) {
    BitMap2D temp(100, num_vreg_flags);
    temp.clear();
    _vreg_flags = temp;
  }
  _vreg_flags.at_put_grow(vreg_num, f, true);
}

bool LIRGenerator::is_vreg_flag_set(int vreg_num, VregFlag f) {
  if (!_vreg_flags.is_valid_index(vreg_num, f)) {
    return false;
  }
  return _vreg_flags.at(vreg_num, f);
}


// Block local constant handling.  This code is useful for keeping
// unpinned constants and constants which aren't exposed in the IR in
// registers.  Unpinned Constant instructions have their operands
// cleared when the block is finished so that other blocks can't end
// up referring to their registers.

LIR_Opr LIRGenerator::load_constant(Constant* x) {
  assert(!x->is_pinned(), "only for unpinned constants");
  _unpinned_constants.append(x);
  return load_constant(LIR_OprFact::value_type(x->type())->as_constant_ptr());
}


LIR_Opr LIRGenerator::load_constant(LIR_Const* c) {
  BasicType t = c->type();
  for (int i = 0; i < _constants.length(); i++) {
    LIR_Const* other = _constants.at(i);
    if (t == other->type()) {
      switch (t) {
      case T_INT:
      case T_FLOAT:
        if (c->as_jint_bits() != other->as_jint_bits()) continue;
        break;
      case T_LONG:
      case T_DOUBLE:
        if (c->as_jint_hi_bits() != other->as_jint_hi_bits()) continue;
        if (c->as_jint_lo_bits() != other->as_jint_lo_bits()) continue;
        break;
      case T_OBJECT:
        if (c->as_jobject() != other->as_jobject()) continue;
        break;
      }
      return _reg_for_constants.at(i);
    }
  }

  LIR_Opr result = new_register(t);
  __ move((LIR_Opr)c, result);
  _constants.append(c);
  _reg_for_constants.append(result);
  return result;
}

// Various barriers

void LIRGenerator::pre_barrier(LIR_Opr addr_opr, LIR_Opr pre_val,
                               bool do_load, bool patch, CodeEmitInfo* info) {
  // Do the pre-write barrier, if any.
  switch (_bs->kind()) {
#if INCLUDE_ALL_GCS
    case BarrierSet::G1SATBCT:
    case BarrierSet::G1SATBCTLogging:
      G1SATBCardTableModRef_pre_barrier(addr_opr, pre_val, do_load, patch, info);
      break;
    case BarrierSet::ShenandoahBarrierSet:
      if (ShenandoahSATBBarrier) {
        G1SATBCardTableModRef_pre_barrier(addr_opr, pre_val, do_load, patch, info);
      }
      break;
#endif // INCLUDE_ALL_GCS
    case BarrierSet::CardTableModRef:
    case BarrierSet::CardTableExtension:
      // No pre barriers
      break;
    case BarrierSet::ModRef:
    case BarrierSet::Other:
      // No pre barriers
      break;
    default      :
      ShouldNotReachHere();

  }
}

void LIRGenerator::post_barrier(LIR_OprDesc* addr, LIR_OprDesc* new_val) {
  switch (_bs->kind()) {
#if INCLUDE_ALL_GCS
    case BarrierSet::G1SATBCT:
    case BarrierSet::G1SATBCTLogging:
      G1SATBCardTableModRef_post_barrier(addr,  new_val);
      break;
    case BarrierSet::ShenandoahBarrierSet:
      ShenandoahBarrierSetC1::bsc1()->storeval_barrier(this, new_val, NULL, false);
      break;
#endif // INCLUDE_ALL_GCS
    case BarrierSet::CardTableModRef:
    case BarrierSet::CardTableExtension:
      CardTableModRef_post_barrier(addr,  new_val);
      break;
    case BarrierSet::ModRef:
    case BarrierSet::Other:
      // No post barriers
      break;
    default      :
      ShouldNotReachHere();
    }
}

////////////////////////////////////////////////////////////////////////
#if INCLUDE_ALL_GCS

void LIRGenerator::G1SATBCardTableModRef_pre_barrier(LIR_Opr addr_opr, LIR_Opr pre_val,
                                                     bool do_load, bool patch, CodeEmitInfo* info) {
  // First we test whether marking is in progress.
  BasicType flag_type;
  if (in_bytes(PtrQueue::byte_width_of_active()) == 4) {
    flag_type = T_INT;
  } else {
    guarantee(in_bytes(PtrQueue::byte_width_of_active()) == 1,
              "Assumption");
    flag_type = T_BYTE;
  }
  LIR_Opr thrd = getThreadPointer();
  LIR_Address* mark_active_flag_addr =
    new LIR_Address(thrd,
                    in_bytes(JavaThread::satb_mark_queue_offset() +
                             PtrQueue::byte_offset_of_active()),
                    flag_type);
  // Read the marking-in-progress flag.
  LIR_Opr flag_val = new_register(T_INT);
  __ load(mark_active_flag_addr, flag_val);
  __ cmp(lir_cond_notEqual, flag_val, LIR_OprFact::intConst(0));

  LIR_PatchCode pre_val_patch_code = lir_patch_none;

  CodeStub* slow;

  if (do_load) {
    assert(pre_val == LIR_OprFact::illegalOpr, "sanity");
    assert(addr_opr != LIR_OprFact::illegalOpr, "sanity");

    if (patch)
      pre_val_patch_code = lir_patch_normal;

    pre_val = new_register(T_OBJECT);

    if (!addr_opr->is_address()) {
      assert(addr_opr->is_register(), "must be");
      addr_opr = LIR_OprFact::address(new LIR_Address(addr_opr, T_OBJECT));
    }
    slow = new G1PreBarrierStub(addr_opr, pre_val, pre_val_patch_code, info);
  } else {
    assert(addr_opr == LIR_OprFact::illegalOpr, "sanity");
    assert(pre_val->is_register(), "must be");
    assert(pre_val->type() == T_OBJECT, "must be an object");
    assert(info == NULL, "sanity");

    slow = new G1PreBarrierStub(pre_val);
  }

  __ branch(lir_cond_notEqual, T_INT, slow);
  __ branch_destination(slow->continuation());
}

void LIRGenerator::G1SATBCardTableModRef_post_barrier(LIR_OprDesc* addr, LIR_OprDesc* new_val) {
  // If the "new_val" is a constant NULL, no barrier is necessary.
  if (new_val->is_constant() &&
      new_val->as_constant_ptr()->as_jobject() == NULL) return;

  if (!new_val->is_register()) {
    LIR_Opr new_val_reg = new_register(T_OBJECT);
    if (new_val->is_constant()) {
      __ move(new_val, new_val_reg);
    } else {
      __ leal(new_val, new_val_reg);
    }
    new_val = new_val_reg;
  }
  assert(new_val->is_register(), "must be a register at this point");

  if (addr->is_address()) {
    LIR_Address* address = addr->as_address_ptr();
    LIR_Opr ptr = new_pointer_register();
    if (!address->index()->is_valid() && address->disp() == 0) {
      __ move(address->base(), ptr);
    } else {
      assert(address->disp() != max_jint, "lea doesn't support patched addresses!");
      __ leal(addr, ptr);
    }
    addr = ptr;
  }
  assert(addr->is_register(), "must be a register at this point");

  LIR_Opr xor_res = new_pointer_register();
  LIR_Opr xor_shift_res = new_pointer_register();
  if (TwoOperandLIRForm ) {
    __ move(addr, xor_res);
    __ logical_xor(xor_res, new_val, xor_res);
    __ move(xor_res, xor_shift_res);
    __ unsigned_shift_right(xor_shift_res,
                            LIR_OprFact::intConst(HeapRegion::LogOfHRGrainBytes),
                            xor_shift_res,
                            LIR_OprDesc::illegalOpr());
  } else {
    __ logical_xor(addr, new_val, xor_res);
    __ unsigned_shift_right(xor_res,
                            LIR_OprFact::intConst(HeapRegion::LogOfHRGrainBytes),
                            xor_shift_res,
                            LIR_OprDesc::illegalOpr());
  }

  if (!new_val->is_register()) {
    LIR_Opr new_val_reg = new_register(T_OBJECT);
    __ leal(new_val, new_val_reg);
    new_val = new_val_reg;
  }
  assert(new_val->is_register(), "must be a register at this point");

  __ cmp(lir_cond_notEqual, xor_shift_res, LIR_OprFact::intptrConst(NULL_WORD));

  CodeStub* slow = new G1PostBarrierStub(addr, new_val);
  __ branch(lir_cond_notEqual, LP64_ONLY(T_LONG) NOT_LP64(T_INT), slow);
  __ branch_destination(slow->continuation());
}

#endif // INCLUDE_ALL_GCS
////////////////////////////////////////////////////////////////////////

void LIRGenerator::CardTableModRef_post_barrier(LIR_OprDesc* addr, LIR_OprDesc* new_val) {

  assert(sizeof(*((CardTableModRefBS*)_bs)->byte_map_base) == sizeof(jbyte), "adjust this code");
  LIR_Const* card_table_base = new LIR_Const(((CardTableModRefBS*)_bs)->byte_map_base);
  if (addr->is_address()) {
    LIR_Address* address = addr->as_address_ptr();
    // ptr cannot be an object because we use this barrier for array card marks
    // and addr can point in the middle of an array.
    LIR_Opr ptr = new_pointer_register();
    if (!address->index()->is_valid() && address->disp() == 0) {
      __ move(address->base(), ptr);
    } else {
      assert(address->disp() != max_jint, "lea doesn't support patched addresses!");
      __ leal(addr, ptr);
    }
    addr = ptr;
  }
  assert(addr->is_register(), "must be a register at this point");

#ifdef CARDTABLEMODREF_POST_BARRIER_HELPER
  CardTableModRef_post_barrier_helper(addr, card_table_base);
#else
  LIR_Opr tmp = new_pointer_register();
  if (TwoOperandLIRForm) {
    __ move(addr, tmp);
    __ unsigned_shift_right(tmp, CardTableModRefBS::card_shift, tmp);
  } else {
    __ unsigned_shift_right(addr, CardTableModRefBS::card_shift, tmp);
  }

  if (UseConcMarkSweepGC && CMSPrecleaningEnabled) {
    __ membar_storestore();
  }

  if (can_inline_as_constant(card_table_base)) {
    __ move(LIR_OprFact::intConst(0),
              new LIR_Address(tmp, card_table_base->as_jint(), T_BYTE));
  } else {
    __ move(LIR_OprFact::intConst(0),
              new LIR_Address(tmp, load_constant(card_table_base),
                              T_BYTE));
  }
#endif
}


//------------------------field access--------------------------------------

// Comment copied form templateTable_i486.cpp
// ----------------------------------------------------------------------------
// Volatile variables demand their effects be made known to all CPU's in
// order.  Store buffers on most chips allow reads & writes to reorder; the
// JMM's ReadAfterWrite.java test fails in -Xint mode without some kind of
// memory barrier (i.e., it's not sufficient that the interpreter does not
// reorder volatile references, the hardware also must not reorder them).
//
// According to the new Java Memory Model (JMM):
// (1) All volatiles are serialized wrt to each other.
// ALSO reads & writes act as aquire & release, so:
// (2) A read cannot let unrelated NON-volatile memory refs that happen after
// the read float up to before the read.  It's OK for non-volatile memory refs
// that happen before the volatile read to float down below it.
// (3) Similar a volatile write cannot let unrelated NON-volatile memory refs
// that happen BEFORE the write float down to after the write.  It's OK for
// non-volatile memory refs that happen after the volatile write to float up
// before it.
//
// We only put in barriers around volatile refs (they are expensive), not
// _between_ memory refs (that would require us to track the flavor of the
// previous memory refs).  Requirements (2) and (3) require some barriers
// before volatile stores and after volatile loads.  These nearly cover
// requirement (1) but miss the volatile-store-volatile-load case.  This final
// case is placed after volatile-stores although it could just as well go
// before volatile-loads.


void LIRGenerator::do_StoreField(StoreField* x) {
  bool needs_patching = x->needs_patching();
  bool is_volatile = x->field()->is_volatile();
  BasicType field_type = x->field_type();
  bool is_oop = (field_type == T_ARRAY || field_type == T_OBJECT);

  CodeEmitInfo* info = NULL;
  if (needs_patching) {
    assert(x->explicit_null_check() == NULL, "can't fold null check into patching field access");
    info = state_for(x, x->state_before());
  } else if (x->needs_null_check()) {
    NullCheck* nc = x->explicit_null_check();
    if (nc == NULL) {
      info = state_for(x);
    } else {
      info = state_for(nc);
    }
  }


  LIRItem object(x->obj(), this);
  LIRItem value(x->value(),  this);

  object.load_item();

  if (is_volatile || needs_patching) {
    // load item if field is volatile (fewer special cases for volatiles)
    // load item if field not initialized
    // load item if field not constant
    // because of code patching we cannot inline constants
    if (field_type == T_BYTE || field_type == T_BOOLEAN) {
      value.load_byte_item();
    } else  {
      value.load_item();
    }
  } else {
    value.load_for_store(field_type);
  }

  set_no_result(x);

#ifndef PRODUCT
  if (PrintNotLoaded && needs_patching) {
    tty->print_cr("   ###class not loaded at store_%s bci %d",
                  x->is_static() ?  "static" : "field", x->printable_bci());
  }
#endif

  if (x->needs_null_check() &&
      (needs_patching ||
       MacroAssembler::needs_explicit_null_check(x->offset()))) {
    // Emit an explicit null check because the offset is too large.
    // If the class is not loaded and the object is NULL, we need to deoptimize to throw a
    // NoClassDefFoundError in the interpreter instead of an implicit NPE from compiled code.
    __ null_check(object.result(), new CodeEmitInfo(info), /* deoptimize */ needs_patching);
  }

  LIR_Address* address;
  if (needs_patching) {
    // we need to patch the offset in the instruction so don't allow
    // generate_address to try to be smart about emitting the -1.
    // Otherwise the patching code won't know how to find the
    // instruction to patch.
    address = new LIR_Address(object.result(), PATCHED_ADDR, field_type);
  } else {
    address = generate_address(object.result(), x->offset(), field_type);
  }

  if (is_volatile && os::is_MP()) {
    __ membar_release();
  }

  if (is_oop) {
    // Do the pre-write barrier, if any.
    pre_barrier(LIR_OprFact::address(address),
                LIR_OprFact::illegalOpr /* pre_val */,
                true /* do_load*/,
                needs_patching,
                (info ? new CodeEmitInfo(info) : NULL));
  }

  if (is_volatile && !needs_patching) {
    volatile_field_store(value.result(), address, info);
  } else {
    LIR_PatchCode patch_code = needs_patching ? lir_patch_normal : lir_patch_none;
    __ store(value.result(), address, info, patch_code);
  }

  if (is_oop) {
    // Store to object so mark the card of the header
    post_barrier(object.result(), value.result());
  }

  if (is_volatile && os::is_MP()) {
    __ membar();
  }
}


void LIRGenerator::do_LoadField(LoadField* x) {
  bool needs_patching = x->needs_patching();
  bool is_volatile = x->field()->is_volatile();
  BasicType field_type = x->field_type();

  CodeEmitInfo* info = NULL;
  if (needs_patching) {
    assert(x->explicit_null_check() == NULL, "can't fold null check into patching field access");
    info = state_for(x, x->state_before());
  } else if (x->needs_null_check()) {
    NullCheck* nc = x->explicit_null_check();
    if (nc == NULL) {
      info = state_for(x);
    } else {
      info = state_for(nc);
    }
  }

  LIRItem object(x->obj(), this);

  object.load_item();

#ifndef PRODUCT
  if (PrintNotLoaded && needs_patching) {
    tty->print_cr("   ###class not loaded at load_%s bci %d",
                  x->is_static() ?  "static" : "field", x->printable_bci());
  }
#endif

  bool stress_deopt = StressLoopInvariantCodeMotion && info && info->deoptimize_on_exception();
  if (x->needs_null_check() &&
      (needs_patching ||
       MacroAssembler::needs_explicit_null_check(x->offset()) ||
       stress_deopt)) {
    LIR_Opr obj = object.result();
    if (stress_deopt) {
      obj = new_register(T_OBJECT);
      __ move(LIR_OprFact::oopConst(NULL), obj);
    }
    // Emit an explicit null check because the offset is too large.
    // If the class is not loaded and the object is NULL, we need to deoptimize to throw a
    // NoClassDefFoundError in the interpreter instead of an implicit NPE from compiled code.
    __ null_check(obj, new CodeEmitInfo(info), /* deoptimize */ needs_patching);
  }

  LIR_Opr reg = rlock_result(x, field_type);
  LIR_Address* address;
  if (needs_patching) {
    // we need to patch the offset in the instruction so don't allow
    // generate_address to try to be smart about emitting the -1.
    // Otherwise the patching code won't know how to find the
    // instruction to patch.
    address = new LIR_Address(object.result(), PATCHED_ADDR, field_type);
  } else {
    address = generate_address(object.result(), x->offset(), field_type);
  }

#if INCLUDE_ALL_GCS
  if (UseShenandoahGC && (field_type == T_OBJECT || field_type == T_ARRAY)) {
    LIR_Opr tmp = new_register(T_OBJECT);
    LIR_Opr addr = ShenandoahBarrierSet::barrier_set()->bsc1()->resolve_address(this, address, field_type, needs_patching ? info : NULL);
    if (is_volatile) {
      volatile_field_load(addr->as_address_ptr(), tmp, info);
    } else {
      __ load(addr->as_address_ptr(), tmp, info);
    }
    if (is_volatile && os::is_MP()) {
      __ membar_acquire();
    }
    tmp = ShenandoahBarrierSet::barrier_set()->bsc1()->load_reference_barrier(this, tmp, addr);
    __ move(tmp, reg);
  } else
#endif
  {
  if (is_volatile && !needs_patching) {
    volatile_field_load(address, reg, info);
  } else {
    LIR_PatchCode patch_code = needs_patching ? lir_patch_normal : lir_patch_none;
    __ load(address, reg, info, patch_code);
  }
  if (is_volatile && os::is_MP()) {
    __ membar_acquire();
  }
  }
}


//------------------------java.nio.Buffer.checkIndex------------------------

// int java.nio.Buffer.checkIndex(int)
void LIRGenerator::do_NIOCheckIndex(Intrinsic* x) {
  // NOTE: by the time we are in checkIndex() we are guaranteed that
  // the buffer is non-null (because checkIndex is package-private and
  // only called from within other methods in the buffer).
  assert(x->number_of_arguments() == 2, "wrong type");
  LIRItem buf  (x->argument_at(0), this);
  LIRItem index(x->argument_at(1), this);
  buf.load_item();
  index.load_item();

  LIR_Opr result = rlock_result(x);
  if (GenerateRangeChecks) {
    CodeEmitInfo* info = state_for(x);
    CodeStub* stub = new RangeCheckStub(info, index.result(), true);
    if (index.result()->is_constant()) {
      cmp_mem_int(lir_cond_belowEqual, buf.result(), java_nio_Buffer::limit_offset(), index.result()->as_jint(), info);
      __ branch(lir_cond_belowEqual, T_INT, stub);
    } else {
      cmp_reg_mem(lir_cond_aboveEqual, index.result(), buf.result(),
                  java_nio_Buffer::limit_offset(), T_INT, info);
      __ branch(lir_cond_aboveEqual, T_INT, stub);
    }
    __ move(index.result(), result);
  } else {
    // Just load the index into the result register
    __ move(index.result(), result);
  }
}


//------------------------array access--------------------------------------


void LIRGenerator::do_ArrayLength(ArrayLength* x) {
  LIRItem array(x->array(), this);
  array.load_item();
  LIR_Opr reg = rlock_result(x);

  CodeEmitInfo* info = NULL;
  if (x->needs_null_check()) {
    NullCheck* nc = x->explicit_null_check();
    if (nc == NULL) {
      info = state_for(x);
    } else {
      info = state_for(nc);
    }
    if (StressLoopInvariantCodeMotion && info->deoptimize_on_exception()) {
      LIR_Opr obj = new_register(T_OBJECT);
      __ move(LIR_OprFact::oopConst(NULL), obj);
      __ null_check(obj, new CodeEmitInfo(info));
    }
  }
  __ load(new LIR_Address(array.result(), arrayOopDesc::length_offset_in_bytes(), T_INT), reg, info, lir_patch_none);
}


void LIRGenerator::do_LoadIndexed(LoadIndexed* x) {
  bool use_length = x->length() != NULL;
  LIRItem array(x->array(), this);
  LIRItem index(x->index(), this);
  LIRItem length(this);
  bool needs_range_check = x->compute_needs_range_check();

  if (use_length && needs_range_check) {
    length.set_instruction(x->length());
    length.load_item();
  }

  array.load_item();
  if (index.is_constant() && can_inline_as_constant(x->index())) {
    // let it be a constant
    index.dont_load_item();
  } else {
    index.load_item();
  }

  CodeEmitInfo* range_check_info = state_for(x);
  CodeEmitInfo* null_check_info = NULL;
  if (x->needs_null_check()) {
    NullCheck* nc = x->explicit_null_check();
    if (nc != NULL) {
      null_check_info = state_for(nc);
    } else {
      null_check_info = range_check_info;
    }
    if (StressLoopInvariantCodeMotion && null_check_info->deoptimize_on_exception()) {
      LIR_Opr obj = new_register(T_OBJECT);
      __ move(LIR_OprFact::oopConst(NULL), obj);
      __ null_check(obj, new CodeEmitInfo(null_check_info));
    }
  }

  // emit array address setup early so it schedules better
  LIR_Address* array_addr = emit_array_address(array.result(), index.result(), x->elt_type(), false);

  if (GenerateRangeChecks && needs_range_check) {
    if (StressLoopInvariantCodeMotion && range_check_info->deoptimize_on_exception()) {
      __ branch(lir_cond_always, T_ILLEGAL, new RangeCheckStub(range_check_info, index.result()));
    } else if (use_length) {
      // TODO: use a (modified) version of array_range_check that does not require a
      //       constant length to be loaded to a register
      __ cmp(lir_cond_belowEqual, length.result(), index.result());
      __ branch(lir_cond_belowEqual, T_INT, new RangeCheckStub(range_check_info, index.result()));
    } else {
      array_range_check(array.result(), index.result(), null_check_info, range_check_info);
      // The range check performs the null check, so clear it out for the load
      null_check_info = NULL;
    }
  }

  LIR_Opr result = rlock_result(x, x->elt_type());

#if INCLUDE_ALL_GCS
  if (UseShenandoahGC && (x->elt_type() == T_OBJECT || x->elt_type() == T_ARRAY)) {
    LIR_Opr tmp = new_register(T_OBJECT);
    LIR_Opr addr = ShenandoahBarrierSet::barrier_set()->bsc1()->resolve_address(this, array_addr, x->elt_type(), NULL);
    __ move(addr->as_address_ptr(), tmp, null_check_info);
    tmp = ShenandoahBarrierSet::barrier_set()->bsc1()->load_reference_barrier(this, tmp, addr);
    __ move(tmp, result);
  } else
#endif
  __ move(array_addr, result, null_check_info);

}


void LIRGenerator::do_NullCheck(NullCheck* x) {
  if (x->can_trap()) {
    LIRItem value(x->obj(), this);
    value.load_item();
    CodeEmitInfo* info = state_for(x);
    __ null_check(value.result(), info);
  }
}


void LIRGenerator::do_TypeCast(TypeCast* x) {
  LIRItem value(x->obj(), this);
  value.load_item();
  // the result is the same as from the node we are casting
  set_result(x, value.result());
}


void LIRGenerator::do_Throw(Throw* x) {
  LIRItem exception(x->exception(), this);
  exception.load_item();
  set_no_result(x);
  LIR_Opr exception_opr = exception.result();
  CodeEmitInfo* info = state_for(x, x->state());

#ifndef PRODUCT
  if (PrintC1Statistics) {
    increment_counter(Runtime1::throw_count_address(), T_INT);
  }
#endif

  // check if the instruction has an xhandler in any of the nested scopes
  bool unwind = false;
  if (info->exception_handlers()->length() == 0) {
    // this throw is not inside an xhandler
    unwind = true;
  } else {
    // get some idea of the throw type
    bool type_is_exact = true;
    ciType* throw_type = x->exception()->exact_type();
    if (throw_type == NULL) {
      type_is_exact = false;
      throw_type = x->exception()->declared_type();
    }
    if (throw_type != NULL && throw_type->is_instance_klass()) {
      ciInstanceKlass* throw_klass = (ciInstanceKlass*)throw_type;
      unwind = !x->exception_handlers()->could_catch(throw_klass, type_is_exact);
    }
  }

  // do null check before moving exception oop into fixed register
  // to avoid a fixed interval with an oop during the null check.
  // Use a copy of the CodeEmitInfo because debug information is
  // different for null_check and throw.
  if (GenerateCompilerNullChecks &&
      (x->exception()->as_NewInstance() == NULL && x->exception()->as_ExceptionObject() == NULL)) {
    // if the exception object wasn't created using new then it might be null.
    __ null_check(exception_opr, new CodeEmitInfo(info, x->state()->copy(ValueStack::ExceptionState, x->state()->bci())));
  }

  if (compilation()->env()->jvmti_can_post_on_exceptions()) {
    // we need to go through the exception lookup path to get JVMTI
    // notification done
    unwind = false;
  }

  // move exception oop into fixed register
  __ move(exception_opr, exceptionOopOpr());

  if (unwind) {
    __ unwind_exception(exceptionOopOpr());
  } else {
    __ throw_exception(exceptionPcOpr(), exceptionOopOpr(), info);
  }
}


void LIRGenerator::do_RoundFP(RoundFP* x) {
  LIRItem input(x->input(), this);
  input.load_item();
  LIR_Opr input_opr = input.result();
  assert(input_opr->is_register(), "why round if value is not in a register?");
  assert(input_opr->is_single_fpu() || input_opr->is_double_fpu(), "input should be floating-point value");
  if (input_opr->is_single_fpu()) {
    set_result(x, round_item(input_opr)); // This code path not currently taken
  } else {
    LIR_Opr result = new_register(T_DOUBLE);
    set_vreg_flag(result, must_start_in_memory);
    __ roundfp(input_opr, LIR_OprFact::illegalOpr, result);
    set_result(x, result);
  }
}

// Here UnsafeGetRaw may have x->base() and x->index() be int or long
// on both 64 and 32 bits. Expecting x->base() to be always long on 64bit.
void LIRGenerator::do_UnsafeGetRaw(UnsafeGetRaw* x) {
  LIRItem base(x->base(), this);
  LIRItem idx(this);

  base.load_item();
  if (x->has_index()) {
    idx.set_instruction(x->index());
    idx.load_nonconstant();
  }

  LIR_Opr reg = rlock_result(x, x->basic_type());

  int   log2_scale = 0;
  if (x->has_index()) {
    log2_scale = x->log2_scale();
  }

  assert(!x->has_index() || idx.value() == x->index(), "should match");

  LIR_Opr base_op = base.result();
  LIR_Opr index_op = idx.result();
#ifndef _LP64
  if (base_op->type() == T_LONG) {
    base_op = new_register(T_INT);
    __ convert(Bytecodes::_l2i, base.result(), base_op);
  }
  if (x->has_index()) {
    if (index_op->type() == T_LONG) {
      LIR_Opr long_index_op = index_op;
      if (index_op->is_constant()) {
        long_index_op = new_register(T_LONG);
        __ move(index_op, long_index_op);
      }
      index_op = new_register(T_INT);
      __ convert(Bytecodes::_l2i, long_index_op, index_op);
    } else {
      assert(x->index()->type()->tag() == intTag, "must be");
    }
  }
  // At this point base and index should be all ints.
  assert(base_op->type() == T_INT && !base_op->is_constant(), "base should be an non-constant int");
  assert(!x->has_index() || index_op->type() == T_INT, "index should be an int");
#else
  if (x->has_index()) {
    if (index_op->type() == T_INT) {
      if (!index_op->is_constant()) {
        index_op = new_register(T_LONG);
        __ convert(Bytecodes::_i2l, idx.result(), index_op);
      }
    } else {
      assert(index_op->type() == T_LONG, "must be");
      if (index_op->is_constant()) {
        index_op = new_register(T_LONG);
        __ move(idx.result(), index_op);
      }
    }
  }
  // At this point base is a long non-constant
  // Index is a long register or a int constant.
  // We allow the constant to stay an int because that would allow us a more compact encoding by
  // embedding an immediate offset in the address expression. If we have a long constant, we have to
  // move it into a register first.
  assert(base_op->type() == T_LONG && !base_op->is_constant(), "base must be a long non-constant");
  assert(!x->has_index() || (index_op->type() == T_INT && index_op->is_constant()) ||
                            (index_op->type() == T_LONG && !index_op->is_constant()), "unexpected index type");
#endif

  BasicType dst_type = x->basic_type();

  LIR_Address* addr;
  if (index_op->is_constant()) {
    assert(log2_scale == 0, "must not have a scale");
    assert(index_op->type() == T_INT, "only int constants supported");
    addr = new LIR_Address(base_op, index_op->as_jint(), dst_type);
  } else {
#if defined(X86) || defined(AARCH64)
    addr = new LIR_Address(base_op, index_op, LIR_Address::Scale(log2_scale), 0, dst_type);
#elif defined(GENERATE_ADDRESS_IS_PREFERRED)
    addr = generate_address(base_op, index_op, log2_scale, 0, dst_type);
#else
    if (index_op->is_illegal() || log2_scale == 0) {
      addr = new LIR_Address(base_op, index_op, dst_type);
    } else {
      LIR_Opr tmp = new_pointer_register();
      __ shift_left(index_op, log2_scale, tmp);
      addr = new LIR_Address(base_op, tmp, dst_type);
    }
#endif
  }

  if (x->may_be_unaligned() && (dst_type == T_LONG || dst_type == T_DOUBLE)) {
    __ unaligned_move(addr, reg);
  } else {
    if (dst_type == T_OBJECT && x->is_wide()) {
      __ move_wide(addr, reg);
    } else {
      __ move(addr, reg);
    }
  }
}


void LIRGenerator::do_UnsafePutRaw(UnsafePutRaw* x) {
  int  log2_scale = 0;
  BasicType type = x->basic_type();

  if (x->has_index()) {
    log2_scale = x->log2_scale();
  }

  LIRItem base(x->base(), this);
  LIRItem value(x->value(), this);
  LIRItem idx(this);

  base.load_item();
  if (x->has_index()) {
    idx.set_instruction(x->index());
    idx.load_item();
  }

  if (type == T_BYTE || type == T_BOOLEAN) {
    value.load_byte_item();
  } else {
    value.load_item();
  }

  set_no_result(x);

  LIR_Opr base_op = base.result();
  LIR_Opr index_op = idx.result();

#ifdef GENERATE_ADDRESS_IS_PREFERRED
  LIR_Address* addr = generate_address(base_op, index_op, log2_scale, 0, x->basic_type());
#else
#ifndef _LP64
  if (base_op->type() == T_LONG) {
    base_op = new_register(T_INT);
    __ convert(Bytecodes::_l2i, base.result(), base_op);
  }
  if (x->has_index()) {
    if (index_op->type() == T_LONG) {
      index_op = new_register(T_INT);
      __ convert(Bytecodes::_l2i, idx.result(), index_op);
    }
  }
  // At this point base and index should be all ints and not constants
  assert(base_op->type() == T_INT && !base_op->is_constant(), "base should be an non-constant int");
  assert(!x->has_index() || (index_op->type() == T_INT && !index_op->is_constant()), "index should be an non-constant int");
#else
  if (x->has_index()) {
    if (index_op->type() == T_INT) {
      index_op = new_register(T_LONG);
      __ convert(Bytecodes::_i2l, idx.result(), index_op);
    }
  }
  // At this point base and index are long and non-constant
  assert(base_op->type() == T_LONG && !base_op->is_constant(), "base must be a non-constant long");
  assert(!x->has_index() || (index_op->type() == T_LONG && !index_op->is_constant()), "index must be a non-constant long");
#endif

  if (log2_scale != 0) {
    // temporary fix (platform dependent code without shift on Intel would be better)
    // TODO: ARM also allows embedded shift in the address
    LIR_Opr tmp = new_pointer_register();
    if (TwoOperandLIRForm) {
      __ move(index_op, tmp);
      index_op = tmp;
    }
    __ shift_left(index_op, log2_scale, tmp);
    if (!TwoOperandLIRForm) {
      index_op = tmp;
    }
  }

  LIR_Address* addr = new LIR_Address(base_op, index_op, x->basic_type());
#endif // !GENERATE_ADDRESS_IS_PREFERRED
  __ move(value.result(), addr);
}


void LIRGenerator::do_UnsafeGetObject(UnsafeGetObject* x) {
  BasicType type = x->basic_type();
  LIRItem src(x->object(), this);
  LIRItem off(x->offset(), this);

  off.load_item();
  src.load_item();

  LIR_Opr value = rlock_result(x, x->basic_type());

#if INCLUDE_ALL_GCS
  if (UseShenandoahGC && (type == T_OBJECT || type == T_ARRAY)) {
    LIR_Opr tmp = new_register(T_OBJECT);
    get_Object_unsafe(tmp, src.result(), off.result(), type, x->is_volatile());
    tmp = ShenandoahBarrierSet::barrier_set()->bsc1()->load_reference_barrier(this, tmp, LIR_OprFact::addressConst(0));
    __ move(tmp, value);
  } else
#endif
  get_Object_unsafe(value, src.result(), off.result(), type, x->is_volatile());

#if INCLUDE_ALL_GCS
  // We might be reading the value of the referent field of a
  // Reference object in order to attach it back to the live
  // object graph. If G1 is enabled then we need to record
  // the value that is being returned in an SATB log buffer.
  //
  // We need to generate code similar to the following...
  //
  // if (offset == java_lang_ref_Reference::referent_offset) {
  //   if (src != NULL) {
  //     if (klass(src)->reference_type() != REF_NONE) {
  //       pre_barrier(..., value, ...);
  //     }
  //   }
  // }

  if ((UseShenandoahGC || UseG1GC) && type == T_OBJECT) {
    bool gen_pre_barrier = true;     // Assume we need to generate pre_barrier.
    bool gen_offset_check = true;    // Assume we need to generate the offset guard.
    bool gen_source_check = true;    // Assume we need to check the src object for null.
    bool gen_type_check = true;      // Assume we need to check the reference_type.

    if (off.is_constant()) {
      jlong off_con = (off.type()->is_int() ?
                        (jlong) off.get_jint_constant() :
                        off.get_jlong_constant());


      if (off_con != (jlong) java_lang_ref_Reference::referent_offset) {
        // The constant offset is something other than referent_offset.
        // We can skip generating/checking the remaining guards and
        // skip generation of the code stub.
        gen_pre_barrier = false;
      } else {
        // The constant offset is the same as referent_offset -
        // we do not need to generate a runtime offset check.
        gen_offset_check = false;
      }
    }

    // We don't need to generate stub if the source object is an array
    if (gen_pre_barrier && src.type()->is_array()) {
      gen_pre_barrier = false;
    }

    if (gen_pre_barrier) {
      // We still need to continue with the checks.
      if (src.is_constant()) {
        ciObject* src_con = src.get_jobject_constant();
        guarantee(src_con != NULL, "no source constant");

        if (src_con->is_null_object()) {
          // The constant src object is null - We can skip
          // generating the code stub.
          gen_pre_barrier = false;
        } else {
          // Non-null constant source object. We still have to generate
          // the slow stub - but we don't need to generate the runtime
          // null object check.
          gen_source_check = false;
        }
      }
    }
    if (gen_pre_barrier && !PatchALot) {
      // Can the klass of object be statically determined to be
      // a sub-class of Reference?
      ciType* type = src.value()->declared_type();
      if ((type != NULL) && type->is_loaded()) {
        if (type->is_subtype_of(compilation()->env()->Reference_klass())) {
          gen_type_check = false;
        } else if (type->is_klass() &&
                   !compilation()->env()->Object_klass()->is_subtype_of(type->as_klass())) {
          // Not Reference and not Object klass.
          gen_pre_barrier = false;
        }
      }
    }

    if (gen_pre_barrier) {
      LabelObj* Lcont = new LabelObj();

      // We can have generate one runtime check here. Let's start with
      // the offset check.
      // Allocate temp register to src and load it here, otherwise
      // control flow below may confuse register allocator.
      LIR_Opr src_reg = new_register(T_OBJECT);
      __ move(src.result(), src_reg);
      if (gen_offset_check) {
        // if (offset != referent_offset) -> continue
        // If offset is an int then we can do the comparison with the
        // referent_offset constant; otherwise we need to move
        // referent_offset into a temporary register and generate
        // a reg-reg compare.

        LIR_Opr referent_off;

        if (off.type()->is_int()) {
          referent_off = LIR_OprFact::intConst(java_lang_ref_Reference::referent_offset);
        } else {
          assert(off.type()->is_long(), "what else?");
          referent_off = new_register(T_LONG);
          __ move(LIR_OprFact::longConst(java_lang_ref_Reference::referent_offset), referent_off);
        }
        __ cmp(lir_cond_notEqual, off.result(), referent_off);
        __ branch(lir_cond_notEqual, as_BasicType(off.type()), Lcont->label());
      }
      if (gen_source_check) {
        // offset is a const and equals referent offset
        // if (source == null) -> continue
        __ cmp(lir_cond_equal, src_reg, LIR_OprFact::oopConst(NULL));
        __ branch(lir_cond_equal, T_OBJECT, Lcont->label());
      }
      LIR_Opr src_klass = new_register(T_METADATA);
      if (gen_type_check) {
        // We have determined that offset == referent_offset && src != null.
        // if (src->_klass->_reference_type == REF_NONE) -> continue
        __ move(new LIR_Address(src_reg, oopDesc::klass_offset_in_bytes(), T_ADDRESS), src_klass);
        LIR_Address* reference_type_addr = new LIR_Address(src_klass, in_bytes(InstanceKlass::reference_type_offset()), T_BYTE);
        LIR_Opr reference_type = new_register(T_INT);
        __ move(reference_type_addr, reference_type);
        __ cmp(lir_cond_equal, reference_type, LIR_OprFact::intConst(REF_NONE));
        __ branch(lir_cond_equal, T_INT, Lcont->label());
      }
      {
        // We have determined that src->_klass->_reference_type != REF_NONE
        // so register the value in the referent field with the pre-barrier.
        pre_barrier(LIR_OprFact::illegalOpr /* addr_opr */,
                    value  /* pre_val */,
                    false  /* do_load */,
                    false  /* patch */,
                    NULL   /* info */);
      }
      __ branch_destination(Lcont->label());
    }
  }
#endif // INCLUDE_ALL_GCS

  if (x->is_volatile() && os::is_MP()) __ membar_acquire();
}


void LIRGenerator::do_UnsafePutObject(UnsafePutObject* x) {
  BasicType type = x->basic_type();
  LIRItem src(x->object(), this);
  LIRItem off(x->offset(), this);
  LIRItem data(x->value(), this);

  src.load_item();
  if (type == T_BOOLEAN || type == T_BYTE) {
    data.load_byte_item();
  } else {
    data.load_item();
  }
  off.load_item();

  set_no_result(x);

  if (x->is_volatile() && os::is_MP()) __ membar_release();
  put_Object_unsafe(src.result(), off.result(), data.result(), type, x->is_volatile());
  if (x->is_volatile() && os::is_MP()) __ membar();
}


void LIRGenerator::do_UnsafePrefetch(UnsafePrefetch* x, bool is_store) {
  LIRItem src(x->object(), this);
  LIRItem off(x->offset(), this);

  src.load_item();
  if (off.is_constant() && can_inline_as_constant(x->offset())) {
    // let it be a constant
    off.dont_load_item();
  } else {
    off.load_item();
  }

  set_no_result(x);

  LIR_Address* addr = generate_address(src.result(), off.result(), 0, 0, T_BYTE);
  __ prefetch(addr, is_store);
}


void LIRGenerator::do_UnsafePrefetchRead(UnsafePrefetchRead* x) {
  do_UnsafePrefetch(x, false);
}


void LIRGenerator::do_UnsafePrefetchWrite(UnsafePrefetchWrite* x) {
  do_UnsafePrefetch(x, true);
}


void LIRGenerator::do_SwitchRanges(SwitchRangeArray* x, LIR_Opr value, BlockBegin* default_sux) {
  int lng = x->length();

  for (int i = 0; i < lng; i++) {
    SwitchRange* one_range = x->at(i);
    int low_key = one_range->low_key();
    int high_key = one_range->high_key();
    BlockBegin* dest = one_range->sux();
    if (low_key == high_key) {
      __ cmp(lir_cond_equal, value, low_key);
      __ branch(lir_cond_equal, T_INT, dest);
    } else if (high_key - low_key == 1) {
      __ cmp(lir_cond_equal, value, low_key);
      __ branch(lir_cond_equal, T_INT, dest);
      __ cmp(lir_cond_equal, value, high_key);
      __ branch(lir_cond_equal, T_INT, dest);
    } else {
      LabelObj* L = new LabelObj();
      __ cmp(lir_cond_less, value, low_key);
      __ branch(lir_cond_less, T_INT, L->label());
      __ cmp(lir_cond_lessEqual, value, high_key);
      __ branch(lir_cond_lessEqual, T_INT, dest);
      __ branch_destination(L->label());
    }
  }
  __ jump(default_sux);
}


SwitchRangeArray* LIRGenerator::create_lookup_ranges(TableSwitch* x) {
  SwitchRangeList* res = new SwitchRangeList();
  int len = x->length();
  if (len > 0) {
    BlockBegin* sux = x->sux_at(0);
    int key = x->lo_key();
    BlockBegin* default_sux = x->default_sux();
    SwitchRange* range = new SwitchRange(key, sux);
    for (int i = 0; i < len; i++, key++) {
      BlockBegin* new_sux = x->sux_at(i);
      if (sux == new_sux) {
        // still in same range
        range->set_high_key(key);
      } else {
        // skip tests which explicitly dispatch to the default
        if (sux != default_sux) {
          res->append(range);
        }
        range = new SwitchRange(key, new_sux);
      }
      sux = new_sux;
    }
    if (res->length() == 0 || res->last() != range)  res->append(range);
  }
  return res;
}


// we expect the keys to be sorted by increasing value
SwitchRangeArray* LIRGenerator::create_lookup_ranges(LookupSwitch* x) {
  SwitchRangeList* res = new SwitchRangeList();
  int len = x->length();
  if (len > 0) {
    BlockBegin* default_sux = x->default_sux();
    int key = x->key_at(0);
    BlockBegin* sux = x->sux_at(0);
    SwitchRange* range = new SwitchRange(key, sux);
    for (int i = 1; i < len; i++) {
      int new_key = x->key_at(i);
      BlockBegin* new_sux = x->sux_at(i);
      if (key+1 == new_key && sux == new_sux) {
        // still in same range
        range->set_high_key(new_key);
      } else {
        // skip tests which explicitly dispatch to the default
        if (range->sux() != default_sux) {
          res->append(range);
        }
        range = new SwitchRange(new_key, new_sux);
      }
      key = new_key;
      sux = new_sux;
    }
    if (res->length() == 0 || res->last() != range)  res->append(range);
  }
  return res;
}


void LIRGenerator::do_TableSwitch(TableSwitch* x) {
  LIRItem tag(x->tag(), this);
  tag.load_item();
  set_no_result(x);

  if (x->is_safepoint()) {
    __ safepoint(safepoint_poll_register(), state_for(x, x->state_before()));
  }

  // move values into phi locations
  move_to_phi(x->state());

  int lo_key = x->lo_key();
  int len = x->length();
  assert(lo_key <= (lo_key + (len - 1)), "integer overflow");
  LIR_Opr value = tag.result();
  if (UseTableRanges) {
    do_SwitchRanges(create_lookup_ranges(x), value, x->default_sux());
  } else {
    for (int i = 0; i < len; i++) {
      __ cmp(lir_cond_equal, value, i + lo_key);
      __ branch(lir_cond_equal, T_INT, x->sux_at(i));
    }
    __ jump(x->default_sux());
  }
}


void LIRGenerator::do_LookupSwitch(LookupSwitch* x) {
  LIRItem tag(x->tag(), this);
  tag.load_item();
  set_no_result(x);

  if (x->is_safepoint()) {
    __ safepoint(safepoint_poll_register(), state_for(x, x->state_before()));
  }

  // move values into phi locations
  move_to_phi(x->state());

  LIR_Opr value = tag.result();
  if (UseTableRanges) {
    do_SwitchRanges(create_lookup_ranges(x), value, x->default_sux());
  } else {
    int len = x->length();
    for (int i = 0; i < len; i++) {
      __ cmp(lir_cond_equal, value, x->key_at(i));
      __ branch(lir_cond_equal, T_INT, x->sux_at(i));
    }
    __ jump(x->default_sux());
  }
}


void LIRGenerator::do_Goto(Goto* x) {
  set_no_result(x);

  if (block()->next()->as_OsrEntry()) {
    // need to free up storage used for OSR entry point
    LIR_Opr osrBuffer = block()->next()->operand();
    BasicTypeList signature;
    signature.append(NOT_LP64(T_INT) LP64_ONLY(T_LONG)); // pass a pointer to osrBuffer
    CallingConvention* cc = frame_map()->c_calling_convention(&signature);
    __ move(osrBuffer, cc->args()->at(0));
    __ call_runtime_leaf(CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_end),
                         getThreadTemp(), LIR_OprFact::illegalOpr, cc->args());
  }

  if (x->is_safepoint()) {
    ValueStack* state = x->state_before() ? x->state_before() : x->state();

    // increment backedge counter if needed
    CodeEmitInfo* info = state_for(x, state);
    increment_backedge_counter(info, x->profiled_bci());
    CodeEmitInfo* safepoint_info = state_for(x, state);
    __ safepoint(safepoint_poll_register(), safepoint_info);
  }

  // Gotos can be folded Ifs, handle this case.
  if (x->should_profile()) {
    ciMethod* method = x->profiled_method();
    assert(method != NULL, "method should be set if branch is profiled");
    ciMethodData* md = method->method_data_or_null();
    assert(md != NULL, "Sanity");
    ciProfileData* data = md->bci_to_data(x->profiled_bci());
    assert(data != NULL, "must have profiling data");
    int offset;
    if (x->direction() == Goto::taken) {
      assert(data->is_BranchData(), "need BranchData for two-way branches");
      offset = md->byte_offset_of_slot(data, BranchData::taken_offset());
    } else if (x->direction() == Goto::not_taken) {
      assert(data->is_BranchData(), "need BranchData for two-way branches");
      offset = md->byte_offset_of_slot(data, BranchData::not_taken_offset());
    } else {
      assert(data->is_JumpData(), "need JumpData for branches");
      offset = md->byte_offset_of_slot(data, JumpData::taken_offset());
    }
    LIR_Opr md_reg = new_register(T_METADATA);
    __ metadata2reg(md->constant_encoding(), md_reg);

    increment_counter(new LIR_Address(md_reg, offset,
                                      NOT_LP64(T_INT) LP64_ONLY(T_LONG)), DataLayout::counter_increment);
  }

  // emit phi-instruction move after safepoint since this simplifies
  // describing the state as the safepoint.
  move_to_phi(x->state());

  __ jump(x->default_sux());
}

/**
 * Emit profiling code if needed for arguments, parameters, return value types
 *
 * @param md                    MDO the code will update at runtime
 * @param md_base_offset        common offset in the MDO for this profile and subsequent ones
 * @param md_offset             offset in the MDO (on top of md_base_offset) for this profile
 * @param profiled_k            current profile
 * @param obj                   IR node for the object to be profiled
 * @param mdp                   register to hold the pointer inside the MDO (md + md_base_offset).
 *                              Set once we find an update to make and use for next ones.
 * @param not_null              true if we know obj cannot be null
 * @param signature_at_call_k   signature at call for obj
 * @param callee_signature_k    signature of callee for obj
 *                              at call and callee signatures differ at method handle call
 * @return                      the only klass we know will ever be seen at this profile point
 */
ciKlass* LIRGenerator::profile_type(ciMethodData* md, int md_base_offset, int md_offset, intptr_t profiled_k,
                                    Value obj, LIR_Opr& mdp, bool not_null, ciKlass* signature_at_call_k,
                                    ciKlass* callee_signature_k) {
  ciKlass* result = NULL;
  bool do_null = !not_null && !TypeEntries::was_null_seen(profiled_k);
  bool do_update = !TypeEntries::is_type_unknown(profiled_k);
  // known not to be null or null bit already set and already set to
  // unknown: nothing we can do to improve profiling
  if (!do_null && !do_update) {
    return result;
  }

  ciKlass* exact_klass = NULL;
  Compilation* comp = Compilation::current();
  if (do_update) {
    // try to find exact type, using CHA if possible, so that loading
    // the klass from the object can be avoided
    ciType* type = obj->exact_type();
    if (type == NULL) {
      type = obj->declared_type();
      type = comp->cha_exact_type(type);
    }
    assert(type == NULL || type->is_klass(), "type should be class");
    exact_klass = (type != NULL && type->is_loaded()) ? (ciKlass*)type : NULL;

    do_update = exact_klass == NULL || ciTypeEntries::valid_ciklass(profiled_k) != exact_klass;
  }

  if (!do_null && !do_update) {
    return result;
  }

  ciKlass* exact_signature_k = NULL;
  if (do_update) {
    // Is the type from the signature exact (the only one possible)?
    exact_signature_k = signature_at_call_k->exact_klass();
    if (exact_signature_k == NULL) {
      exact_signature_k = comp->cha_exact_type(signature_at_call_k);
    } else {
      result = exact_signature_k;
      // Known statically. No need to emit any code: prevent
      // LIR_Assembler::emit_profile_type() from emitting useless code
      profiled_k = ciTypeEntries::with_status(result, profiled_k);
    }
    // exact_klass and exact_signature_k can be both non NULL but
    // different if exact_klass is loaded after the ciObject for
    // exact_signature_k is created.
    if (exact_klass == NULL && exact_signature_k != NULL && exact_klass != exact_signature_k) {
      // sometimes the type of the signature is better than the best type
      // the compiler has
      exact_klass = exact_signature_k;
    }
    if (callee_signature_k != NULL &&
        callee_signature_k != signature_at_call_k) {
      ciKlass* improved_klass = callee_signature_k->exact_klass();
      if (improved_klass == NULL) {
        improved_klass = comp->cha_exact_type(callee_signature_k);
      }
      if (exact_klass == NULL && improved_klass != NULL && exact_klass != improved_klass) {
        exact_klass = exact_signature_k;
      }
    }
    do_update = exact_klass == NULL || ciTypeEntries::valid_ciklass(profiled_k) != exact_klass;
  }

  if (!do_null && !do_update) {
    return result;
  }

  if (mdp == LIR_OprFact::illegalOpr) {
    mdp = new_register(T_METADATA);
    __ metadata2reg(md->constant_encoding(), mdp);
    if (md_base_offset != 0) {
      LIR_Address* base_type_address = new LIR_Address(mdp, md_base_offset, T_ADDRESS);
      mdp = new_pointer_register();
      __ leal(LIR_OprFact::address(base_type_address), mdp);
    }
  }
  LIRItem value(obj, this);
  value.load_item();
  __ profile_type(new LIR_Address(mdp, md_offset, T_METADATA),
                  value.result(), exact_klass, profiled_k, new_pointer_register(), not_null, exact_signature_k != NULL);
  return result;
}

// profile parameters on entry to the root of the compilation
void LIRGenerator::profile_parameters(Base* x) {
  if (compilation()->profile_parameters()) {
    CallingConvention* args = compilation()->frame_map()->incoming_arguments();
    ciMethodData* md = scope()->method()->method_data_or_null();
    assert(md != NULL, "Sanity");

    if (md->parameters_type_data() != NULL) {
      ciParametersTypeData* parameters_type_data = md->parameters_type_data();
      ciTypeStackSlotEntries* parameters =  parameters_type_data->parameters();
      LIR_Opr mdp = LIR_OprFact::illegalOpr;
      for (int java_index = 0, i = 0, j = 0; j < parameters_type_data->number_of_parameters(); i++) {
        LIR_Opr src = args->at(i);
        assert(!src->is_illegal(), "check");
        BasicType t = src->type();
        if (t == T_OBJECT || t == T_ARRAY) {
          intptr_t profiled_k = parameters->type(j);
          Local* local = x->state()->local_at(java_index)->as_Local();
          ciKlass* exact = profile_type(md, md->byte_offset_of_slot(parameters_type_data, ParametersTypeData::type_offset(0)),
                                        in_bytes(ParametersTypeData::type_offset(j)) - in_bytes(ParametersTypeData::type_offset(0)),
                                        profiled_k, local, mdp, false, local->declared_type()->as_klass(), NULL);
          // If the profile is known statically set it once for all and do not emit any code
          if (exact != NULL) {
            md->set_parameter_type(j, exact);
          }
          j++;
        }
        java_index += type2size[t];
      }
    }
  }
}

void LIRGenerator::do_Base(Base* x) {
  __ std_entry(LIR_OprFact::illegalOpr);
  // Emit moves from physical registers / stack slots to virtual registers
  CallingConvention* args = compilation()->frame_map()->incoming_arguments();
  IRScope* irScope = compilation()->hir()->top_scope();
  int java_index = 0;
  for (int i = 0; i < args->length(); i++) {
    LIR_Opr src = args->at(i);
    assert(!src->is_illegal(), "check");
    BasicType t = src->type();

    // Types which are smaller than int are passed as int, so
    // correct the type which passed.
    switch (t) {
    case T_BYTE:
    case T_BOOLEAN:
    case T_SHORT:
    case T_CHAR:
      t = T_INT;
      break;
    }

    LIR_Opr dest = new_register(t);
    __ move(src, dest);

    // Assign new location to Local instruction for this local
    Local* local = x->state()->local_at(java_index)->as_Local();
    assert(local != NULL, "Locals for incoming arguments must have been created");
#ifndef __SOFTFP__
    // The java calling convention passes double as long and float as int.
    assert(as_ValueType(t)->tag() == local->type()->tag(), "check");
#endif // __SOFTFP__
    local->set_operand(dest);
    _instruction_for_operand.at_put_grow(dest->vreg_number(), local, NULL);
    java_index += type2size[t];
  }

  if (compilation()->env()->dtrace_method_probes()) {
    BasicTypeList signature;
    signature.append(LP64_ONLY(T_LONG) NOT_LP64(T_INT));    // thread
    signature.append(T_METADATA); // Method*
    LIR_OprList* args = new LIR_OprList();
    args->append(getThreadPointer());
    LIR_Opr meth = new_register(T_METADATA);
    __ metadata2reg(method()->constant_encoding(), meth);
    args->append(meth);
    call_runtime(&signature, args, CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_method_entry), voidType, NULL);
  }

  if (method()->is_synchronized()) {
    LIR_Opr obj;
    if (method()->is_static()) {
      obj = new_register(T_OBJECT);
      __ oop2reg(method()->holder()->java_mirror()->constant_encoding(), obj);
    } else {
      Local* receiver = x->state()->local_at(0)->as_Local();
      assert(receiver != NULL, "must already exist");
      obj = receiver->operand();
    }
    assert(obj->is_valid(), "must be valid");

    if (method()->is_synchronized() && GenerateSynchronizationCode) {
      LIR_Opr lock = new_register(T_INT);
      __ load_stack_address_monitor(0, lock);

      CodeEmitInfo* info = new CodeEmitInfo(scope()->start()->state()->copy(ValueStack::StateBefore, SynchronizationEntryBCI), NULL, x->check_flag(Instruction::DeoptimizeOnException));
      CodeStub* slow_path = new MonitorEnterStub(obj, lock, info);

      // receiver is guaranteed non-NULL so don't need CodeEmitInfo
      __ lock_object(syncTempOpr(), obj, lock, new_register(T_OBJECT), slow_path, NULL);
    }
  }

  // increment invocation counters if needed
  if (!method()->is_accessor()) { // Accessors do not have MDOs, so no counting.
    profile_parameters(x);
    CodeEmitInfo* info = new CodeEmitInfo(scope()->start()->state()->copy(ValueStack::StateBefore, SynchronizationEntryBCI), NULL, false);
    increment_invocation_counter(info);
  }

  // all blocks with a successor must end with an unconditional jump
  // to the successor even if they are consecutive
  __ jump(x->default_sux());
}


void LIRGenerator::do_OsrEntry(OsrEntry* x) {
  // construct our frame and model the production of incoming pointer
  // to the OSR buffer.
  __ osr_entry(LIR_Assembler::osrBufferPointer());
  LIR_Opr result = rlock_result(x);
  __ move(LIR_Assembler::osrBufferPointer(), result);
}


void LIRGenerator::invoke_load_arguments(Invoke* x, LIRItemList* args, const LIR_OprList* arg_list) {
  assert(args->length() == arg_list->length(),
         err_msg_res("args=%d, arg_list=%d", args->length(), arg_list->length()));
  for (int i = x->has_receiver() ? 1 : 0; i < args->length(); i++) {
    LIRItem* param = args->at(i);
    LIR_Opr loc = arg_list->at(i);
    if (loc->is_register()) {
      param->load_item_force(loc);
    } else {
      LIR_Address* addr = loc->as_address_ptr();
      param->load_for_store(addr->type());
      if (addr->type() == T_OBJECT) {
        __ move_wide(param->result(), addr);
      } else
        if (addr->type() == T_LONG || addr->type() == T_DOUBLE) {
          __ unaligned_move(param->result(), addr);
        } else {
          __ move(param->result(), addr);
        }
    }
  }

  if (x->has_receiver()) {
    LIRItem* receiver = args->at(0);
    LIR_Opr loc = arg_list->at(0);
    if (loc->is_register()) {
      receiver->load_item_force(loc);
    } else {
      assert(loc->is_address(), "just checking");
      receiver->load_for_store(T_OBJECT);
      __ move_wide(receiver->result(), loc->as_address_ptr());
    }
  }
}


// Visits all arguments, returns appropriate items without loading them
LIRItemList* LIRGenerator::invoke_visit_arguments(Invoke* x) {
  LIRItemList* argument_items = new LIRItemList();
  if (x->has_receiver()) {
    LIRItem* receiver = new LIRItem(x->receiver(), this);
    argument_items->append(receiver);
  }
  for (int i = 0; i < x->number_of_arguments(); i++) {
    LIRItem* param = new LIRItem(x->argument_at(i), this);
    argument_items->append(param);
  }
  return argument_items;
}


// The invoke with receiver has following phases:
//   a) traverse and load/lock receiver;
//   b) traverse all arguments -> item-array (invoke_visit_argument)
//   c) push receiver on stack
//   d) load each of the items and push on stack
//   e) unlock receiver
//   f) move receiver into receiver-register %o0
//   g) lock result registers and emit call operation
//
// Before issuing a call, we must spill-save all values on stack
// that are in caller-save register. "spill-save" moves those registers
// either in a free callee-save register or spills them if no free
// callee save register is available.
//
// The problem is where to invoke spill-save.
// - if invoked between e) and f), we may lock callee save
//   register in "spill-save" that destroys the receiver register
//   before f) is executed
// - if we rearrange f) to be earlier (by loading %o0) it
//   may destroy a value on the stack that is currently in %o0
//   and is waiting to be spilled
// - if we keep the receiver locked while doing spill-save,
//   we cannot spill it as it is spill-locked
//
void LIRGenerator::do_Invoke(Invoke* x) {
  CallingConvention* cc = frame_map()->java_calling_convention(x->signature(), true);

  LIR_OprList* arg_list = cc->args();
  LIRItemList* args = invoke_visit_arguments(x);
  LIR_Opr receiver = LIR_OprFact::illegalOpr;

  // setup result register
  LIR_Opr result_register = LIR_OprFact::illegalOpr;
  if (x->type() != voidType) {
    result_register = result_register_for(x->type());
  }

  CodeEmitInfo* info = state_for(x, x->state());

  invoke_load_arguments(x, args, arg_list);

  if (x->has_receiver()) {
    args->at(0)->load_item_force(LIR_Assembler::receiverOpr());
    receiver = args->at(0)->result();
  }

  // emit invoke code
  bool optimized = x->target_is_loaded() && x->target_is_final();
  assert(receiver->is_illegal() || receiver->is_equal(LIR_Assembler::receiverOpr()), "must match");

  // JSR 292
  // Preserve the SP over MethodHandle call sites, if needed.
  ciMethod* target = x->target();
  bool is_method_handle_invoke = (// %%% FIXME: Are both of these relevant?
                                  target->is_method_handle_intrinsic() ||
                                  target->is_compiled_lambda_form());
  if (is_method_handle_invoke) {
    info->set_is_method_handle_invoke(true);
    if(FrameMap::method_handle_invoke_SP_save_opr() != LIR_OprFact::illegalOpr) {
        __ move(FrameMap::stack_pointer(), FrameMap::method_handle_invoke_SP_save_opr());
    }
  }

  switch (x->code()) {
    case Bytecodes::_invokestatic:
      __ call_static(target, result_register,
                     SharedRuntime::get_resolve_static_call_stub(),
                     arg_list, info);
      break;
    case Bytecodes::_invokespecial:
    case Bytecodes::_invokevirtual:
    case Bytecodes::_invokeinterface:
      // for final target we still produce an inline cache, in order
      // to be able to call mixed mode
      if (x->code() == Bytecodes::_invokespecial || optimized) {
        __ call_opt_virtual(target, receiver, result_register,
                            SharedRuntime::get_resolve_opt_virtual_call_stub(),
                            arg_list, info);
      } else if (x->vtable_index() < 0) {
        __ call_icvirtual(target, receiver, result_register,
                          SharedRuntime::get_resolve_virtual_call_stub(),
                          arg_list, info);
      } else {
        int entry_offset = InstanceKlass::vtable_start_offset() + x->vtable_index() * vtableEntry::size();
        int vtable_offset = entry_offset * wordSize + vtableEntry::method_offset_in_bytes();
        __ call_virtual(target, receiver, result_register, vtable_offset, arg_list, info);
      }
      break;
    case Bytecodes::_invokedynamic: {
      __ call_dynamic(target, receiver, result_register,
                      SharedRuntime::get_resolve_static_call_stub(),
                      arg_list, info);
      break;
    }
    default:
      fatal(err_msg("unexpected bytecode: %s", Bytecodes::name(x->code())));
      break;
  }

  // JSR 292
  // Restore the SP after MethodHandle call sites, if needed.
  if (is_method_handle_invoke
      && FrameMap::method_handle_invoke_SP_save_opr() != LIR_OprFact::illegalOpr) {
    __ move(FrameMap::method_handle_invoke_SP_save_opr(), FrameMap::stack_pointer());
  }

  if (x->type()->is_float() || x->type()->is_double()) {
    // Force rounding of results from non-strictfp when in strictfp
    // scope (or when we don't know the strictness of the callee, to
    // be safe.)
    if (method()->is_strict()) {
      if (!x->target_is_loaded() || !x->target_is_strictfp()) {
        result_register = round_item(result_register);
      }
    }
  }

  if (result_register->is_valid()) {
    LIR_Opr result = rlock_result(x);
    __ move(result_register, result);
  }
}


void LIRGenerator::do_FPIntrinsics(Intrinsic* x) {
  assert(x->number_of_arguments() == 1, "wrong type");
  LIRItem value       (x->argument_at(0), this);
  LIR_Opr reg = rlock_result(x);
  value.load_item();
  LIR_Opr tmp = force_to_spill(value.result(), as_BasicType(x->type()));
  __ move(tmp, reg);
}



// Code for  :  x->x() {x->cond()} x->y() ? x->tval() : x->fval()
void LIRGenerator::do_IfOp(IfOp* x) {
#ifdef ASSERT
  {
    ValueTag xtag = x->x()->type()->tag();
    ValueTag ttag = x->tval()->type()->tag();
    assert(xtag == intTag || xtag == objectTag, "cannot handle others");
    assert(ttag == addressTag || ttag == intTag || ttag == objectTag || ttag == longTag, "cannot handle others");
    assert(ttag == x->fval()->type()->tag(), "cannot handle others");
  }
#endif

  LIRItem left(x->x(), this);
  LIRItem right(x->y(), this);
  left.load_item();
  if (can_inline_as_constant(right.value())) {
    right.dont_load_item();
  } else {
    right.load_item();
  }

  LIRItem t_val(x->tval(), this);
  LIRItem f_val(x->fval(), this);
  t_val.dont_load_item();
  f_val.dont_load_item();
  LIR_Opr reg = rlock_result(x);

  __ cmp(lir_cond(x->cond()), left.result(), right.result());
  __ cmove(lir_cond(x->cond()), t_val.result(), f_val.result(), reg, as_BasicType(x->x()->type()));
}

#ifdef JFR_HAVE_INTRINSICS
void LIRGenerator::do_ClassIDIntrinsic(Intrinsic* x) {
  CodeEmitInfo* info = state_for(x);
  CodeEmitInfo* info2 = new CodeEmitInfo(info); // Clone for the second null check

  assert(info != NULL, "must have info");
  LIRItem arg(x->argument_at(0), this);

  arg.load_item();
  LIR_Opr klass = new_register(T_METADATA);
  __ move(new LIR_Address(arg.result(), java_lang_Class::klass_offset_in_bytes(), T_ADDRESS), klass, info);
  LIR_Opr id = new_register(T_LONG);
  ByteSize offset = KLASS_TRACE_ID_OFFSET;
  LIR_Address* trace_id_addr = new LIR_Address(klass, in_bytes(offset), T_LONG);

  __ move(trace_id_addr, id);
  __ logical_or(id, LIR_OprFact::longConst(0x01l), id);
  __ store(id, trace_id_addr);

#ifdef TRACE_ID_META_BITS
  __ logical_and(id, LIR_OprFact::longConst(~TRACE_ID_META_BITS), id);
#endif
#ifdef TRACE_ID_SHIFT
  __ unsigned_shift_right(id, TRACE_ID_SHIFT, id);
#endif

  __ move(id, rlock_result(x));
}

void LIRGenerator::do_getEventWriter(Intrinsic* x) {
  LabelObj* L_end = new LabelObj();

  LIR_Address* jobj_addr = new LIR_Address(getThreadPointer(),
                                           in_bytes(THREAD_LOCAL_WRITER_OFFSET_JFR),
                                           T_OBJECT);
  LIR_Opr result = rlock_result(x);
  __ move_wide(jobj_addr, result);
  __ cmp(lir_cond_equal, result, LIR_OprFact::oopConst(NULL));
  __ branch(lir_cond_equal, T_OBJECT, L_end->label());
  __ move_wide(new LIR_Address(result, T_OBJECT), result);

  __ branch_destination(L_end->label());
}
#endif

void LIRGenerator::do_RuntimeCall(address routine, int expected_arguments, Intrinsic* x) {
    assert(x->number_of_arguments() == expected_arguments, "wrong type");
    LIR_Opr reg = result_register_for(x->type());
    __ call_runtime_leaf(routine, getThreadTemp(),
                         reg, new LIR_OprList());
    LIR_Opr result = rlock_result(x);
    __ move(reg, result);
}

#ifdef TRACE_HAVE_INTRINSICS
void LIRGenerator::do_ThreadIDIntrinsic(Intrinsic* x) {
    LIR_Opr thread = getThreadPointer();
    LIR_Opr osthread = new_pointer_register();
    __ move(new LIR_Address(thread, in_bytes(JavaThread::osthread_offset()), osthread->type()), osthread);
    size_t thread_id_size = OSThread::thread_id_size();
    if (thread_id_size == (size_t) BytesPerLong) {
      LIR_Opr id = new_register(T_LONG);
      __ move(new LIR_Address(osthread, in_bytes(OSThread::thread_id_offset()), T_LONG), id);
      __ convert(Bytecodes::_l2i, id, rlock_result(x));
    } else if (thread_id_size == (size_t) BytesPerInt) {
      __ move(new LIR_Address(osthread, in_bytes(OSThread::thread_id_offset()), T_INT), rlock_result(x));
    } else {
      ShouldNotReachHere();
    }
}

void LIRGenerator::do_ClassIDIntrinsic(Intrinsic* x) {
    CodeEmitInfo* info = state_for(x);
    CodeEmitInfo* info2 = new CodeEmitInfo(info); // Clone for the second null check
    BasicType klass_pointer_type = NOT_LP64(T_INT) LP64_ONLY(T_LONG);
    assert(info != NULL, "must have info");
    LIRItem arg(x->argument_at(1), this);
    arg.load_item();
    LIR_Opr klass = new_pointer_register();
    __ move(new LIR_Address(arg.result(), java_lang_Class::klass_offset_in_bytes(), klass_pointer_type), klass, info);
    LIR_Opr id = new_register(T_LONG);
    ByteSize offset = TRACE_ID_OFFSET;
    LIR_Address* trace_id_addr = new LIR_Address(klass, in_bytes(offset), T_LONG);
    __ move(trace_id_addr, id);
    __ logical_or(id, LIR_OprFact::longConst(0x01l), id);
    __ store(id, trace_id_addr);
    __ logical_and(id, LIR_OprFact::longConst(~0x3l), id);
    __ move(id, rlock_result(x));
}
#endif

void LIRGenerator::do_Intrinsic(Intrinsic* x) {
  switch (x->id()) {
  case vmIntrinsics::_intBitsToFloat      :
  case vmIntrinsics::_doubleToRawLongBits :
  case vmIntrinsics::_longBitsToDouble    :
  case vmIntrinsics::_floatToRawIntBits   : {
    do_FPIntrinsics(x);
    break;
  }

#ifdef JFR_HAVE_INTRINSICS
  case vmIntrinsics::_getClassId:
    do_ClassIDIntrinsic(x);
    break;
  case vmIntrinsics::_getEventWriter:
    do_getEventWriter(x);
    break;
  case vmIntrinsics::_counterTime:
    do_RuntimeCall(CAST_FROM_FN_PTR(address, JFR_TIME_FUNCTION), 0, x);
    break;
#endif

  case vmIntrinsics::_currentTimeMillis:
    do_RuntimeCall(CAST_FROM_FN_PTR(address, os::javaTimeMillis), 0, x);
    break;

  case vmIntrinsics::_nanoTime:
    do_RuntimeCall(CAST_FROM_FN_PTR(address, os::javaTimeNanos), 0, x);
    break;

  case vmIntrinsics::_Object_init:    do_RegisterFinalizer(x); break;
  case vmIntrinsics::_isInstance:     do_isInstance(x);    break;
  case vmIntrinsics::_isPrimitive:    do_isPrimitive(x);   break;
  case vmIntrinsics::_getClass:       do_getClass(x);      break;
  case vmIntrinsics::_currentThread:  do_currentThread(x); break;

  case vmIntrinsics::_dlog:           // fall through
  case vmIntrinsics::_dlog10:         // fall through
  case vmIntrinsics::_dabs:           // fall through
  case vmIntrinsics::_dsqrt:          // fall through
  case vmIntrinsics::_dtan:           // fall through
  case vmIntrinsics::_dsin :          // fall through
  case vmIntrinsics::_dcos :          // fall through
  case vmIntrinsics::_dexp :          // fall through
  case vmIntrinsics::_dpow :          do_MathIntrinsic(x); break;
  case vmIntrinsics::_arraycopy:      do_ArrayCopy(x);     break;

  // java.nio.Buffer.checkIndex
  case vmIntrinsics::_checkIndex:     do_NIOCheckIndex(x); break;

  case vmIntrinsics::_compareAndSwapObject:
    do_CompareAndSwap(x, objectType);
    break;
  case vmIntrinsics::_compareAndSwapInt:
    do_CompareAndSwap(x, intType);
    break;
  case vmIntrinsics::_compareAndSwapLong:
    do_CompareAndSwap(x, longType);
    break;

  case vmIntrinsics::_loadFence :
    if (os::is_MP()) __ membar_acquire();
    break;
  case vmIntrinsics::_storeFence:
    if (os::is_MP()) __ membar_release();
    break;
  case vmIntrinsics::_fullFence :
    if (os::is_MP()) __ membar();
    break;

  case vmIntrinsics::_Reference_get:
    do_Reference_get(x);
    break;

  case vmIntrinsics::_updateCRC32:
  case vmIntrinsics::_updateBytesCRC32:
  case vmIntrinsics::_updateByteBufferCRC32:
    do_update_CRC32(x);
    break;

  default: ShouldNotReachHere(); break;
  }
}

void LIRGenerator::profile_arguments(ProfileCall* x) {
  if (compilation()->profile_arguments()) {
    int bci = x->bci_of_invoke();
    ciMethodData* md = x->method()->method_data_or_null();
    ciProfileData* data = md->bci_to_data(bci);
    if (data != NULL) {
      if ((data->is_CallTypeData() && data->as_CallTypeData()->has_arguments()) ||
          (data->is_VirtualCallTypeData() && data->as_VirtualCallTypeData()->has_arguments())) {
        ByteSize extra = data->is_CallTypeData() ? CallTypeData::args_data_offset() : VirtualCallTypeData::args_data_offset();
        int base_offset = md->byte_offset_of_slot(data, extra);
        LIR_Opr mdp = LIR_OprFact::illegalOpr;
        ciTypeStackSlotEntries* args = data->is_CallTypeData() ? ((ciCallTypeData*)data)->args() : ((ciVirtualCallTypeData*)data)->args();

        Bytecodes::Code bc = x->method()->java_code_at_bci(bci);
        int start = 0;
        int stop = data->is_CallTypeData() ? ((ciCallTypeData*)data)->number_of_arguments() : ((ciVirtualCallTypeData*)data)->number_of_arguments();
        if (x->callee()->is_loaded() && x->callee()->is_static() && Bytecodes::has_receiver(bc)) {
          // first argument is not profiled at call (method handle invoke)
          assert(x->method()->raw_code_at_bci(bci) == Bytecodes::_invokehandle, "invokehandle expected");
          start = 1;
        }
        ciSignature* callee_signature = x->callee()->signature();
        // method handle call to virtual method
        bool has_receiver = x->callee()->is_loaded() && !x->callee()->is_static() && !Bytecodes::has_receiver(bc);
        ciSignatureStream callee_signature_stream(callee_signature, has_receiver ? x->callee()->holder() : NULL);

        bool ignored_will_link;
        ciSignature* signature_at_call = NULL;
        x->method()->get_method_at_bci(bci, ignored_will_link, &signature_at_call);
        ciSignatureStream signature_at_call_stream(signature_at_call);

        // if called through method handle invoke, some arguments may have been popped
        for (int i = 0; i < stop && i+start < x->nb_profiled_args(); i++) {
          int off = in_bytes(TypeEntriesAtCall::argument_type_offset(i)) - in_bytes(TypeEntriesAtCall::args_data_offset());
          ciKlass* exact = profile_type(md, base_offset, off,
              args->type(i), x->profiled_arg_at(i+start), mdp,
              !x->arg_needs_null_check(i+start),
              signature_at_call_stream.next_klass(), callee_signature_stream.next_klass());
          if (exact != NULL) {
            md->set_argument_type(bci, i, exact);
          }
        }
      } else {
#ifdef ASSERT
        Bytecodes::Code code = x->method()->raw_code_at_bci(x->bci_of_invoke());
        int n = x->nb_profiled_args();
        assert(MethodData::profile_parameters() && (MethodData::profile_arguments_jsr292_only() ||
            (x->inlined() && ((code == Bytecodes::_invokedynamic && n <= 1) || (code == Bytecodes::_invokehandle && n <= 2)))),
            "only at JSR292 bytecodes");
#endif
      }
    }
  }
}

// profile parameters on entry to an inlined method
void LIRGenerator::profile_parameters_at_call(ProfileCall* x) {
  if (compilation()->profile_parameters() && x->inlined()) {
    ciMethodData* md = x->callee()->method_data_or_null();
    if (md != NULL) {
      ciParametersTypeData* parameters_type_data = md->parameters_type_data();
      if (parameters_type_data != NULL) {
        ciTypeStackSlotEntries* parameters =  parameters_type_data->parameters();
        LIR_Opr mdp = LIR_OprFact::illegalOpr;
        bool has_receiver = !x->callee()->is_static();
        ciSignature* sig = x->callee()->signature();
        ciSignatureStream sig_stream(sig, has_receiver ? x->callee()->holder() : NULL);
        int i = 0; // to iterate on the Instructions
        Value arg = x->recv();
        bool not_null = false;
        int bci = x->bci_of_invoke();
        Bytecodes::Code bc = x->method()->java_code_at_bci(bci);
        // The first parameter is the receiver so that's what we start
        // with if it exists. One exception is method handle call to
        // virtual method: the receiver is in the args list
        if (arg == NULL || !Bytecodes::has_receiver(bc)) {
          i = 1;
          arg = x->profiled_arg_at(0);
          not_null = !x->arg_needs_null_check(0);
        }
        int k = 0; // to iterate on the profile data
        for (;;) {
          intptr_t profiled_k = parameters->type(k);
          ciKlass* exact = profile_type(md, md->byte_offset_of_slot(parameters_type_data, ParametersTypeData::type_offset(0)),
                                        in_bytes(ParametersTypeData::type_offset(k)) - in_bytes(ParametersTypeData::type_offset(0)),
                                        profiled_k, arg, mdp, not_null, sig_stream.next_klass(), NULL);
          // If the profile is known statically set it once for all and do not emit any code
          if (exact != NULL) {
            md->set_parameter_type(k, exact);
          }
          k++;
          if (k >= parameters_type_data->number_of_parameters()) {
#ifdef ASSERT
            int extra = 0;
            if (MethodData::profile_arguments() && TypeProfileParmsLimit != -1 &&
                x->nb_profiled_args() >= TypeProfileParmsLimit &&
                x->recv() != NULL && Bytecodes::has_receiver(bc)) {
              extra += 1;
            }
            assert(i == x->nb_profiled_args() - extra || (TypeProfileParmsLimit != -1 && TypeProfileArgsLimit > TypeProfileParmsLimit), "unused parameters?");
#endif
            break;
          }
          arg = x->profiled_arg_at(i);
          not_null = !x->arg_needs_null_check(i);
          i++;
        }
      }
    }
  }
}

void LIRGenerator::do_ProfileCall(ProfileCall* x) {
  // Need recv in a temporary register so it interferes with the other temporaries
  LIR_Opr recv = LIR_OprFact::illegalOpr;
  LIR_Opr mdo = new_register(T_METADATA);
  // tmp is used to hold the counters on SPARC
  LIR_Opr tmp = new_pointer_register();

  if (x->nb_profiled_args() > 0) {
    profile_arguments(x);
  }

  // profile parameters on inlined method entry including receiver
  if (x->recv() != NULL || x->nb_profiled_args() > 0) {
    profile_parameters_at_call(x);
  }

  if (x->recv() != NULL) {
    LIRItem value(x->recv(), this);
    value.load_item();
    recv = new_register(T_OBJECT);
    __ move(value.result(), recv);
  }
  __ profile_call(x->method(), x->bci_of_invoke(), x->callee(), mdo, recv, tmp, x->known_holder());
}

void LIRGenerator::do_ProfileReturnType(ProfileReturnType* x) {
  int bci = x->bci_of_invoke();
  ciMethodData* md = x->method()->method_data_or_null();
  ciProfileData* data = md->bci_to_data(bci);
  if (data != NULL) {
    assert(data->is_CallTypeData() || data->is_VirtualCallTypeData(), "wrong profile data type");
    ciReturnTypeEntry* ret = data->is_CallTypeData() ? ((ciCallTypeData*)data)->ret() : ((ciVirtualCallTypeData*)data)->ret();
    LIR_Opr mdp = LIR_OprFact::illegalOpr;

    bool ignored_will_link;
    ciSignature* signature_at_call = NULL;
    x->method()->get_method_at_bci(bci, ignored_will_link, &signature_at_call);

    // The offset within the MDO of the entry to update may be too large
    // to be used in load/store instructions on some platforms. So have
    // profile_type() compute the address of the profile in a register.
    ciKlass* exact = profile_type(md, md->byte_offset_of_slot(data, ret->type_offset()), 0,
        ret->type(), x->ret(), mdp,
        !x->needs_null_check(),
        signature_at_call->return_type()->as_klass(),
        x->callee()->signature()->return_type()->as_klass());
    if (exact != NULL) {
      md->set_return_type(bci, exact);
    }
  }
}

void LIRGenerator::do_ProfileInvoke(ProfileInvoke* x) {
  // We can safely ignore accessors here, since c2 will inline them anyway,
  // accessors are also always mature.
  if (!x->inlinee()->is_accessor()) {
    CodeEmitInfo* info = state_for(x, x->state(), true);
    // Notify the runtime very infrequently only to take care of counter overflows
    increment_event_counter_impl(info, x->inlinee(), (1 << Tier23InlineeNotifyFreqLog) - 1, InvocationEntryBci, false, true);
  }
}

void LIRGenerator::increment_event_counter(CodeEmitInfo* info, int bci, bool backedge) {
  int freq_log = 0;
  int level = compilation()->env()->comp_level();
  if (level == CompLevel_limited_profile) {
    freq_log = (backedge ? Tier2BackedgeNotifyFreqLog : Tier2InvokeNotifyFreqLog);
  } else if (level == CompLevel_full_profile) {
    freq_log = (backedge ? Tier3BackedgeNotifyFreqLog : Tier3InvokeNotifyFreqLog);
  } else {
    ShouldNotReachHere();
  }
  // Increment the appropriate invocation/backedge counter and notify the runtime.
  increment_event_counter_impl(info, info->scope()->method(), (1 << freq_log) - 1, bci, backedge, true);
}

void LIRGenerator::increment_event_counter_impl(CodeEmitInfo* info,
                                                ciMethod *method, int frequency,
                                                int bci, bool backedge, bool notify) {
  assert(frequency == 0 || is_power_of_2(frequency + 1), "Frequency must be x^2 - 1 or 0");
  int level = _compilation->env()->comp_level();
  assert(level > CompLevel_simple, "Shouldn't be here");

  int offset = -1;
  LIR_Opr counter_holder = NULL;
  if (level == CompLevel_limited_profile) {
    MethodCounters* counters_adr = method->ensure_method_counters();
    if (counters_adr == NULL) {
      bailout("method counters allocation failed");
      return;
    }
    counter_holder = new_pointer_register();
    __ move(LIR_OprFact::intptrConst(counters_adr), counter_holder);
    offset = in_bytes(backedge ? MethodCounters::backedge_counter_offset() :
                                 MethodCounters::invocation_counter_offset());
  } else if (level == CompLevel_full_profile) {
    counter_holder = new_register(T_METADATA);
    offset = in_bytes(backedge ? MethodData::backedge_counter_offset() :
                                 MethodData::invocation_counter_offset());
    ciMethodData* md = method->method_data_or_null();
    assert(md != NULL, "Sanity");
    __ metadata2reg(md->constant_encoding(), counter_holder);
  } else {
    ShouldNotReachHere();
  }
  LIR_Address* counter = new LIR_Address(counter_holder, offset, T_INT);
  LIR_Opr result = new_register(T_INT);
  __ load(counter, result);
  __ add(result, LIR_OprFact::intConst(InvocationCounter::count_increment), result);
  __ store(result, counter);
  if (notify) {
    LIR_Opr mask = load_immediate(frequency << InvocationCounter::count_shift, T_INT);
    LIR_Opr meth = new_register(T_METADATA);
    __ metadata2reg(method->constant_encoding(), meth);
    __ logical_and(result, mask, result);
    __ cmp(lir_cond_equal, result, LIR_OprFact::intConst(0));
    // The bci for info can point to cmp for if's we want the if bci
    CodeStub* overflow = new CounterOverflowStub(info, bci, meth);
    __ branch(lir_cond_equal, T_INT, overflow);
    __ branch_destination(overflow->continuation());
  }
}

void LIRGenerator::do_RuntimeCall(RuntimeCall* x) {
  LIR_OprList* args = new LIR_OprList(x->number_of_arguments());
  BasicTypeList* signature = new BasicTypeList(x->number_of_arguments());

  if (x->pass_thread()) {
    signature->append(LP64_ONLY(T_LONG) NOT_LP64(T_INT));    // thread
    args->append(getThreadPointer());
  }

  for (int i = 0; i < x->number_of_arguments(); i++) {
    Value a = x->argument_at(i);
    LIRItem* item = new LIRItem(a, this);
    item->load_item();
    args->append(item->result());
    signature->append(as_BasicType(a->type()));
  }

  LIR_Opr result = call_runtime(signature, args, x->entry(), x->type(), NULL);
  if (x->type() == voidType) {
    set_no_result(x);
  } else {
    __ move(result, rlock_result(x));
  }
}

#ifdef ASSERT
void LIRGenerator::do_Assert(Assert *x) {
  ValueTag tag = x->x()->type()->tag();
  If::Condition cond = x->cond();

  LIRItem xitem(x->x(), this);
  LIRItem yitem(x->y(), this);
  LIRItem* xin = &xitem;
  LIRItem* yin = &yitem;

  assert(tag == intTag, "Only integer assertions are valid!");

  xin->load_item();
  yin->dont_load_item();

  set_no_result(x);

  LIR_Opr left = xin->result();
  LIR_Opr right = yin->result();

  __ lir_assert(lir_cond(x->cond()), left, right, x->message(), true);
}
#endif

void LIRGenerator::do_RangeCheckPredicate(RangeCheckPredicate *x) {


  Instruction *a = x->x();
  Instruction *b = x->y();
  if (!a || StressRangeCheckElimination) {
    assert(!b || StressRangeCheckElimination, "B must also be null");

    CodeEmitInfo *info = state_for(x, x->state());
    CodeStub* stub = new PredicateFailedStub(info);

    __ jump(stub);
  } else if (a->type()->as_IntConstant() && b->type()->as_IntConstant()) {
    int a_int = a->type()->as_IntConstant()->value();
    int b_int = b->type()->as_IntConstant()->value();

    bool ok = false;

    switch(x->cond()) {
      case Instruction::eql: ok = (a_int == b_int); break;
      case Instruction::neq: ok = (a_int != b_int); break;
      case Instruction::lss: ok = (a_int < b_int); break;
      case Instruction::leq: ok = (a_int <= b_int); break;
      case Instruction::gtr: ok = (a_int > b_int); break;
      case Instruction::geq: ok = (a_int >= b_int); break;
      case Instruction::aeq: ok = ((unsigned int)a_int >= (unsigned int)b_int); break;
      case Instruction::beq: ok = ((unsigned int)a_int <= (unsigned int)b_int); break;
      default: ShouldNotReachHere();
    }

    if (ok) {

      CodeEmitInfo *info = state_for(x, x->state());
      CodeStub* stub = new PredicateFailedStub(info);

      __ jump(stub);
    }
  } else {

    ValueTag tag = x->x()->type()->tag();
    If::Condition cond = x->cond();
    LIRItem xitem(x->x(), this);
    LIRItem yitem(x->y(), this);
    LIRItem* xin = &xitem;
    LIRItem* yin = &yitem;

    assert(tag == intTag, "Only integer deoptimizations are valid!");

    xin->load_item();
    yin->dont_load_item();
    set_no_result(x);

    LIR_Opr left = xin->result();
    LIR_Opr right = yin->result();

    CodeEmitInfo *info = state_for(x, x->state());
    CodeStub* stub = new PredicateFailedStub(info);

    __ cmp(lir_cond(cond), left, right);
    __ branch(lir_cond(cond), right->type(), stub);
  }
}


LIR_Opr LIRGenerator::call_runtime(Value arg1, address entry, ValueType* result_type, CodeEmitInfo* info) {
  LIRItemList args(1);
  LIRItem value(arg1, this);
  args.append(&value);
  BasicTypeList signature;
  signature.append(as_BasicType(arg1->type()));

  return call_runtime(&signature, &args, entry, result_type, info);
}


LIR_Opr LIRGenerator::call_runtime(Value arg1, Value arg2, address entry, ValueType* result_type, CodeEmitInfo* info) {
  LIRItemList args(2);
  LIRItem value1(arg1, this);
  LIRItem value2(arg2, this);
  args.append(&value1);
  args.append(&value2);
  BasicTypeList signature;
  signature.append(as_BasicType(arg1->type()));
  signature.append(as_BasicType(arg2->type()));

  return call_runtime(&signature, &args, entry, result_type, info);
}


LIR_Opr LIRGenerator::call_runtime(BasicTypeArray* signature, LIR_OprList* args,
                                   address entry, ValueType* result_type, CodeEmitInfo* info) {
  // get a result register
  LIR_Opr phys_reg = LIR_OprFact::illegalOpr;
  LIR_Opr result = LIR_OprFact::illegalOpr;
  if (result_type->tag() != voidTag) {
    result = new_register(result_type);
    phys_reg = result_register_for(result_type);
  }

  // move the arguments into the correct location
  CallingConvention* cc = frame_map()->c_calling_convention(signature);
  assert(cc->length() == args->length(), "argument mismatch");
  for (int i = 0; i < args->length(); i++) {
    LIR_Opr arg = args->at(i);
    LIR_Opr loc = cc->at(i);
    if (loc->is_register()) {
      __ move(arg, loc);
    } else {
      LIR_Address* addr = loc->as_address_ptr();
//           if (!can_store_as_constant(arg)) {
//             LIR_Opr tmp = new_register(arg->type());
//             __ move(arg, tmp);
//             arg = tmp;
//           }
      if (addr->type() == T_LONG || addr->type() == T_DOUBLE) {
        __ unaligned_move(arg, addr);
      } else {
        __ move(arg, addr);
      }
    }
  }

  if (info) {
    __ call_runtime(entry, getThreadTemp(), phys_reg, cc->args(), info);
  } else {
    __ call_runtime_leaf(entry, getThreadTemp(), phys_reg, cc->args());
  }
  if (result->is_valid()) {
    __ move(phys_reg, result);
  }
  return result;
}


LIR_Opr LIRGenerator::call_runtime(BasicTypeArray* signature, LIRItemList* args,
                                   address entry, ValueType* result_type, CodeEmitInfo* info) {
  // get a result register
  LIR_Opr phys_reg = LIR_OprFact::illegalOpr;
  LIR_Opr result = LIR_OprFact::illegalOpr;
  if (result_type->tag() != voidTag) {
    result = new_register(result_type);
    phys_reg = result_register_for(result_type);
  }

  // move the arguments into the correct location
  CallingConvention* cc = frame_map()->c_calling_convention(signature);

  assert(cc->length() == args->length(), "argument mismatch");
  for (int i = 0; i < args->length(); i++) {
    LIRItem* arg = args->at(i);
    LIR_Opr loc = cc->at(i);
    if (loc->is_register()) {
      arg->load_item_force(loc);
    } else {
      LIR_Address* addr = loc->as_address_ptr();
      arg->load_for_store(addr->type());
      if (addr->type() == T_LONG || addr->type() == T_DOUBLE) {
        __ unaligned_move(arg->result(), addr);
      } else {
        __ move(arg->result(), addr);
      }
    }
  }

  if (info) {
    __ call_runtime(entry, getThreadTemp(), phys_reg, cc->args(), info);
  } else {
    __ call_runtime_leaf(entry, getThreadTemp(), phys_reg, cc->args());
  }
  if (result->is_valid()) {
    __ move(phys_reg, result);
  }
  return result;
}

void LIRGenerator::do_MemBar(MemBar* x) {
  if (os::is_MP()) {
    LIR_Code code = x->code();
    switch(code) {
      case lir_membar_acquire   : __ membar_acquire(); break;
      case lir_membar_release   : __ membar_release(); break;
      case lir_membar           : __ membar(); break;
      case lir_membar_loadload  : __ membar_loadload(); break;
      case lir_membar_storestore: __ membar_storestore(); break;
      case lir_membar_loadstore : __ membar_loadstore(); break;
      case lir_membar_storeload : __ membar_storeload(); break;
      default                   : ShouldNotReachHere(); break;
    }
  }
}

LIR_Opr LIRGenerator::maybe_mask_boolean(StoreIndexed* x, LIR_Opr array, LIR_Opr value, CodeEmitInfo*& null_check_info) {
  if (x->check_boolean()) {
    LIR_Opr value_fixed = rlock_byte(T_BYTE);
    if (TwoOperandLIRForm) {
      __ move(value, value_fixed);
      __ logical_and(value_fixed, LIR_OprFact::intConst(1), value_fixed);
    } else {
      __ logical_and(value, LIR_OprFact::intConst(1), value_fixed);
    }
    LIR_Opr klass = new_register(T_METADATA);
    __ move(new LIR_Address(array, oopDesc::klass_offset_in_bytes(), T_ADDRESS), klass, null_check_info);
    null_check_info = NULL;
    LIR_Opr layout = new_register(T_INT);
    __ move(new LIR_Address(klass, in_bytes(Klass::layout_helper_offset()), T_INT), layout);
    int diffbit = Klass::layout_helper_boolean_diffbit();
    __ logical_and(layout, LIR_OprFact::intConst(diffbit), layout);
    __ cmp(lir_cond_notEqual, layout, LIR_OprFact::intConst(0));
    __ cmove(lir_cond_notEqual, value_fixed, value, value_fixed, T_BYTE);
    value = value_fixed;
  }
  return value;
}
