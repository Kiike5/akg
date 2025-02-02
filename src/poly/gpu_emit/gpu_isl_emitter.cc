/**
 * Copyright 2020-2021 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gpu_isl_emitter.h"
#include "emit_pass.h"
#include <sstream>
#include <algorithm>

namespace akg {
namespace ir {
namespace poly {

Expr GpuIslEmitter::EmitLoad(const isl::ast_expr &expr, const Type type) {
  if (PRINT_EMITTER) {
    LOG(INFO) << ">>>>>>>>>>>>INPUT AST_NODE[LOAD]<<<<<<<<<<<<<<\n" << expr;
  }
  if (auto op = expr.as<isl::ast_expr_op>()) {
    if (auto access = op.as<isl::ast_expr_op_access>()) {
      CHECK(op.get_arg(0).as<isl::ast_expr_id>());
      auto var = op.get_arg(0).as<isl::ast_expr_id>().get_id();
      Array<Expr> local_args;
      for (unsigned int i = 1; i < op.get_n_arg(); ++i) {
        local_args.push_back(Interpret(op.get_arg(i)));
      }

      Tensor t = info_.FindTensor(var);
      auto call = Call::make(type, t->op->name, local_args, Call::CallType::Halide, t->op, t->value_index);
      if (PRINT_EMITTER) {
        LOG(INFO) << ">>>>>>>>>>>>OUTPUT STMT<<<<<<<<<<<<\n" << call;
      }
      return call;
    }
  }
  return Expr();
}

Stmt GpuIslEmitter::EmitRead(const isl::ast_node_user &node) {
  isl::id node_id = node.get_annotation();
  isl::pw_multi_aff iterator_map = node_info_map_.at(node_id).iterator_map;
  isl::pw_multi_aff hoisted = iterator_map.range_factor_range();
  isl::pw_multi_aff original = iterator_map.range_factor_domain().range_factor_range();

  isl::id original_tensor = original.get_tuple_id(isl_dim_out);

  auto build = node_info_map_.at(node_id).build;
  auto lhs = build.access_from(isl::multi_pw_aff(hoisted));
  auto rhs = build.access_from(isl::multi_pw_aff(original));

  Type type = info_.GetDtypeOf(rhs);
  if (auto op = lhs.as<isl::ast_expr_op>()) {
    if (auto access = op.as<isl::ast_expr_op_access>()) {
      Expr value = EmitLoad(rhs, type);
      auto var = op.get_arg(0).as<isl::ast_expr_id>().get_id();

      Array<Expr> local_args;
      for (unsigned int i = 1; i < op.get_n_arg(); ++i) {
        local_args.push_back(Interpret(op.get_arg(i)));
      }

      Tensor t = info_.FindTensor(var);
      CHECK(t.defined());
      return Provide::make(t->op, 0, value, local_args);
    }
  }
  return Stmt();
}

Stmt GpuIslEmitter::EmitWrite(const isl::ast_node_user &node) {
  auto node_id = node.get_annotation();
  CHECK_GT(node_info_map_.count(node_id), 0);
  auto iterator_map = node_info_map_.at(node_id).iterator_map;
  auto hoisted = iterator_map.range_factor_range();
  auto original = iterator_map.range_factor_domain().range_factor_range();

  auto build = node_info_map_.at(node_id).build;
  auto rhs = build.access_from(isl::multi_pw_aff(hoisted));
  auto lhs = build.access_from(isl::multi_pw_aff(original));
  Type type = info_.GetDtypeOf(lhs);

  if (auto op = lhs.as<isl::ast_expr_op>()) {
    if (auto access = op.as<isl::ast_expr_op_access>()) {
      Expr value = EmitLoad(rhs, type);
      auto var = op.get_arg(0).as<isl::ast_expr_id>().get_id();

      Array<Expr> local_args;
      for (unsigned int i = 1; i < op.get_n_arg(); ++i) {
        local_args.push_back(Interpret(op.get_arg(static_cast<int>(i))));
      }

      Tensor t = info_.FindTensor(var);
      CHECK(t.defined());

      return Provide::make(t->op, 0, value, local_args);
    }
  }
  return Stmt();
}

Stmt GpuIslEmitter::EmitSync() {
  return Evaluate::make(Call::make(Int(32), STORAGE_SYNC, {StringImm::make(SYNC_SCOP_SHARED)}, Call::Intrinsic));
}

Stmt GpuIslEmitter::EmitStmt(const isl::ast_node_user &node) {
  CHECK(node.get_expr().isa<isl::ast_expr_op>());
  isl::ast_expr_op usr_expr = node.get_expr().as<isl::ast_expr_op>();
  CHECK(usr_expr);
  auto stmt_id = usr_expr.get_arg(0).as<isl::ast_expr_id>().get_id();
  auto node_id = node.get_annotation();

  if (info_.IsRead(stmt_id)) {
    Stmt s;
    s = EmitRead(node);
    s = AttrStmt::make(Expr(""), GMREAD_FLAG, StringImm::make(GMREAD_FLAG), s);
    return s;
  } else if (info_.IsWrite(stmt_id)) {
    if (info_.IsGMWrite(stmt_id) || info_.IsGMLWrite(stmt_id)) {
      auto iterator_map = node_info_map_.at(node_id).iterator_map;
      auto original = iterator_map.range_factor_domain().range_factor_range();
      auto srcid = original.get_tuple_id(isl_dim_out);
      bool no_need_to_emit = GpuIslEmitter::NoNeedToEmitForTempTensor(srcid);
      if (no_need_to_emit) return Stmt();
    }
    return EmitWrite(node);
  } else if (info_.IsSync(stmt_id)) {
    return EmitSync();
  } else {
    return EmitUserStmt(node);
  }
}

bool GpuIslEmitter::NoNeedToEmitForTempTensor(const isl::id &id) {
  bool no_need = true;
  auto origin_binds = info_.user_config_.GetOriginBind();
  for (auto i : origin_binds) {
    if (!i.first.defined()) continue;
    std::string name = i.first->op->name;
    if (name == id.name()) {
      no_need = false;
      break;
    }
  }
  return no_need;
}

Stmt GpuIslEmitter::EmitBlock(const isl::ast_node_block &block_node) {
  std::vector<Stmt> stmts;

  int num = block_node.get_children().size();
  int last_num = 0;
  for (int i = num - 1; i >= 0; --i) {
    auto child = block_node.get_children().at(i);

    if (auto node = child.as<isl::ast_node_user>()) {
      CHECK(node.get_expr().isa<isl::ast_expr_op>());
      isl::ast_expr_op usr_expr = node.get_expr().as<isl::ast_expr_op>();
      CHECK(usr_expr);
      auto stmt_id = usr_expr.get_arg(0).as<isl::ast_expr_id>().get_id();
      if (info_.IsRealize(stmt_id)) {
        isl::id new_stmt_id = isl::id(stmt_id.ctx(), stmt_id.name().substr(REALIZE_PREFIX_LEN));
        int stmt_num = stmts.size();
        CHECK_NE(stmt_num, 0) << "when stmt_num is zero, no realize should be emitted!.";
        if (stmt_num == 1) {
          stmts[0] = InsertRealize(stmts[0], new_stmt_id);
        } else {
          if (stmt_num - last_num == 1) {
            stmts[0] = InsertRealize(stmts[0], new_stmt_id);
          } else {
            for (int index = stmt_num - 2 - last_num; index >= 0; --index) {
              auto p_index = static_cast<unsigned int>(index);
              stmts[p_index] = Block::make(stmts[p_index], stmts[p_index + 1]);
            }
            stmts[0] = InsertRealize(stmts[0], new_stmt_id);
          }
        }
        last_num = stmt_num - 1;
        continue;
      }
    }

    Stmt body = EmitAst(child);
    if (!body.defined()) continue;
    stmts.insert(stmts.begin(), body);
  }

  int len = stmts.size();

  if (len == 0) {
    return Stmt();
  }

  if (last_num == len - 1) {
    return stmts[0];
  } else {
    for (int index = len - 2 - last_num; index >= 0; --index) {
      auto p_index = static_cast<unsigned int>(index);
      stmts[p_index] = Block::make(stmts[p_index], stmts[p_index + 1]);
    }
    return stmts[0];
  }
}

Stmt GpuIslEmitter::EmitFor(const isl::ast_node_for &node) {
  isl::id isl_iter_id = node.get_iterator().as<isl::ast_expr_id>().get_id();
  VarExpr iter_expr(isl_iter_id.to_str());
  PushIter(iter_expr.get());

  Expr init_expr = Interpret(node.get_init());

  auto isl_cond = node.get_cond().as<isl::ast_expr_op>();
  CHECK(isl_cond.as<isl::ast_expr_op_lt>() || isl_cond.as<isl::ast_expr_op_le>());
  auto cond_lhs = isl_cond.get_arg(0).as<isl::ast_expr_id>();
  CHECK(cond_lhs);
  CHECK_EQ(cond_lhs.get_id(), isl_iter_id);
  Expr cond_expr = Interpret(isl_cond.get_arg(1));

  int64_t inc = static_cast<int64_t>(WrappedStrtol(node.get_inc().to_C_str()));
  CHECK_NE(inc, 0) << "stride should not be zero!.";

  bool need_to_modify_inc_ = false;
  if (inc != 1) {
    need_to_modify_inc_ = true;
    Expr original_init_expr = init_expr;
    init_expr = ModifyTheInitExpr(init_expr);
    cond_expr = ModifyTheCondExpr(cond_expr, static_cast<int>(inc));
    Expr modify_iter = ModifyTheIterExpr(iter_expr, static_cast<int>(inc), original_init_expr);
    stride_modify_iter_map_[iter_expr.get()] = modify_iter;
  }

  if (isl_cond.as<isl::ast_expr_op_le>()) {
    cond_expr = Simplify(cond_expr + 1);
  }

  cond_expr = Simplify(cond_expr - init_expr);

  Stmt body_stmt = EmitAst(node.get_body());

  if (!body_stmt.defined()) {
    PopIter(iter_expr.get());
    return Stmt();
  }

  if (need_to_modify_inc_) {
    stride_modify_iter_map_.erase(iter_expr.get());
  }
  PopIter(iter_expr.get());
  Stmt stmt = For::make(iter_expr, init_expr, cond_expr, ForType::Serial, DeviceAPI::None, body_stmt);
  return stmt;
}

Stmt GpuIslEmitter::EmitIf(const isl::ast_node_if &node) {
  Expr cond_expr = Interpret(node.get_cond());
  cur_if_list_.push_back(cond_expr.get());
  Stmt then_case = EmitAst(node.get_then_node());
  if (!then_case.defined()) {
    return Stmt();
  }
  Stmt else_case;
  if (node.has_else_node()) {
    else_case = EmitAst(node.get_else_node());
  }
  cur_if_list_.pop_back();

  Stmt s;
  if (!cond_expr.defined()) {
    s = then_case;
  } else {
    s = IfThenElse::make(cond_expr, then_case, else_case);
  }

  return s;
}

Expr GpuIslEmitter::ModifyTheInitExpr(const Expr &e) { return 0; }

Expr GpuIslEmitter::ModifyTheCondExpr(const Expr &e, int inc) { return e / Expr(inc); }

Expr GpuIslEmitter::ModifyTheIterExpr(const VarExpr &iter, int inc, const Expr &init) {
  return Simplify(iter * inc + init);
}

int GpuIslEmitter::GetThreadExtent(const std::string &name) {
  if (name == BLOCK_IDX_X || name == BLOCK_IDX_Y || name == BLOCK_IDX_Z) {
    auto block_cfg = info_.user_config_.GetBlockConfig();
    CHECK(block_cfg) << "block config is null.";
    return name == BLOCK_IDX_X ? block_cfg->GetX().second
                               : (name == BLOCK_IDX_Y ? block_cfg->GetY().second : block_cfg->GetZ().second);
  }

  if (name == THREAD_IDX_X || name == THREAD_IDX_Y || name == THREAD_IDX_Z) {
    auto thread_cfg = info_.user_config_.GetThreadConfig();
    CHECK(thread_cfg) << "thread config is null.";
    if (info_.user_config_.GetEnableOneDimThread()) {
      return name == THREAD_IDX_X ? (thread_cfg->GetX().second * thread_cfg->GetY().second * thread_cfg->GetZ().second)
                                  : 1;
    }
    return name == THREAD_IDX_X ? thread_cfg->GetX().second
                                : (name == THREAD_IDX_Y ? thread_cfg->GetY().second : thread_cfg->GetZ().second);
  }
  LOG(WARNING) << "Unrecognized thread name " << name;
  return 1;
}

Stmt GpuIslEmitter::Emit(const isl::ast_node &node) {
  Stmt stmt = EmitAst(node);

  // emit realize for temporary tensor
  stmt = EmitRealizeForGlobalTensor(stmt);

  // iter var node attr emit
  std::map<std::string, VarExpr>::iterator it;
  for (it = iter_name_map_.begin(); it != iter_name_map_.end(); it++) {
    IterVar axis = IterVarNode::make(Range(), it->second, air::kThreadIndex, it->second->name_hint);
    stmt = AttrStmt::make(axis, air::ir::attr::thread_extent, Expr(GetThreadExtent(it->second->name_hint)), stmt);
  }

  // attr for one dimension mapping
  if (info_.user_config_.GetEnableOneDimThread()) {
    auto thread_cfg = info_.user_config_.GetThreadConfig();
    CHECK(thread_cfg) << "thread config is null.";
    int tx = thread_cfg->GetX().second;
    stmt = AttrStmt::make(Expr(""), ORIGIN_THREAD_DIM_X, Expr(tx), stmt);
  }

  return stmt;
}

Stmt GpuIslEmitter::EmitRealizeForGlobalTensor(Stmt stmt) {
  auto binds = info_.user_config_.GetBind();
  auto origin_binds = info_.user_config_.GetOriginBind();
  std::unordered_set<std::string> tensor_name;

  for (auto i : binds) {
    if (!i.first.defined()) continue;
    tensor_name.insert(i.first->op->name);
  }

  for (auto i : binds) {
    if (!i.first.defined()) continue;
    // input and output tensor, no need to emit realize
    if (origin_binds.find(i.first) != origin_binds.end()) {
      continue;
    }

    // promoted tensor, the realize info already emitted before
    std::string name = i.first->op->name;
    if (IsEndsWith(name, MEM_TYPE_SHARED) || IsEndsWith(name, MEM_TYPE_LOCAL)) {
      continue;
    }

    // if the tensor is temporary, but has already promoted, there is no need to emit realize
    if (tensor_name.find(name + "_" + MEM_TYPE_SHARED) != tensor_name.end() ||
        tensor_name.find(name + "_" + MEM_TYPE_LOCAL) != tensor_name.end()) {
      continue;
    }

    // if the tensor is temporary and it is not promoted, it needs to emit realize
    stmt = InsertRealize(stmt, isl::id(info_.GetCtx(), name));
  }
  return stmt;
}

Stmt GpuIslEmitter::EmitMark(const isl::ast_node_mark &node) {
  std::string mark = node.get_id().get_name();

  // add for prefetch pass
  if (mark == PROMOTE_GLOBAL_TO_SHARED_AB) {
    Stmt stmt = EmitAst(node.get_node());
    if (!stmt.defined()) {
      return Stmt();
    }
    return AttrStmt::make(Expr("INFO"), SHARED_MEM_PROMOTED_COMPLETE, StringImm::make(SHARED_MEM_PROMOTED_COMPLETE),
                          stmt);
  }

  Stmt stmt;

  if ((mark == PROMOTE_VECTORIZATION) || (mark == PROMOTE_REGISTER_TO_GLOBAL) || (mark == PROMOTE_REGISTER_TO_SHARED) ||
      (mark == PROMOTE_SHARED_TO_GLOBAL)) {
    stmt = EmitAst(node.get_node());
    if (!stmt.defined()) {
      return Stmt();
    }
    stmt = AttrStmt::make(Expr("INFO"), mark, StringImm::make(mark), stmt);
  } else {
    stmt = EmitAst(node.get_node());
  }

  return stmt;
}

std::string GpuIslEmitter::FindRealizeScopeToString(const isl::id &var) {
  if (info_.analysis_result_.CountBufferDefInfo(var)) {
    auto tensor_info = info_.analysis_result_.GetBufferDefInfo(var);
    MemType mem_type = tensor_info.DstMemType();

    switch (mem_type) {
      case MemType::SHARED_:
        return MEM_TYPE_SHARED;
      case MemType::LOCAL_:
        return MEM_TYPE_LOCAL;
      default:
        LOG(FATAL) << "unexpected mem_type of var " << var;
        return "ERROR";
    }
  }
  return "";
}

Expr GpuIslEmitter::FindRealizeScope(const isl::id &var) { return Expr(FindRealizeScopeToString(var)); }

Stmt GpuIslEmitter::InsertRealize(Stmt stmt, const isl::id &var) {
  stmt = FindInnerRealize(var.get_name()).Mutate(stmt);

  // A tensor may be defined multiple times in BufferDefInfo due to nested realize.
  // Because we cannot determine which one we actually want, we have to be conservative here
  // and allocate space for the largest shape to avoid overflow.
  Tensor t = info_.FindTensorWithLargestShape(var);
  Region bounds;

  // no isolate
  if (bounds.empty()) {
    for (auto j : t->shape) {
      bounds.push_back(Range::make_by_min_extent(Expr(0), j));
    }
  }

  // If isolate, make a new buffer
  auto buf = info_.user_config_.GetBind().at(t);

  auto tt = placeholder(t->shape, t->dtype, t->op->name);

  stmt = TensorSubstitute(stmt, t->op, tt->op, tt->value_index);
  t = tt;
  if (info_.analysis_result_.CountBufferDefInfo(var)) {
    auto decl = info_.analysis_result_.GetBufferDefInfo(var);
    decl.tensor = t;
  }
  info_.user_config_.SetBind(t, buf);
  stmt = TensorSubstitute2(stmt, t->op->func_name(), t->op, t->value_index);
  stmt = Realize::make(t->op, t->value_index, t->dtype, bounds, const_true(1), stmt);
  stmt = AttrStmt::make(t->op, air::ir::attr::realize_scope, FindRealizeScope(var), stmt);

  return stmt;
}

Expr GpuIslEmitter::IterNameAdaptor(std::string name) {
  if (iter_name_map_.find(name) != iter_name_map_.end()) {
    return iter_name_map_[name];
  } else if (name.find(REPLACE) != std::string::npos) {
    name = name.substr(strlen(REPLACE));
    return AdaptPolyNewVar(name);
  } else {
    return VarExpr(name);
  }
}

// if new var is added in poly process, modify the logic here.
// another modify pos is IterNameAdaptor interface
Expr GpuIslEmitter::AdaptPolyNewVar(std::string name) {
  Expr e;
  std::string t0_string = T0;
  int suffix_len = t0_string.size() + 1;
  auto tensor_name = name.substr(0, name.size() - suffix_len);
  if (!info_.user_config_.GetReplaceConfig().count(tensor_name)) {
    return e;
  }
  auto mapping_cfg = (info_.user_config_.GetReplaceConfig()[tensor_name]);
  CHECK(mapping_cfg) << "mapping config is null.";
  if (mapping_cfg->type == MappingType::REPLACE_THREADS) {
    e = AdaptThreadNewVar(name, mapping_cfg);
  } else {
    e = AdaptBlockNewVar(name, mapping_cfg);
  }
  CHECK(e.defined()) << "new var is null";
  return e;
}

Expr GpuIslEmitter::AdaptBlockNewVar(const std::string name, MappingCfg *mapping_cfg) {
  Expr e;
  if (name.find(CONV_H_W) != std::string::npos) {
    int mx = mapping_cfg->GetX().second;
    if (name.find(B0) != std::string::npos) {
      e = Mod::make(iter_name_map_[B1], mx);
      return e;
    } else if (name.find(B1) != std::string::npos) {
      e = Div::make(iter_name_map_[B1], mx);
      return e;
    }
  } else if (name.find(CONV_N) != std::string::npos) {
    return iter_name_map_[B2];
  } else if (name.find(CONV_O) != std::string::npos) {
    return iter_name_map_[B0];
  }
  return e;
}

Expr GpuIslEmitter::AdaptThreadNewVar(const std::string name, MappingCfg *mapping_cfg) {
  Expr e;
  int mx = mapping_cfg->GetX().second;
  if (name.find(WARP_COMPUTE) != std::string::npos) {
    if (name.find(T0) != std::string::npos) {
      e = Div::make(iter_name_map_[T0], WARP_SIZE);
      e = Mod::make(e, mx);
      return e;
    } else if (name.find(T1) != std::string::npos) {
      e = Div::make(iter_name_map_[T0], WARP_SIZE);
      e = Div::make(e, mx);
      return e;
    }
  } else {
    Expr div_e = iter_name_map_[T0];
    for (size_t i = 0; i < mapping_cfg->bound; ++i) {
      std::string thread_id_name = "t" + std::to_string(i);
      if (name.find(thread_id_name) == std::string::npos) {
        continue;
      }

      e = iter_name_map_[T0];
      int thread_id_number = mapping_cfg->GetAt(i).second;

      if (i == 0) {
        e = Mod::make(e, thread_id_number);
        return e;
      }

      for (size_t j = 0; j < i; ++j) {
        thread_id_number = mapping_cfg->GetAt(j).second;
        e = Div::make(e, thread_id_number);
      }

      thread_id_number = mapping_cfg->GetAt(i).second;
      e = Mod::make(e, thread_id_number);
      return e;
    }
  }
  return e;
}

Expr GpuIslEmitter::Interpret(const isl::ast_expr &e) {
  if (auto int_expr = e.as<isl::ast_expr_int>()) {
    return Expr(IslExprToSInt(int_expr));
  } else if (auto id_expr = e.as<isl::ast_expr_id>()) {
    // If this variable is defined by loop index, we need sharing it.
    const Variable *var = GetIterByName(id_expr.get_id().get_name());
    if (var) {
      if (stride_modify_iter_map_.find(var) != stride_modify_iter_map_.end()) {
        return stride_modify_iter_map_[var];
      }
      return VarExpr(GetObjPtr(var));
    } else {
      return IterNameAdaptor(id_expr.get_id().to_str());
    }
  } else if (auto op_expr = e.as<isl::ast_expr_op>()) {
    return InterpretOp(op_expr);
  } else {
    LOG(FATAL) << "NYI " << e;
    return 0;
  }
}

Stmt GpuIslEmitter::EmitAccessNodeFromPromoteAcsCall(isl::id var, const Node *node, Array<Expr> &args) {
  const Call *call = static_cast<const Call *>(node);
  Tensor t = info_.FindTensor(var);
  return Evaluate::make(Call::make(call->type, var.get_name(), args, call->call_type, t->op, t->value_index));
}

Stmt GpuIslEmitter::EmitAccessNodeFromPromoteAcsProvide(isl::id var, const Node *node, Array<Expr> &args) {
  const auto provide = static_cast<const Provide *>(node);
  Tensor t = info_.FindTensor(var);
  Stmt s = Provide::make(t->op, 0, provide->value, args);
  return s;
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
