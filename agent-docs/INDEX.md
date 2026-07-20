# FlowGraph Agent Knowledge Base - Index

$KB:flow:guide:flowgraph-index
signature: n/a (guide article, not code-derived)

## What this is

The agent-facing knowledge base for the **generic, upstream Flow Graph plugin** (`CoreSDK/Plugins/Runtime/ThirdParty/FlowGraph/`) - node/addon reference articles for classes that are not Meerkat-specific. This folder is a **sibling** to `FlowGraph/docs/` (the upstream Jekyll documentation website); do not confuse the two or nest content inside `docs/`.

## Master index

See the KB master index for the federated layout across all three roots: $KB:kb:guide:master-index

## Placement rule

A node/addon's article lives here **only if it is generic** (owned by this plugin, no RGI/MKT dependency). Meerkat-specific (`MKT*`) nodes/addons are documented in `Projects/Meerkat/agent-docs/Flow/` instead - see $KB:kb:guide:master-index.

## Layout (populated starting Increment 1.4)

- One article per generic node class (e.g. upstream `UFlowNode_*`).
- One article per generic addon class.
- `Concept` articles for generic ecosystem semantics (e.g. the Courier text format, the addon attachment handshake).

## Conventions

Article template, cross-link tag scheme (`$KB:<domain>:<kind>:<id>`), signature-line staleness diffing, and locked-block markup are defined once, KB-wide, in `Projects/Meerkat/agent-docs/Flow/GARDENING.md` and in `Projects/Meerkat/agent-docs/process/FLOW_CODEV_AGENT_PLAN.md` Section 4. This index does not restate them.

## Available guides

- $KB:flow:guide:courier-text-format - the `FlowGraphExport v1.0` grammar (property/pin/connection/addon syntax) plus the glossary's own markdown structure. Fetch this before writing or parsing Courier text.

## Authoring (`Authoring/`)

How to create new **generic** Flow nodes/addons (no MKT dependency). Project-specific authoring
lives in the owning project's root instead - see $KB:kb:guide:master-index.

- $KB:flow:guide:choosing-a-node-authoring-approach - node vs addon, C++ vs Blueprint decision guide.
- $KB:flow:guide:creating-a-node-in-cpp - native `UFlowNode`/`UFlowNodeAddOn` creation workflow.
- $KB:flow:guide:creating-a-node-in-blueprint - Blueprint creation, incl. the Flow-Blueprint-asset-type trap.
- $KB:flow:concept:declaring-pins - the full pin surface: exec, static, data, auto-generated, and context pins.
- $KB:flow:concept:addon-attachment-eligibility-for-authors - controlling which AddOns may attach (the two-sided accept/reject handshake).

## See also
- $KB:flow:guide:aiflowgraph-index
- $KB:kb:guide:master-index
