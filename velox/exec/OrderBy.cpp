/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "velox/exec/OrderBy.h"
#include "velox/exec/OperatorUtils.h"
#include "velox/exec/Task.h"
#include "velox/vector/FlatVector.h"

namespace facebook::velox::exec {

namespace {
CompareFlags fromSortOrderToCompareFlags(const core::SortOrder& sortOrder) {
  return {
      sortOrder.isNullsFirst(),
      sortOrder.isAscending(),
      false,
      CompareFlags::NullHandlingMode::NoStop};
}
} // namespace

OrderBy::OrderBy(
    int32_t operatorId,
    DriverCtx* driverCtx,
    const std::shared_ptr<const core::OrderByNode>& orderByNode)
    : Operator(
          driverCtx,
          orderByNode->outputType(),
          operatorId,
          orderByNode->id(),
          "OrderBy",
          orderByNode->canSpill(driverCtx->queryConfig())
              ? driverCtx->makeSpillConfig(operatorId)
              : std::nullopt) {
  VELOX_CHECK(pool()->trackUsage());
  std::vector<column_index_t> sortColumnIndices;
  std::vector<CompareFlags> sortCompareFlags;
  sortColumnIndices.reserve(orderByNode->sortingKeys().size());
  sortCompareFlags.reserve(orderByNode->sortingKeys().size());
  for (int i = 0; i < orderByNode->sortingKeys().size(); ++i) {
    const auto channel =
        exprToChannel(orderByNode->sortingKeys()[i].get(), outputType_);
    VELOX_CHECK(
        channel != kConstantChannel,
        "OrderBy doesn't allow constant sorting keys");
    sortColumnIndices.push_back(channel);
    sortCompareFlags.push_back(
        fromSortOrderToCompareFlags(orderByNode->sortingOrders()[i]));
  }
  sortBuffer_ = std::make_unique<SortBuffer>(
      outputType_,
      sortColumnIndices,
      sortCompareFlags,
      outputBatchRows(), // TODO(gaoge): Move to where we can estimate the
                         // average row size and set the output batch rows based
                         // on it.
      pool(),
      &nonReclaimableSection_,
      &numSpillRuns_,
      spillConfig_.has_value() ? &(spillConfig_.value()) : nullptr,
      operatorCtx_->driverCtx()->queryConfig().orderBySpillMemoryThreshold());
}

void OrderBy::addInput(RowVectorPtr input) {
  sortBuffer_->addInput(input);
}

void OrderBy::reclaim(
    uint64_t targetBytes,
    memory::MemoryReclaimer::Stats& stats) {
  VELOX_CHECK(canReclaim());
  VELOX_CHECK(!nonReclaimableSection_);
  auto* driver = operatorCtx_->driver();

  // NOTE: an order by operator is reclaimable if it hasn't started output
  // processing and is not under non-reclaimable execution section.
  if (noMoreInput_) {
    // TODO: reduce the log frequency if it is too verbose.
    ++stats.numNonReclaimableAttempts;
    LOG(WARNING)
        << "Can't reclaim from order by operator which has started producing output: "
        << pool()->name()
        << ", usage: " << succinctBytes(pool()->currentBytes())
        << ", reservation: " << succinctBytes(pool()->reservedBytes());
    return;
  }

  // TODO: support fine-grain disk spilling based on 'targetBytes' after having
  // row container memory compaction support later.
  sortBuffer_->spill(0, targetBytes);
  // Release the minimum reserved memory.
  pool()->release();
}

void OrderBy::noMoreInput() {
  Operator::noMoreInput();
  sortBuffer_->noMoreInput();

  recordSpillStats();
}

RowVectorPtr OrderBy::getOutput() {
  if (finished_ || !noMoreInput_) {
    return nullptr;
  }

  RowVectorPtr output = sortBuffer_->getOutput();
  finished_ = (output == nullptr);
  return output;
}

void OrderBy::abort() {
  Operator::abort();
  sortBuffer_.reset();
}

void OrderBy::recordSpillStats() {
  VELOX_CHECK_NOT_NULL(sortBuffer_);
  const auto spillStats = sortBuffer_->spilledStats();
  if (spillStats.has_value()) {
    Operator::recordSpillStats(spillStats.value());
  }
}
} // namespace facebook::velox::exec
