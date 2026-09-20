#pragma once

// The canonical section/context order tables (36 §2.2). Every dsh numeric value
// is preserved for a shared name so prose ordering matches; ymh-local names take
// unused gap values and never reuse a dsh value. `section_order()` /
// `context_order()` map a registered name onto its order so config and
// registration sites do not hardcode the integers in two places. Unknown names
// throw `ConfigError` (36 §4.7).

#include <cstdint>
#include <string_view>

namespace ymh {

enum class SectionOrder : std::int32_t {
    // dsh values, verbatim (shared names).
    HarnessIdentity            = -1000,
    DeploymentPersonaPrefix    =     0,
    PlanPolicy                 =   500,
    TeamPolicy                 =   600,
    PtcOnly                    =   800,
    FileReference              =   900,
    ToolShell                  =  1000,
    ToolPwsh                   =  1010,
    ToolRead                   =  1100,
    ToolWrite                  =  1200,
    ToolEdit                   =  1300,
    ToolGlob                   =  1400,
    ToolGrep                   =  1500,
    ToolJobs                   =  1600,
    ToolPty                    =  1700,
    ToolWebSearch              =  2000,
    ToolWebFetch               =  2100,
    ToolLsp                    =  2200,
    ToolSessionQuery           =  2300,
    ToolGoal                   =  2400,
    ToolCordis                 =  2500,
    ToolWorkflow               =  2600,
    ToolRalph                  =  2700,
    ToolSubagent               =  2800,
    ToolReport                 =  2900,
    ToolsSdk                   =  5000,
    DeliverableFileReferences  =  9000,
    StructuredOutput           =  9900,
    HarnessSource              = 10000,
    WebSurface                 = 10100,
    DeploymentPersonaSuffix    = 10200,
    // ymh-local allocations in dsh gaps.
    ToolGit                    =  1050,
    ToolSkill                  =  1800,
    ToolPlan                   =  1900,
};

enum class ContextOrder : std::int32_t {
    SandboxPolicy       = 110,
    ApprovalPolicy      = 115,
    SubagentDelegation  = 120,
};

// Accepts either the dsh constant name (e.g. "HARNESS_IDENTITY") or the ymh
// registered section name (e.g. "harness:identity").
[[nodiscard]] std::int32_t section_order(std::string_view name);

// Accepts either the dsh constant name (e.g. "SANDBOX_POLICY") or the ymh
// registered context name (e.g. "sandbox:policy").
[[nodiscard]] std::int32_t context_order(std::string_view name);

} // namespace ymh
