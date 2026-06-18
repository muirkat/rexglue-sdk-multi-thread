/**
 * @file        tests/unit/codegen/phase_validate_test.cpp
 * @brief       Unit tests for the Validate analysis phase
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * These exercise the phase against synthetic binaries built with
 * BinaryView::fromSections(), so no XEX/Module is required. The focus is the
 * conditional-branch (bc) resolution check: a bc whose target lands past a
 * truncated function end must fail at codegen time rather than baking a runtime
 * REX_FATAL into the generated C++.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/codegen/analysis_errors.h>
#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/function_types.h>
#include <rex/codegen/phases.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kTextBase = 0x10000000;

// Store a big-endian (guest PPC) instruction word at byte offset `off`.
void put32be(std::vector<uint8_t>& mem, size_t off, uint32_t word) {
  mem[off + 0] = static_cast<uint8_t>(word >> 24);
  mem[off + 1] = static_cast<uint8_t>(word >> 16);
  mem[off + 2] = static_cast<uint8_t>(word >> 8);
  mem[off + 3] = static_cast<uint8_t>(word >> 0);
}

// bc BO,BI,target -- opcode 16. BO=12 ("branch if CR bit set"), BI=0, AA=LK=0.
// `relBytes` is the signed byte displacement from the instruction to the target
// (must be 4-aligned and fit the 14-bit BD field).
uint32_t encodeBc(int32_t relBytes) {
  uint32_t bd = static_cast<uint32_t>(relBytes) & 0xFFFC;
  return (16u << 26) | (12u << 21) | (0u << 16) | bd;
}

// bl target -- opcode 18, LK=1. `relBytes` fits the 24-bit LI field.
uint32_t encodeBl(int32_t relBytes) {
  uint32_t li = static_cast<uint32_t>(relBytes) & 0x03FFFFFC;
  return (18u << 26) | li | 1u;
}

// Build a context over a single executable .text section of `sizeBytes` zeros
// (zero decodes to a harmless opcode the validator ignores).
CodegenContext makeContext(std::vector<uint8_t> text) {
  std::vector<BinaryView::SectionSpec> specs;
  specs.push_back(BinaryView::SectionSpec{
      .name = ".text", .baseAddress = kTextBase, .data = std::move(text), .executable = true});
  auto size = static_cast<uint32_t>(specs[0].data.size());
  auto view = BinaryView::fromSections(kTextBase, size, std::move(specs));
  return CodegenContext::Create(std::move(view), RecompilerConfig{});
}

size_t unresolvedCount(const CodegenContext& ctx) {
  return ctx.errors.Count(AnalysisErrors::Category::UnresolvedCall);
}

}  // namespace

TEST_CASE("Validate flags a bc whose target is past a truncated function end",
          "[codegen][phase_validate]") {
  // Function declared [base, base+0x100), but a bc at base+0x80 targets
  // base+0x140 -- 0x40 past the declared end, owned by no function. This is the
  // exact class that used to bake REX_FATAL("Unresolved branch ...") at runtime.
  std::vector<uint8_t> text(0x200, 0);
  const uint32_t site = kTextBase + 0x80;
  const uint32_t target = kTextBase + 0x140;
  put32be(text, 0x80, encodeBc(static_cast<int32_t>(target - site)));

  auto ctx = makeContext(std::move(text));
  ctx.graph.addFunction(kTextBase, 0x100, FunctionAuthority::CONFIG);
  ctx.graph.addBlockToFunction(kTextBase, Block{kTextBase, 0x100});

  auto result = phases::Validate(ctx);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(unresolvedCount(ctx) >= 1);
}

TEST_CASE("Validate accepts a bc to an internal label", "[codegen][phase_validate]") {
  // bc at base+0x80 targets base+0xC0, inside the function's own block -> goto.
  std::vector<uint8_t> text(0x200, 0);
  const uint32_t site = kTextBase + 0x80;
  const uint32_t target = kTextBase + 0xC0;
  put32be(text, 0x80, encodeBc(static_cast<int32_t>(target - site)));

  auto ctx = makeContext(std::move(text));
  ctx.graph.addFunction(kTextBase, 0x100, FunctionAuthority::CONFIG);
  ctx.graph.addBlockToFunction(kTextBase, Block{kTextBase, 0x100});

  auto result = phases::Validate(ctx);

  REQUIRE(result.has_value());
  REQUIRE(unresolvedCount(ctx) == 0);
}

TEST_CASE("Validate accepts a bc tail-call with a recorded call edge",
          "[codegen][phase_validate]") {
  // bc at A+0x80 targets B's entry; a tail-call edge is recorded at the site, so
  // emission resolves it to a conditional tail call.
  const uint32_t fnA = kTextBase;
  const uint32_t fnB = kTextBase + 0x1000;
  std::vector<uint8_t> text(0x1200, 0);
  const uint32_t site = fnA + 0x80;
  put32be(text, 0x80, encodeBc(static_cast<int32_t>(fnB - site)));

  auto ctx = makeContext(std::move(text));
  ctx.graph.addFunction(fnA, 0x100, FunctionAuthority::CONFIG);
  ctx.graph.addBlockToFunction(fnA, Block{fnA, 0x100});
  auto* b = ctx.graph.addFunction(fnB, 0x100, FunctionAuthority::DISCOVERED);
  ctx.graph.addBlockToFunction(fnB, Block{fnB, 0x100});
  ctx.graph.addTailCallToFunction(fnA, site, CallTarget::function(b));

  auto result = phases::Validate(ctx);

  REQUIRE(result.has_value());
  REQUIRE(unresolvedCount(ctx) == 0);
}

TEST_CASE("Validate flags a bc to a function entry with no recorded call edge",
          "[codegen][phase_validate]") {
  // The subtle case that distinguishes the precise (emission-mirroring) check
  // from a loose "is the target inside any function?" test: the target IS another
  // function's entry, but no call edge was recorded at the site, so emission
  // would bake a FATAL. Validate must reject it.
  const uint32_t fnA = kTextBase;
  const uint32_t fnB = kTextBase + 0x1000;
  std::vector<uint8_t> text(0x1200, 0);
  const uint32_t site = fnA + 0x80;
  put32be(text, 0x80, encodeBc(static_cast<int32_t>(fnB - site)));

  auto ctx = makeContext(std::move(text));
  ctx.graph.addFunction(fnA, 0x100, FunctionAuthority::CONFIG);
  ctx.graph.addBlockToFunction(fnA, Block{fnA, 0x100});
  ctx.graph.addFunction(fnB, 0x100, FunctionAuthority::DISCOVERED);
  ctx.graph.addBlockToFunction(fnB, Block{fnB, 0x100});
  // Deliberately no addTailCallToFunction.

  auto result = phases::Validate(ctx);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(unresolvedCount(ctx) >= 1);
}

TEST_CASE("Validate flags a bl to an address in no function", "[codegen][phase_validate]") {
  // Sanity check that the harness also drives the unconditional b/bl path.
  std::vector<uint8_t> text(0x200, 0);
  const uint32_t site = kTextBase + 0x80;
  const uint32_t target = kTextBase + 0x4000;  // outside any function
  put32be(text, 0x80, encodeBl(static_cast<int32_t>(target - site)));

  auto ctx = makeContext(std::move(text));
  ctx.graph.addFunction(kTextBase, 0x100, FunctionAuthority::CONFIG);
  ctx.graph.addBlockToFunction(kTextBase, Block{kTextBase, 0x100});

  auto result = phases::Validate(ctx);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(unresolvedCount(ctx) >= 1);
}
