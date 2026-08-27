// =============================================================================
// VORTEX-OS — Cost / Token Budgeting Tracker (C++/CLI, .NET 10)
// =============================================================================
// Persists per-dispatch token + cost to state/cost_log.jsonl, computes USD
// from .vortex/model_prices.json, and raises PENDING_HUMAN gates at 80% /
// 100% of a project's budget (resolved from env > project _meta.json > global
// default). Pure file I/O + JSON; no network calls.
//
// Per PRD-12 (docs/prd-phase2/prd-12-cost-token-budgeting.md).
// =============================================================================
#pragma once

#include "VortexCommon.h"

namespace Vortex {

    public ref class CostTracker abstract sealed {
    public:
        // Append one entry to state/cost_log.jsonl. File is created if missing.
        // Tokens are clamped to non-negative. Cost is computed from the price
        // table. Returns the cost_usd that was written (or 0 on failure).
        static double RecordTokens(Paths^ p,
                                   String^ taskId,
                                   String^ agent,
                                   String^ project,
                                   String^ model,
                                   int tokensIn,
                                   int tokensOut,
                                   int durationMs,
                                   array<String^>^ tags);

        // Compute the USD cost for a given model + token counts. Reads the
        // price table from .vortex/model_prices.json. Falls back to the
        // "default" entry if the model is unknown; falls back to 0 if no
        // price table exists. Pure function (no side effects).
        static double ComputeCost(Paths^ p,
                                   String^ model,
                                   int tokensIn,
                                   int tokensOut);

        // Sum the cost_log.jsonl for a given project. Returns total cost_usd
        // (0 if the file is missing or empty).
        static double ProjectCostSoFar(Paths^ p, String^ project);

        // Sum the cost_log.jsonl for a given project, optionally filtered by
        // --since (unix seconds). Returns total cost_usd.
        static double ProjectCostSince(Paths^ p, String^ project, long sinceUnix);

        // Resolve the budget for a project. Priority:
        //   1. $env:VORTEX_BUDGET_USD_TOTAL  (if set)
        //   2. <skill>/_meta.json -> .budgets.usd_total  (if present)
        //   3. $VORTEX_HOME/.vortex/budgets.json -> defaults
        //   4. 0 (no budget)
        // The (tokens, usd) pair are written to the out params.
        static void ResolveBudget(Paths^ p, String^ project,
                                  long% tokensTotal, double% usdTotal);

        // Check the budget for a project. If cost_so_far is at 80% of the
        // usd_total, yields a PENDING_HUMAN gate (severity MEDIUM) ONCE per
        // day per project. If at 100%, yields a gate (severity CRITICAL).
        // Returns 0 if no alert was raised, or 1 if the gate is pending
        // (the engine exits with 203 from the gate; the return value is for
        // unit tests).
        static int CheckBudget(Paths^ p, String^ project, String^ taskId);

        // Cost report: returns the formatted text table for the CLI.
        // If project is empty, sums across all projects.
        // If sinceUnix is 0, no time filter.
        static String^ FormatReport(Paths^ p,
                                    String^ project,
                                    long sinceUnix,
                                    bool asJson);

        // Path helpers (exposed for unit tests)
        static String^ CostLogFile(Paths^ p);
        static String^ ModelPricesFile(Paths^ p);
        static String^ BudgetsFile(Paths^ p);
    };
}
