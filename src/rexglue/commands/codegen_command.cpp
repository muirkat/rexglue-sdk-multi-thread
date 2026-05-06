/**
 * @file        rexglue/commands/codegen_command.cpp
 * @brief       Code generation command implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "codegen_command.h"

#include <filesystem>

#include <fmt/format.h>

#include <rex/codegen/codegen.h>
#include <rex/codegen/manifest.h>
#include <rex/codegen/project_recompiler.h>
#include <rex/logging.h>

namespace rexglue::cli {

Result<void> CodegenFromConfig(const std::string& config_path, const CliContext& ctx) {
  REXLOG_INFO("Generating code with config: {}", config_path);

  // Detect manifest vs legacy single-binary config
  if (rex::codegen::ManifestConfig::IsManifest(config_path)) {
    auto manifest = rex::codegen::ManifestConfig::Load(config_path);
    if (!manifest) {
      return Err<void>(rex::ErrorCategory::Config, "Failed to load manifest");
    }
    rex::codegen::ProjectRecompiler recompiler(std::move(*manifest));
    rex::codegen::ProjectRecompilerOptions opts{
        .targets = ctx.targets,
        .force = ctx.force,
        .enableExceptionHandlers = ctx.enableExceptionHandlers,
    };
    return recompiler.Run(opts);
  }

  // Legacy single-binary path
  auto pipeline = rex::codegen::CodegenPipeline::Create(config_path);
  if (!pipeline) {
    return Err<void>(pipeline.error());
  }

  if (ctx.enableExceptionHandlers) {
    pipeline->context().Config().generateExceptionHandlers = true;
    REXLOG_INFO("Exception handler generation enabled");
  }

  return pipeline->Run(ctx.force);
}

}  // namespace rexglue::cli
