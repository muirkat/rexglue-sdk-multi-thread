/**
 * @file        codegen/phase_gapfill.cpp
 * @brief       GapFill phase: find uncovered code regions and register them as functions
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "ppc/instruction.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/codegen/phases.h>
#include "phase_helpers.h"

#include <rex/logging.h>

#include "codegen_logging.h"
#include <rex/memory/utils.h>

#include <ppc.h>

using rex::codegen::ppc::decode_instruction;
using rex::codegen::ppc::Opcode;
using rex::memory::load_and_swap;

namespace rex::codegen {

namespace {

//=============================================================================
// GapFill to register uncovered code regions
//=============================================================================

// Split a code region into function segments based on terminators (blr, tail calls).
std::vector<CodeRegion> splitRegionOnTerminators(
    const CodeRegion& region, const BinaryView& binary,
    const std::unordered_set<uint32_t>& knownCallables) {
  std::vector<CodeRegion> segments;
  uint32_t segmentStart = region.start;

  for (uint32_t addr = region.start; addr < region.end; addr += 4) {
    const uint8_t* data = binary.translate(addr);
    if (!data)
      break;

    uint32_t raw = load_and_swap<uint32_t>(data);
    auto decoded = decode_instruction(addr, raw);
    bool shouldSplit = false;
    const char* reason = nullptr;

    // Check for terminators
    if (decoded.is_return()) {
      shouldSplit = true;
      reason = "blr";
    } else if (decoded.opcode == Opcode::b && decoded.branch_target.has_value()) {
      uint32_t target = decoded.branch_target.value();
      // Don't split on tail recursion (branch to own segment start)
      if (target != segmentStart && knownCallables.contains(target)) {
        shouldSplit = true;
        reason = "tail call";
      }
    }

    if (shouldSplit) {
      uint32_t segmentEnd = addr + 4;
      if (segmentEnd > segmentStart) {
        segments.push_back({segmentStart, segmentEnd});
        REXCODEGEN_TRACE("GapFill: split segment 0x{:08X}-0x{:08X} ({} at 0x{:08X})", segmentStart,
                         segmentEnd, reason, addr);
      }
      segmentStart = segmentEnd;
    }
  }

  // Handle remaining code after last terminator
  if (segmentStart < region.end) {
    segments.push_back({segmentStart, region.end});
  }

  return segments;
}

// Check if address looks like exception handler data (handler ptr + rdata ptr)
bool looksLikeExceptionData(const BinaryView& binary, const FunctionGraph& graph, uint32_t addr) {
  const uint8_t* data = binary.translate(addr);
  if (!data)
    return false;

  // Exception handler data pattern:
  // [addr+0]: pointer to __C_specific_handler (entry point)
  // [addr+4]: pointer to scope table in .rdata
  uint32_t firstDword = load_and_swap<uint32_t>(data);
  uint32_t secondDword = load_and_swap<uint32_t>(data + 4);

  // Check if first dword is a known entry point (like __C_specific_handler)
  if (!graph.isEntryPoint(firstDword)) {
    return false;
  }

  // Check if second dword points to .rdata section
  auto* rdataSection = binary.findSectionByName(".rdata");
  if (!rdataSection)
    return false;

  uint32_t rdataStart = rdataSection->baseAddress;
  uint32_t rdataEnd = rdataStart + rdataSection->size;

  if (secondDword >= rdataStart && secondDword < rdataEnd) {
    REXCODEGEN_TRACE(
        "GapFill: 0x{:08X} looks like exception data (handler=0x{:08X}, scope=0x{:08X}), skipping",
        addr, firstDword, secondDword);
    return true;
  }

  return false;
}

void gapFillCodeRegions(CodegenContext& ctx) {
  REXCODEGEN_TRACE("Analyze: checking for uncovered code regions...");

  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  auto& scan = ctx.scan;

  // Build set of known callables for tail call detection
  std::unordered_set<uint32_t> knownCallables;
  for (const auto& [addr, node] : graph.functions()) {
    knownCallables.insert(addr);
  }

  size_t gapsFound = 0;
  size_t segmentsCreated = 0;

  for (const auto& region : scan.codeRegions) {
    // Split region on terminators (blr, tail calls), then check each segment
    auto segments = splitRegionOnTerminators(region, binary, knownCallables);

    for (const auto& segment : segments) {
      // Skip if this segment's start is already a registered function entry
      if (graph.isEntryPoint(segment.start))
        continue;

      // Skip if this segment's start is inside another function
      if (auto* containingFunc = graph.getFunctionContaining(segment.start)) {
        continue;
      }

      // Skip if this looks like exception handler data (handler ptr + rdata ptr)
      if (looksLikeExceptionData(binary, graph, segment.start))
        continue;

      uint32_t segmentSize = segment.size();
      graph.addFunction(segment.start, segmentSize, FunctionAuthority::GAP_FILL, false);

      REXCODEGEN_TRACE("GapFill: registered sub_{:08X} (0x{:08X}-0x{:08X}, {} bytes)",
                       segment.start, segment.start, segment.end, segmentSize);
      segmentsCreated++;
    }

    gapsFound++;
  }

  if (segmentsCreated > 0) {
    REXCODEGEN_TRACE("Analyze: registered {} gap functions from {} regions", segmentsCreated,
                     gapsFound);
  } else {
    REXCODEGEN_TRACE("Analyze: no uncovered regions found");
  }
}

// Demand-driven gap fill: register a function at each unresolved branch target
// that expects a callable but has no owning entry, and return how many were
// added.
//
// Block discovery registers `bl` call targets as functions but leaves `b`
// tail-call targets to later resolution (phase_discover: "bl only, not b").
// When such a target is a thunk reachable only by `b` -- in particular the tail
// of an over-claiming neighbor whose registered [base,size) bounds still span
// it -- region-based gap fill never carves it, so emission bakes a runtime
// FATAL. Here we seed exactly those targets: addFunction() re-resolves the
// waiting branch as the entry appears.
//
// Surgical by construction: only addresses some branch actually targets are
// considered (data/padding is never a branch target), purely conditional `bc`
// branches are skipped (their targets are internal labels, not entries), and
// targets already inside another function's blocks are left alone (so real
// control flow is never split).
size_t fillUnresolvedBranchTargets(CodegenContext& ctx) {
  auto& graph = ctx.graph;
  auto& binary = ctx.binary();
  const auto& invalid = ctx.analysisState().invalidInstructions;

  // An address is an *interior label* -- a branch target inside some function's
  // body, not a function entry -- if any conditional branch targets it, or if it
  // is already a resolved internal label of some function. Registering a function
  // there splits a real function mid-body: block discovery starts from a non-entry
  // while the over-claiming neighbor still spans the same range, so both emit
  // overlapping code and walk into bytes that vary run-to-run -> silent crash on
  // the real manifest (e.g. 0x83117FB8, reached by both `bc` and `b`).
  //
  // The same address can be reached by BOTH a conditional branch and an
  // unconditional `b` (a `bc`/`b` pair to a shared label), so the per-record
  // "skip if this record is conditional" test below is not enough on its own --
  // the `b` record still slips the interior label through. Collect every
  // interior-label address up front and exclude it regardless of which record
  // proposes it.
  std::unordered_set<uint32_t> interiorLabels;
  for (const auto& [addr, node] : graph.functions()) {
    for (uint32_t label : node->labels())
      interiorLabels.insert(label);
    for (const auto& uj : node->unresolvedJumps()) {
      if (uj.isConditional)  // bc/beq/... target == interior label of some function
        interiorLabels.insert(uj.target);
    }
  }

  std::unordered_set<uint32_t> targets;
  for (const auto& [addr, node] : graph.functions()) {
    for (const auto& uj : node->unresolvedJumps()) {
      // Conditional, non-linking branches (bc/beq/...) target internal labels,
      // never function entries -- registering one would split real control flow.
      if (!uj.isCall && uj.isConditional)
        continue;

      uint32_t t = uj.target;
      if (interiorLabels.contains(t))
        continue;  // also a conditional/interior-label target -- not an entry
      if (graph.getFunction(t) || graph.isImport(t))
        continue;  // already an entry / import
      if (graph.getFunctionContaining(t))
        continue;  // inside another function's blocks -- not a new entry
      if (binary.isInImportExportRange(t) || !binary.isExecutable(t))
        continue;  // must be real executable code
      if (invalid.contains(t))
        continue;  // flagged as data masquerading as code

      targets.insert(t);
    }
  }

  for (uint32_t t : targets) {
    graph.addFunction(t, 4, FunctionAuthority::GAP_FILL, false);
    REXCODEGEN_TRACE("GapFill: demand-registered sub_{:08X} (unresolved branch target)", t);
  }
  return targets.size();
}

//=============================================================================
// Cleanup absorbed GAP_FILL functions
//=============================================================================

void cleanupAbsorbedGapFills(CodegenContext& ctx) {
  auto& graph = ctx.graph;

  // A GAP_FILL at `addr` is absorbed when some OTHER function covers `addr`.
  // containsAddress() always requires base <= addr, so any covering function has
  // base < addr -- which makes both original removal rules (higher authority, or
  // a lower-addressed GAP_FILL) collapse to the single test above. We answer it
  // for every GAP_FILL at once with a sweep line over coverage intervals instead
  // of the old O(functions^2) pairwise scan.
  //
  // Coverage intervals mirror containsAddress(): the whole [base, base+size) range
  // for empty-block / CONFIG / PDATA functions, otherwise each discovered block.
  enum EventKind { kStart = 0, kEnd = 1, kQuery = 2 };
  struct Event {
    uint32_t addr;
    EventKind kind;
    uint32_t owner;
  };

  std::vector<Event> events;
  events.reserve(graph.functionCount() * 2);

  for (const auto& [addr, node] : graph.functions()) {
    const bool wholeRange = node->blocks().empty() ||
                            node->authority() == FunctionAuthority::CONFIG ||
                            node->authority() == FunctionAuthority::PDATA;
    if (wholeRange) {
      events.push_back({node->base(), kStart, node->base()});
      events.push_back({node->base() + node->size(), kEnd, node->base()});
    } else {
      for (const auto& block : node->blocks()) {
        events.push_back({block.base, kStart, node->base()});
        events.push_back({block.end(), kEnd, node->base()});
      }
    }
    if (node->authority() == FunctionAuthority::GAP_FILL)
      events.push_back({addr, kQuery, addr});
  }

  // Sort by address; at equal address apply interval start/end before queries so
  // a query at X sees intervals with start <= X < end (half-open coverage).
  std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
    if (a.addr != b.addr)
      return a.addr < b.addr;
    return a.kind < b.kind;  // kStart, kEnd, then kQuery
  });

  std::unordered_map<uint32_t, int> activeByOwner;  // owner base -> active interval count
  int distinctActiveOwners = 0;
  std::vector<uint32_t> toRemove;

  for (const auto& ev : events) {
    switch (ev.kind) {
      case kStart:
        if (activeByOwner[ev.owner]++ == 0)
          ++distinctActiveOwners;
        break;
      case kEnd:
        if (--activeByOwner[ev.owner] == 0)
          --distinctActiveOwners;
        break;
      case kQuery: {
        // Absorbed if any active owner other than the GAP_FILL itself covers addr.
        int selfActive = activeByOwner[ev.owner] > 0 ? 1 : 0;
        if (distinctActiveOwners - selfActive > 0)
          toRemove.push_back(ev.addr);
        break;
      }
    }
  }

  for (uint32_t addr : toRemove) {
    graph.removeFunction(addr);
  }

  if (!toRemove.empty()) {
    REXCODEGEN_TRACE("Analyze: removed {} absorbed GAP_FILL functions", toRemove.size());
  }
}

}  // anonymous namespace

namespace phases {

VoidResult GapFill(CodegenContext& ctx, ProgressReporter* reporter) {
  (void)reporter;
  gapFillCodeRegions(ctx);

  // Discover blocks for region gap-filled functions
  {
    auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
    size_t discovered = discoverPendingFunctions(ctx, known);
    REXCODEGEN_TRACE("Analyze: discovered blocks for {} gap-filled functions", discovered);
  }

  // Demand-driven pass: claim unresolved branch targets that no entry owns, then
  // discover them. Iterate to a fixpoint -- a thunk's body can `b` to the next
  // thunk in a chain, and addFunction() re-resolves waiting branches as each
  // entry appears, exposing the next target. Cap rounds as a backstop.
  constexpr int kMaxRounds = 64;
  size_t totalDemand = 0;
  int round = 0;
  for (; round < kMaxRounds; ++round) {
    size_t added = fillUnresolvedBranchTargets(ctx);
    if (added == 0)
      break;
    totalDemand += added;

    auto known = buildKnownFunctions(ctx.graph, /*excludeGapFill=*/true);
    discoverPendingFunctions(ctx, known);
  }
  if (round == kMaxRounds)
    REXCODEGEN_WARN("GapFill: demand-driven pass hit round cap ({})", kMaxRounds);
  if (totalDemand > 0)
    REXCODEGEN_TRACE("Analyze: demand-filled {} unresolved branch targets over {} rounds",
                     totalDemand, round);

  cleanupAbsorbedGapFills(ctx);

  return Ok();
}

}  // namespace phases

}  // namespace rex::codegen
