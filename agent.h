// SPDX-FileCopyrightText: 2026 Paolo Anzani
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "api.h"

namespace microcodex {

// Whether the agent tool of another MicroCodex started this process.
bool isSubAgent();

// Creates the filesystem and command tools used by the first coding agent,
// and the agent tool unless this process is already a sub-agent.
// The tool definitions are immutable. Callers should still avoid
// overlapping filesystem mutations when executing their calls in parallel.
std::vector<std::shared_ptr<const ToolBase>> makeCodingTools();

// Lets the agent tool start new sub-agents on api's current model and effort,
// so they follow /model and /effort. Call it before api's first turn; api must
// outlive every call of the tool. Without it, sub-agents start on MicroCodex's
// defaults.
void connectAgentTool(const CodexApi& api);

// Provides the model instructions, the global AGENTS.md and the tool set
// common to the terminal agent. Authentication is deliberately left to the
// caller.
CodexApiConfig makeCodingAgentConfig(std::string model);

}  // namespace microcodex
