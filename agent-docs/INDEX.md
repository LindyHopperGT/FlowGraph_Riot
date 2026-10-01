# FlowGraph Agent Knowledge Base - Index

$KB:flow:guide:flowgraph-index

## What this is

The agent-facing knowledge base for the **generic, upstream Flow Graph plugin** (`CoreSDK/Plugins/Runtime/ThirdParty/FlowGraph/`) - node/addon reference articles for classes that are not tied to any consuming project. This folder is a **sibling** to `FlowGraph/docs/` (the upstream Jekyll documentation website); do not confuse the two or nest content inside `docs/`.

## Master index

See the KB master index for the federated layout across all three roots: $KB:kb:guide:master-index

## Placement rule

A node/addon's article lives here **only if it is generic** (owned by this plugin, no consuming-project dependency). A consuming project's own project-specific nodes/addons are documented in that project's own KB root instead - see $KB:kb:guide:master-index.

**Any non-C++/Blueprint authoring language a consuming project's engine fork adds is that project's own addition, not part of upstream CoreSDK/ThirdParty Flow** - authoring guidance for it (generic or project-specific) belongs in the owning project's own KB root instead, never here. This plugin's `Authoring/` guide covers C++ and Blueprint only.

## Layout

- **No per-class node/addon articles.** A class's documentation lives on the class itself
  (`FFlowAgentDoc`, read via the catalog with `FindFlowNodeTypes`), not as a markdown file.
- `Concept` articles for generic ecosystem semantics (e.g. the Flow Courier format, the addon attachment handshake, subgraph composition).
- `Authoring/` guides for how to build a new generic node/addon.

## Conventions

Article template, cross-link tag scheme (`$KB:<domain>:<kind>:<id>`), and locked-block markup are defined once, KB-wide, in this repo's own KB conventions doc (see $KB:kb:guide:master-index for its location) - a consuming project adopting the `$KB:` protocol supplies its own copy. This index does not restate them.

## Available guides

- $KB:flow:guide:courier-text-format - the Courier v2 JSON document: what its fields mean and what
  a document does when applied (identity, apply order, merge behavior, addon parentage, pin names,
  the error model). The exact schema is published live by `describe_toolset`; read this for meaning,
  not shape.

## Scripts (`Scripts/`)

- `kb_lint.py` - generic KB consistency checker (dangling `$KB:` references, filename/tag
  mismatches, orphan articles) across any set of `agent-docs/` roots. Pure Python, no Unreal/MCP
  dependency. Ships with this plugin rather than a project-specific skill since it operates
  generically on whatever roots are passed to it.

## Concepts (`Concepts/`)

Generic ecosystem semantics that don't belong to any single node/addon class.

- $KB:flow:concept:subgraphs - `FlowNode_SubGraph` composition: embedding one `FlowAsset` inside
  another, the `CustomInput`/`CustomOutput` named-interface mechanism, and when to decompose a
  graph into a sub-asset vs. keep it inline.
- $KB:flow:concept:structuring-coherent-graphs - domain-agnostic composition guidance: node vs
  addon vs pin, multiple entry points vs branch inline, decomposition, readability habits. Project
  domain skills may layer their own domain-specific skeleton patterns on top of this.

## Authoring (`Authoring/`)

How to create new **generic** Flow nodes/addons (no consuming-project dependency). Project-specific
authoring lives in the owning project's root instead - see $KB:kb:guide:master-index.

- $KB:flow:guide:authoring-a-node - node vs addon, C++ vs Blueprint, the creation workflow for each
  (incl. the Flow-Blueprint-asset-type trap), and AddOn attachment eligibility (the two-sided
  accept/reject handshake).
- $KB:flow:concept:declaring-pins - the full pin surface: exec, static, data, auto-generated, and context pins.

## See also
- $KB:flow:guide:aiflowgraph-index
- $KB:kb:guide:master-index
