/**
 * Copyright 2021 Huawei Technologies Co., Ltd
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

#include "operator_mapping_strategy.h"

#include <numeric>

#include "poly/schedule_pass/reschedule.h"
#include "poly/schedule_tree_util.h"
#include "poly/sync_manager.h"
#include "poly/scop.h"

namespace akg {
namespace ir {
namespace poly {

size_t OperatorMappingStrategy::GetFinalMappingThreadNumber(isl::schedule_node &node, const size_t thread_cfg_bound,
                                                            const size_t n_thread_map) {
  auto final_n_thread_map = n_thread_map;
  isl::schedule_node_band band_node = node.as<isl::schedule_node_band>();
  // Split band node according to mapping config and coincidence of band node.
  if (final_n_thread_map > thread_cfg_bound) {
    node = band_node.split(final_n_thread_map - thread_cfg_bound);
    node = node.child(0);
    final_n_thread_map = thread_cfg_bound;
    band_node = node.as<isl::schedule_node_band>();
  }

  // Split to keep nodes with coincident equals to 1.
  if (final_n_thread_map < band_node.n_member() && !scop_info_.user_config_.EnableStitchFusion()) {
    node = band_node.split(final_n_thread_map);
  } else {
    final_n_thread_map = static_cast<size_t>(band_node.n_member());
  }
  return final_n_thread_map;
}

size_t OperatorMappingStrategy::MapThreadHelper(isl::schedule_node &thread_root, const bool need_reverse) {
  auto thread_cfg = scop_info_.user_config_.GetThreadConfig();
  CHECK(thread_cfg != nullptr) << "thread config is null";
  if (thread_cfg->bound < 1 || !thread_root.isa<isl::schedule_node_band>()) {
    return 0;
  }

  int start_node_depth = thread_root.get_tree_depth();
  // Determine max num dimension of threads that can be mapped.
  auto n_thread_map = CountConsecutiveCoincident(thread_root);
  if (n_thread_map < 1) {
    return 0;
  }
  n_thread_map = GetFinalMappingThreadNumber(thread_root, thread_cfg->bound, n_thread_map);

  // Map band under thread_root from inner dim to outer dim.
  Mapping mapping;
  thread_root = MapInnerDimToThreads(thread_root, false, thread_cfg, mapping, need_reverse);
  auto tile_node = GetMarkerName(thread_root, THREAD_MARKER).empty() ? thread_root.child(0) : thread_root;
  scop_info_.upa_node_mapping_.emplace_back(std::make_pair(tile_node, mapping));

  // Do unroll if needed.
  if (scop_info_.user_config_.GetMaxUnrollLoop() != 1) {
    isl::schedule_node unroll_node = thread_root.child(0);
    thread_root = UnrollByMarkOptions(unroll_node, scop_info_.user_config_.GetMaxUnrollLoop());
  }

  int end_node_depth = thread_root.get_tree_depth() - start_node_depth;
  thread_root = thread_root.ancestor(end_node_depth);
  return thread_cfg->bound;
}

isl::schedule_node OperatorMappingStrategy::MapBlockHelper(const isl::schedule_node &orig_node, MappingCfg *block_cfg,
                                                           size_t n_block_map, bool check_extent,
                                                           std::unordered_map<size_t, size_t> map_idx_shift) {
  auto node = orig_node;
  auto band_node = node.as<isl::schedule_node_band>();
  if (!band_node || !band_node.permutable()) {
    LOG(WARNING) << "No permutable outer band node to map block.";
    return node;
  }

  auto partial_schedule = band_node.get_partial_schedule();
  auto upa_list = partial_schedule.get_union_pw_aff_list();

  if (check_extent) {
    auto domain = band_node.get_schedule().get_domain();
    isl::union_pw_aff_list range_aff_list(band_node.ctx(), static_cast<int>(upa_list.size()));
    for (int i = upa_list.size() - 1; i >= 0; --i) {
      auto range = upa_list.get_at(i).intersect_domain(domain);
      range_aff_list = range_aff_list.add(range);
    }
    node = CheckMapSizeAndApplyTile(node, range_aff_list, block_cfg, false);
  }

  upa_list = upa_list.drop(n_block_map, upa_list.size() - n_block_map).reverse();

  node = node.insert_mark(isl::id(node.ctx(), BLOCK_MARKER));
  node = node.child(0);

  Mapping mapping;
  node = CreateAndInsertMapFilter(node, false, upa_list, block_cfg, mapping, map_idx_shift);
  scop_info_.upa_node_mapping_.emplace_back(std::make_pair(node.parent(), mapping));

  return node;
}

size_t ReduceMappingStrategy::MapThreadHelper(isl::schedule_node &thread_root) {
  auto thread_cfg = scop_info_.user_config_.GetThreadConfig();
  CHECK(thread_cfg != nullptr) << "thread config is null";
  if (thread_cfg->bound < 1 || !thread_root.isa<isl::schedule_node_band>()) {
    return 0;
  }

  int start_node_depth = thread_root.get_tree_depth();
  // Determine max num dimension of threads that can be mapped.
  auto n_thread_map = CountConsecutiveCoincident(thread_root);

  std::string reduce_marker_name = "";
  if (thread_root.has_parent()) {
    reduce_marker_name = GetMarkerName(thread_root.parent(), REDUCE_MARKER);
    if (!reduce_marker_name.empty()) {
      thread_root = thread_root.parent().del();
      ++n_thread_map;
    }
  }

  // When akg reduce lib is enabled, we can try to map other injective statements whose coincidence equals 0
  if (n_thread_map < thread_cfg->bound && scop_info_.user_config_.GetEnableAkgReduceLib()) {
    n_thread_map = thread_cfg->bound;
  }

  if (n_thread_map < 1) {
    return 0;
  }
  n_thread_map = GetFinalMappingThreadNumber(thread_root, thread_cfg->bound, n_thread_map);

  // Map band under thread_root from inner dim to outer dim.
  Mapping mapping;
  bool is_y_reduce = scop_info_.analysis_result_.GetReduceDirection() == Y_DIRECTION;
  thread_root = MapInnerDimToThreads(thread_root, false, thread_cfg, mapping, is_y_reduce);

  // If the current band is split during the mapping process, split the reduce axis and non-reduce axis of
  // the outer band.
  bool is_tiled = GetMarkerName(thread_root, THREAD_MARKER).empty();
  if (is_tiled && n_thread_map > 1) {
    isl::schedule_node_band band_node = thread_root.as<isl::schedule_node_band>();
    thread_root = band_node.split(n_thread_map - 1).child(0);
  }
  thread_root = thread_root.insert_mark(reduce_marker_name);
  thread_root = thread_root.child(0);
  // Add the filter that initializes and calls the akg_reduce library for the reduce statement.
  thread_root = InsertReduceExtension(thread_root);
  // The band corresponding to the reduce statement has a REDUCE_MARKER that needs to be deleted at the beginning.
  int end_node_depth = thread_root.get_tree_depth() - start_node_depth + 1;
  thread_root = thread_root.ancestor(end_node_depth);
  scop_info_.upa_node_mapping_.emplace_back(std::make_pair(thread_root, mapping));
  return thread_cfg->bound;
}

size_t ReduceMappingStrategy::GetReduceId() const {
  static size_t reduce_count = 0;
  return reduce_count++;
}

isl::schedule_node ReduceMappingStrategy::InsertReduceExtension(const isl::schedule_node &node) {
  auto thread_cfg = scop_info_.user_config_.GetThreadConfig();
  CHECK(thread_cfg != nullptr) << "thread config is null";

  isl::schedule_node insert_node = node;
  isl::schedule_node parent_node = node;
  isl::schedule_node ancestor_node = node;
  if (insert_node.has_parent()) {
    parent_node = parent_node.parent();
    if (parent_node.has_parent()) {
      ancestor_node = parent_node.parent();
    }
  }

  std::string reduce_marker_name = "";
  if (!GetMarkerName(parent_node, REDUCE_MARKER).empty()) {
    reduce_marker_name = GetMarkerName(parent_node, REDUCE_MARKER);
    insert_node = parent_node.del();
  }

  if (!GetMarkerName(ancestor_node, REDUCE_MARKER).empty()) {
    reduce_marker_name = GetMarkerName(ancestor_node, REDUCE_MARKER);
    insert_node = ancestor_node.del();
  }

  if (reduce_marker_name.empty()) {
    return node;
  }

  reduce_marker_name.erase(0, strlen(REDUCE_MARKER));
  isl::id sync_id = isl::id(insert_node.ctx(), REDUCE_UPDATE + reduce_marker_name);
  isl::id reduction_id = isl::id(insert_node.ctx(), REDUCE_INIT + reduce_marker_name);

  insert_node = InsertExtensionNodeBeforeOrAfter(insert_node, reduction_id, true);
  insert_node = InsertExtensionNodeBeforeOrAfter(insert_node, sync_id, false).parent();
  insert_node = insert_node.parent().insert_mark(REDUCE_AREA_FLAG);

  return insert_node;
}

isl::schedule ReduceMappingStrategy::DetectAndMarkReduce(const isl::schedule &sch) {
  auto final_schedule = sch;
  auto thread_cfg = scop_info_.user_config_.GetThreadConfig();
  CHECK(thread_cfg != nullptr) << "threadconfig is null";
  if (thread_cfg->bound == 0) {
    return final_schedule;
  }

  auto all_reduce_map = scop_info_.analysis_result_.GetReduceTensorInfoMap();
  ReduceManager reduce_manager;
  bool done_separate = false;
  auto GetInnerMostBand = [&done_separate, &all_reduce_map, &reduce_manager, thread_cfg,
                           this](isl::schedule_node node) -> isl::schedule_node {
    if (done_separate) {
      return node;
    }
    auto band_node = node.as<isl::schedule_node_band>();
    if (!band_node || !band_node.permutable()) {
      return node;
    }

    auto band_node_domain = band_node.get_partial_schedule().domain();
    StatementMap all_statements = scop_info_.analysis_result_.GetStatementMap();
    isl::union_map reduce_statement_map = isl::union_map::empty(node.ctx());
    isl::union_set reduce_statements = isl::union_set::empty(node.ctx());

    for (auto it = all_reduce_map.begin(); it != all_reduce_map.end();) {
      reduce_statement_map = reduce_statement_map.unite(it->second.stmt_map);
      auto this_reduce = reduce_manager.GetReduceStatements(band_node_domain, reduce_statement_map, all_statements);
      if (!this_reduce.is_empty()) {
        reduce_statements = reduce_statements.unite(this_reduce);
        all_reduce_map.erase(it++);
      } else {
        ++it;
      }
    }

    if (reduce_statements.n_set() < 1) {
      return node;
    }

    isl::union_map dependences = pass_info_.dependences_.subtract(pass_info_.force_dependences_);
    auto node_bak = node;
    if (!reduce_manager.SplitReduceStatements(node, reduce_statements, dependences, true)) {
      return node_bak;
    }
    done_separate = all_reduce_map.empty();
    return node;
  };
  final_schedule = sch.get_root().map_descendant_bottom_up(GetInnerMostBand).get_schedule();
  if (done_separate) {
    final_schedule = InsertReduceMarker(final_schedule);
    final_schedule = RescheduleForReduce(final_schedule);
  }
  return final_schedule;
}

isl::schedule ReduceMappingStrategy::InsertReduceMarker(const isl::schedule &sch) {
  isl::schedule final_schedule = sch;
  auto all_reduce_map = scop_info_.analysis_result_.GetReduceTensorInfoMap();
  auto InsertMarker = [&all_reduce_map, this](isl::schedule_node node) -> isl::schedule_node {
    ReduceManager reduce_manager;
    auto band_node = node.as<isl::schedule_node_band>();
    if (!band_node) {
      return node;
    }

    for (auto it = all_reduce_map.begin(); it != all_reduce_map.end();) {
      isl::union_map reduce_statement_map = it->second.stmt_map;
      isl::id reduce_id = it->first;
      auto band_node_domain = band_node.get_partial_schedule().domain();
      auto op_type = scop_info_.analysis_result_.GetReduceOpType(reduce_id) + "_";

      StatementMap all_statements = scop_info_.analysis_result_.GetStatementMap();
      isl::union_set reduce_statements =
        reduce_manager.GetReduceStatements(band_node_domain, reduce_statement_map, all_statements);
      if (reduce_statements.n_set() != 1) {
        ++it;
        continue;
      }

      all_reduce_map.erase(it++);
      std::string reduce_marker_name =
        REDUCE_MARKER + op_type + reduce_id.get_name() + "_" + std::to_string(GetReduceId());
      auto reduce_node = band_node.insert_mark(reduce_marker_name);
      return reduce_node;
    }
    return band_node;
  };
  final_schedule = final_schedule.get_root().map_descendant_bottom_up(InsertMarker).get_schedule();
  return final_schedule;
}

isl::schedule ReduceMappingStrategy::RescheduleForReduce(const isl::schedule &sch) {
  auto IsContainCoincidentZero = [](const isl::schedule_node node) -> bool {
    if (!node.isa<isl::schedule_node_band>()) {
      return true;
    }

    auto band_node = node.as<isl::schedule_node_band>();
    for (int i = 0; i < static_cast<int>(band_node.n_member()); ++i) {
      if (band_node.member_get_coincident(i) == 0) {
        return true;
      }
    }
    return false;
  };

  auto SetAllCoincident = [](const isl::schedule_node node) -> isl::schedule_node {
    if (!node.isa<isl::schedule_node_band>()) {
      return node;
    }

    auto band_node = node.as<isl::schedule_node_band>();
    for (int i = 0; i < static_cast<int>(band_node.n_member()); ++i) {
      if (band_node.member_get_coincident(i) == 0) {
        band_node = band_node.member_set_coincident(i, 1);
      }
    }
    return band_node;
  };

  auto root = sch.get_root();
  auto node = root;
  root.foreach_descendant_top_down([&node](const isl::schedule_node &orig_node) -> bool {
    if (!GetMarkerName(orig_node, REDUCE_MARKER).empty() && orig_node.tree_depth() >= 2 &&
        orig_node.ancestor(2).isa<isl::schedule_node_sequence>()) {
      node = orig_node.ancestor(2);
      return false;
    }
    return true;
  });
  if (node.is_equal(root)) {
    return sch;
  }

  int child_number = static_cast<int>(node.n_children());
  Reschedule reschedule(scop_info_, pass_info_);
  for (int i = 0; i < child_number; ++i) {
    auto child_node = node.child(i);
    if (!child_node.isa<isl::schedule_node_filter>() || !child_node.has_children()) {
      continue;
    }

    // Ignore the related statements of the reduce operator.
    if (!GetMarkerName(child_node.child(0), REDUCE_MARKER).empty()) {
      continue;
    }

    // Ignore all related statements that coincide with 1.
    if (!IsContainCoincidentZero(child_node.child(0))) {
      continue;
    }

    auto active_domain = child_node.as<isl::schedule_node_filter>().get_filter();
    auto after_reschedule_node = reschedule.RescheduleSerializeSccs(active_domain, false).get_root();
    after_reschedule_node =
      after_reschedule_node.has_children() ? after_reschedule_node.child(0) : after_reschedule_node;

    // Adjust the coincident of the original schedule tree according to the result of the reschedule.
    bool is_seq =
      after_reschedule_node.isa<isl::schedule_node_sequence>() || after_reschedule_node.isa<isl::schedule_node_set>();
    if (!is_seq) {
      bool is_contain_coincient_zero = IsContainCoincidentZero(after_reschedule_node);
      node = is_contain_coincient_zero ? node : SetAllCoincident(child_node.child(0)).ancestor(2);
    } else {
      int j = 0;
      int reschedule_child_number = static_cast<int>(after_reschedule_node.n_children());
      for (; j < reschedule_child_number; ++j) {
        auto reschedule_child_node = after_reschedule_node.child(j);
        if (!reschedule_child_node.has_children() || IsContainCoincidentZero(reschedule_child_node.child(0))) {
          break;
        }
      }
      node = (j != reschedule_child_number) ? node : SetAllCoincident(child_node.child(0)).ancestor(2);
    }
  }
  return node.get_schedule();
}

bool ReduceMappingStrategy::NeedAtomicAdd(const isl::schedule_node_band &band, size_t n_block_map) {
  if (!scop_info_.user_config_.GetEnableAkgReduceLib()) {
    return false;
  }

  auto non_coin_start_idx = CountConsecutiveCoincident(band);
  bool is_all_reduce =
    band.n_member() == 1 && scop_info_.analysis_result_.GetReduceDirection() == X_DIRECTION && non_coin_start_idx == 1;
  if (is_all_reduce) {
    non_coin_start_idx = 0;  // Compare block size of position 0 to enable atomic add for all reduce ops
  }
  if (n_block_map < non_coin_start_idx) {
    return false;
  }

  auto block_cfg = scop_info_.user_config_.GetBlockConfig();
  CHECK(block_cfg != nullptr) << "block config is null";
  while (non_coin_start_idx < block_cfg->bound) {
    auto idx = block_cfg->bound - non_coin_start_idx - 1;
    if (block_cfg->GetAt(idx).second > 1) {
      return true;
    }
    ++non_coin_start_idx;
  }
  return false;
}

void ReduceMappingStrategy::MarkAtomicAddTensor(const isl::schedule_node_band &band) {
  auto target_stmt = scop_info_.analysis_result_.GetReduceWriteStmt(band);
  auto tensor = target_stmt.range();
  std::unordered_set<isl::id, isl::IslIdIslHash> stmt_ids;
  target_stmt.foreach_map(
    [this, &stmt_ids](const isl::map m) { stmt_ids.insert(m.get_tuple_id(isl_dim_type::isl_dim_in)); });
  tensor.foreach_set([this, &stmt_ids](const isl::set &s) -> void {
    for (auto it : scop_info_.analysis_result_.GetReduceTensorInfoMap()) {
      auto provide = static_cast<const Provide *>(it.second.stmt_node);
      if (stmt_ids.count(it.first) == 0 || provide->func->func_name() != s.get_tuple_name()) {
        continue;
      }
      auto type = scop_info_.analysis_result_.GetReduceOpType(it.first);
      scop_info_.analysis_result_.RecordAtomicTensors(AtomicInfo{s.get_tuple_name(), type});
    }
  });
}

size_t BatchMatmulMappingStrategy::MapThreadHelper(isl::schedule_node &thread_root) {
  auto warp_cfg = scop_info_.user_config_.GetReplaceConfig()[WARP_COMPUTE];
  CHECK(warp_cfg != nullptr) << "warp config is null";
  if (warp_cfg->bound < 1 || !thread_root.isa<isl::schedule_node_band>()) {
    return 0;
  }

  int start_node_depth = thread_root.get_tree_depth();
  // Determine max num dimension of threads that can be mapped.
  auto n_thread_map = CountConsecutiveCoincident(thread_root);
  if (n_thread_map < 1) {
    return 0;
  }
  n_thread_map = GetFinalMappingThreadNumber(thread_root, warp_cfg->bound, n_thread_map);

  // Map band under thread_root from inner dim to outer dim.
  Mapping mapping;
  thread_root = MapInnerDimToThreads(thread_root, false, warp_cfg, mapping, true);
  bool is_tiled = GetMarkerName(thread_root, THREAD_MARKER).empty();
  thread_root = is_tiled ? thread_root.child(0) : thread_root;
  thread_root = thread_root.del().insert_mark(isl::id(thread_root.ctx(), WARP_MARKER));

  int end_node_depth = thread_root.get_tree_depth() - start_node_depth;
  thread_root = thread_root.ancestor(end_node_depth);
  scop_info_.upa_node_mapping_.emplace_back(std::make_pair(thread_root, mapping));
  return warp_cfg->bound;
}

isl::schedule_node ConvMappingStrategy::ResetConvBlockMappingConfig(const isl::schedule_node &orig_node,
                                                                    MappingCfg *block_cfg, const bool check_extent) {
  if (!orig_node.isa<isl::schedule_node_band>()) {
    return orig_node;
  }
  const unsigned outer_band_axis_size = 4;
  auto node = orig_node;
  CHECK_GE(node.as<isl::schedule_node_band>().n_member(), outer_band_axis_size);

  // For the convolution operator, n axis is mapped to blockIdx.z, h axis and w axis are mapped to blockIdx.y, o axis
  // is mapped to blockIdx.x,
  node = node.as<isl::schedule_node_band>().split(1);
  auto new_cfg = std::to_string(block_cfg->GetZ().second);
  scop_info_.user_config_.RecordReplaceConfig(CONV_N, new_cfg, MappingType::REPLACE_BLOCKS);
  auto conv_o_block_cfg = scop_info_.user_config_.GetReplaceConfig()[CONV_N];
  node = MapBlockHelper(node, conv_o_block_cfg, 1, check_extent);

  node = node.child(0).child(0).as<isl::schedule_node_band>().split(2);
  auto partial_schedule = node.as<isl::schedule_node_band>().get_partial_schedule();
  partial_schedule = partial_schedule.intersect_domain(node.get_domain());
  auto upa_list = partial_schedule.get_union_pw_aff_list();
  auto extent_h = upa_list.get_at(0).floor().max_val().get_num_si() + 1;
  auto bind_block_h = std::min(static_cast<int>(extent_h), block_cfg->GetY().second);
  new_cfg = std::to_string(block_cfg->GetY().second / bind_block_h) + " " + std::to_string(bind_block_h);
  scop_info_.user_config_.RecordReplaceConfig(CONV_H_W, new_cfg, MappingType::REPLACE_BLOCKS);
  auto conv_h_w_block_cfg = scop_info_.user_config_.GetReplaceConfig()[CONV_H_W];
  node = MapBlockHelper(node, conv_h_w_block_cfg, 2, check_extent);

  node = node.child(0).child(0);
  new_cfg = std::to_string(block_cfg->GetX().second);
  scop_info_.user_config_.RecordReplaceConfig(CONV_O, new_cfg, MappingType::REPLACE_BLOCKS);
  auto conv_n_block_cfg = scop_info_.user_config_.GetReplaceConfig()[CONV_O];
  node = MapBlockHelper(node, conv_n_block_cfg, 1, check_extent);
  return node;
}

isl::schedule ConvMappingStrategy::MoveKernelHWBand(isl::schedule sch) {
  auto node = sch.root();
  isl::multi_union_pw_aff kh_mupa = isl::multi_union_pw_aff::zero(node.get_domain().get_space().set_from_params());
  isl::multi_union_pw_aff kw_mupa = kh_mupa;
  auto MapFromInner = [this, &kh_mupa, &kw_mupa](isl::schedule_node node) -> isl::schedule_node {
    if (!GetMarkerName(node, KH_KW_MARKER).empty()) {
      node = node.child(0);
      kh_mupa = node.as<isl::schedule_node_band>().get_partial_schedule();
      node = node.del();
      kw_mupa = node.as<isl::schedule_node_band>().get_partial_schedule();
      node = node.del();
      node = node.parent().del();
      return node;
    }
    if (!GetMarkerName(node, PROMOTE_GLOBAL_TO_SHARED_AB).empty()) {
      node = node.insert_mark(CONV_KHKW_OUTER).child(0);
      node = node.insert_partial_schedule(kw_mupa);
      node = node.as<isl::schedule_node_band>().set_permutable(1);
      node = node.insert_partial_schedule(kh_mupa);
      node = node.as<isl::schedule_node_band>().set_permutable(1);
      return node;
    }
    return node;
  };
  sch = sch.get_root().map_descendant_bottom_up(MapFromInner).get_schedule();
  return sch;
}

}  // namespace poly
}  // namespace ir
}  // namespace akg
