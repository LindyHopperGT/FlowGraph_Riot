# Flow Skill - Ops Layer (calling the live Flow MCP ops)

Live-editor layer of the `flow` skill (see `../SKILL.md` for the agnostic core): how to call the Flow
tools on the `unreal-mcp` server's `FlowGraphCourier.FlowMCPToolset` (backed by `UFlowMCPToolset`,
registered via Epic's `ToolsetRegistry`). Load only when you need a running Unreal Editor *and*
already know which op - otherwise read `workflows.md` first.

## Calling convention

Call ops via the `unreal-mcp` server's `call_tool`, with `toolset_name: "FlowGraphCourier.FlowMCPToolset"`
and `tool_name` set to the PascalCase op name (e.g. `"ExportFlowAsset"`, no toolset prefix). Wrap
arguments in a top-level `request` object; field names are camelCase (auto-generated from the
underlying UPROPERTY names), e.g.:

```json
{
  "tool_name": "ApplyFlowPatch",
  "toolset_name": "FlowGraphCourier.FlowMCPToolset",
  "arguments": { "request": { "targetAssetPath": "/Game/...", "mutationText": "...", "mutation": { "bDryRun": true } } }
}
```

`list_toolsets` confirms the toolset is registered; `describe_toolset` (with
`toolset_name: "FlowGraphCourier.FlowMCPToolset"`) returns the exact current input/output JSON
schema for every op - reflected live from the C++ structs, so it wins over this doc on any
disagreement.

Every mutating op takes a `mutation` object. `bDryRun` (like every other dry-run/transaction flag)
must be nested under `mutation` - `{ "mutation": { "bDryRun": true } }` - never at the top level of
`request`. The bridge validates arguments against the tool's input schema before dispatch and
rejects an unknown or misplaced key with a suggestion (e.g. a top-level `bDryRun` is rejected with
a pointer to `mutation.bDryRun`) rather than silently dropping it, so a rejected call is safe to
retry after fixing the path - it never applied. A dry run has two further preconditions and both
fail the call rather than quietly applying anyway: `bUseTransaction` must be true, because rollback
is how a dry run undoes itself, and no editor transaction may already be open around the call.

For a brand-new asset (no prior version), use `ImportAndRegraphFlowAsset` with a full Courier
document; never use it to edit an existing asset. Editing calls the ops below in `workflows.md`'s
"standard loop" order.

## Ops

- **`ExportFlowAsset`** - dumps a `UFlowAsset` to a Courier document in `exportedText`; its node
  GUIDs are what you pass to `ApplyFlowPatch`. The payload is JSON, so parse it as JSON rather than
  line by line. Reserve for whole-graph review; when the request already names or implies specific
  nodes of interest, prefer `ExportFlowSubgraph` scoped to those nodes - it returns a much smaller,
  independently re-importable document instead of the entire asset.
- **`ImportAndRegraphFlowAsset`** - parses a Courier document (in `flowGraphText`) and rebuilds the
  editor graph. **Full-graph replace, not the GUID-keyed reconciler**: every node is destroyed and
  recreated as a new `UObject`, even untouched ones - no identity/position/addon preservation. It
  always creates and fails if something already exists at the path, so it is not an editing op. No
  dry-run support. Reserve for genuinely new assets or a deliberate full rewrite; use
  `ApplyFlowPatch` to edit.
- **`ApplyFlowPatch`** (`targetAssetPath`, `mutationText`, `mutation: { bDryRun }`) - GUID-keyed
  reconciler: applies only what `mutationText` names; everything else (identity, position, untouched
  addons) is preserved. `mutationText` carries a Courier **JSON** document despite the field name.
  Supports upsert-by-GUID, delete ops, `newAlias` for nodes that do not exist yet, and pre-commit
  validation (any `Error` finding blocks a non-dry-run apply, asset left untouched).
  Always dry-run first. Returns `assetPath`, `bApplySucceeded`, `plan`
  (nodes/connections/addons added/updated/deleted, `bIsEmpty`), `aliasMap`, `findings`,
  `validationFindings[]{severity, code, message, nodeGuid, addonGuid, pinName}`, `bHasBlockingError`,
  `metrics{nodesTouched, nodesPreserved, inputSizeBytes}`, `mutation{bDryRun, bSaved, bRolledBack,
  bTransactionOwned, bTransactionCommitted, checkedOutPackages[], modifiedPackages[]}`. On failure
  `plan`/`findings` are still present alongside `bApplySucceeded: false` - check
  `validationFindings` for why - branch on `code` (a stable identifier) rather than matching
  `message` text. Adding `A.Out -> C.In` when `A.Out -> B.In` exists retargets an **exec** pin
  without a delete op (plan reports both); a data output pin may legally feed several inputs and is
  not retargeted. Mutating ops check out the target file themselves and fail if checkout fails -
  there is no manual source-control step to perform first.
- **`AutoFormatFlowGraph`** (`targetAssetPath`, `selectionGuids[]`, `mutation: { bDryRun }`) -
  deterministic layered left-to-right format; unlike `ApplyFlowPatch`'s placement (new nodes only),
  this recomputes and overwrites position for every targeted node. Empty `selectionGuids` formats
  the whole graph. Returns `assetPath`, `nodeCount`, `positions{guid: {x, y}}`, `unresolvedGuids[]`,
  `mutation{...}`. Pass `ApplyFlowPatch`'s `plan.nodesAdded` to tidy only new nodes.
- **`ReconstructFlowGraph`** (`targetAssetPath`, `mutation: { bDryRun }`) - rebuilds a `FlowAsset`'s
  editor graph from its runtime nodes/connections. This is what closes the "freshly duplicated asset
  exports dangling connections" limit below - call it once right after duplicating, and skip opening
  the asset in the Flow editor by hand. Returns `assetPath`, `nodeCount`, `mutation{...}`.
- **`ExportFlowCatalog`** (`outputFilePath`, `assetClassName?`) - exports every node/addon class
  known to the catalog to a JSON file on disk, for offline analysis such as `kb_lint.py --catalog`'s
  bidirectional pattern/concept linkage and stem-collision checks. Not a live-editing op and not
  useful for a normal research/edit loop - reach for `FindFlowNodeTypes` for that. Returns
  `outputFilePath`, `bSucceeded`.
- **Duplicating a FlowAsset** is `AssetTools.duplicate(path, new_path)` on the `editor_toolset`
  server's asset toolset, not a `FlowGraphCourier.FlowMCPToolset` op. Follow it with
  `ReconstructFlowGraph` on the new path before exporting or patching it.
- **`DiffFlowAsset`** (`old`, `new`) - structured diff of two Flow graphs, no editor UI. Either arg
  is an asset path or a raw Courier document. Returns
  `bHasDifferences`, `addedNodes[]`, `removedNodes[]`, `changedNodes[]{nodeGuid,
  changedProperties{}, bPosChanged, oldPos?, newPos?, addonDiffs[]{addonGuid, parentGuid,
  changeType, addonType, changedProperties{}}}`, `addedConnections[]`, `removedConnections[]`. Use
  instead of comparing two full exports by hand.
- **`FindFlowNodes`** (`assetPath`, `titleFilter`, `classFilter`, `bEntryPointsOnly`) - lightweight
  summaries (`nodeGuid`, `title`, `classPath`, `bIsEntryPoint`) filtered by title substring, class,
  and/or entry-point status. Prefer over a full `ExportFlowAsset` to locate nodes on a large graph.
- **`ExportFlowSubgraph`** (`assetPath`, `startNodeGuid`) - exports just the connected region
  (undirected, exec and data edges) reachable from a start node: a bounded, independently
  re-importable Courier document for reading a large graph in pieces. Because reachability is
  undirected it also picks up nodes that only reach the start point upstream through a data pin.
- **`PlanFlowSubgraphFromSelection`** (`targetAssetPath`, `selectionGuids[]`, `newAssetName`) -
  read-only preflight for the editor's exact "create sub-graph from selection" operation. The
  selection is a list of top-level node GUIDs, not a connected-component query. It returns
  `plan{bCanApply, selectedNodeCount, entryCount, exitCount, errors[], warnings[],
  interfaceInputs[], interfaceOutputs[]}`. The planner requires exactly one incoming exec link,
  rejects data links crossing the boundary and rejects interface/Finish nodes.
- **`CreateFlowSubgraphFromSelection`** (`targetAssetPath`, `selectionGuids[]`, `newAssetName`,
  `mutation`) - applies the same collapse without Slate or editor-selection state. Plan first,
  then dry-run this operation, then apply it. The result contains `newAssetPath`,
  `subgraphNodeGuid`, `bApplySucceeded`, the same `plan`, and a two-package `mutation` report.
  `bSave: false` leaves both packages dirty in memory; `targetChangelist` applies to both when
  saving. The mutation must own its transaction (`bUseTransaction: true`) and cannot run inside an
  existing editor transaction. A dry run owns that transaction, rolls the source graph back, and
  removes the generated child asset. This is a mutation operation, unlike `ExportFlowSubgraph`.
- **`FindOrCreateFlowNode`** (`targetAssetPath`, `nodeType`, `matchProperties{}`,
  `mutation: { bDryRun }`) - finds a single `nodeType` node whose properties exactly match
  `matchProperties`, else creates one. Returns `bCreated` and the matched/created `nodeGuid`.
- **`SearchFlowAssets`** (`query`, `flags`, `scope`, `maxDepth?`, `contextAssetPath?`,
  `pinDirection?`, `pinConnection?`) - free-text search across one or all Flow assets, over titles,
  comments, class names, property names and values. `flags` is a bitfield selecting which surfaces
  to search and `scope` selects how wide to go; `maxDepth` bounds subgraph recursion. The two
  scopes that are relative to a single asset require `contextAssetPath` and error without it.
  `query` also matches the `Pin Names` category; use `pinDirection` (`any`/`input`/`output`) and
  `pinConnection` (`any`/`connected`/`unconnected`) to qualify pin-name matches. Results include
  `matchedPins[]{pinName, direction, bConnected}`. Reach for this when looking for a **string**
  rather than a structure - use `FindFlowNodes`/`FindFlowNodeTypes` for structure.
- **`find_assets`** / **`get_asset_summary`** / **`diff_asset`** - general asset discovery and
  diffing, and separate `unreal-mcp` toolsets rather than part of
  `FlowGraphCourier.FlowMCPToolset`. For a known or partial asset name prefer `find_assets` over a
  codebase-search subagent - it resolves a named lookup in one call; reserve codebase search for
  open-ended "how does X work" questions.
- **`CheckAddonAttachmentEligibility`** (`parentClassName`, `addonClassName`) - whether an AddOn can
  attach, via the same two-sided handshake the editor uses (parent and child each get a say).
  Accepts short or full class names. Returns `bEligible`, `result`
  (`TentativeAccept`/`Reject`/`Undetermined`), `reason`. Only `TentativeAccept` is eligible -
  `Undetermined` counts as ineligible. Editor-only (`WITH_EDITOR`).
- **`ListFlowAssetTypes`** (no params) - the cold-start entry point: one row per concrete
  `FlowAsset` subclass with `className`, `classPath`, `description`, `keywords`,
  `allowedNodeCount`, `allowedAddonCount`. Counts only, never the member name arrays - call
  `FindFlowNodeTypes` with `assetClassName` set to see the actual allowed classes. Use this first
  if you don't yet know which `FlowAsset` subclass to scope to.
- **`FindFlowNodeTypes`** (`assetClassName?`, `classNames[]?`, `kind?`, `query?`, `keywords[]?`,
  `categories[]?`, `articles[]?`, `pinTypes[]?`, `origin?`, `sections?`, `bIncludeDeprecated?`,
  `limit?`, `offset?`) - search the project catalog of Flow node/addon **classes**. One
  filter-then-project query covers both "what is legal on this asset type" and "tell me everything
  about this exact class" - **not** to be confused with `FindFlowNodes` above, which finds node
  **instances** on one existing asset.
  - `classNames[]` is exclusive (only filter applied when non-empty) - the deep-dive-by-exact-name
    case. Lenient input: accepts a short stem, full short name, or full class path. An ambiguous stem
    resolves nothing and returns `ambiguousClassNames[]{stem, candidates[]}` - it never guesses.
  - `assetClassName` restricts to classes legal on that `FlowAsset` subclass - the legality-list
    case; `kind` is `"node"|"addon"|"any"` (default `any`); `query` is a case-insensitive substring
    over name+description; `keywords[]`/`categories[]` match if the class carries ANY listed value;
    `articles[]` matches a `pattern:<slug>`/
    `concept:<slug>` the class's hand-authored doc carries; `pinTypes[]` reuses the existing pin-type
    filter (nodes only; addons can carry pins too, contributed to their parent, so a non-empty filter
    no longer blanket-excludes every addon); `origin` scopes to one authoring track -
    `describe_toolset` publishes the values this project actually supports. Deprecated classes are
    excluded by default; `bIncludeDeprecated: true` opts them back in for a filtered query (an exact
    `classNames[]` lookup always resolves one regardless of the flag).
  - `sections` (comma-separated, or a preset) controls what's returned - **each section is
    independently gated, so ask only for what you need**:
    - `"shape"` (default preset, cheapest) - category/keyword/tag/article histograms with counts and
      **no rows at all**. Start here on an unscoped or newly-explored query.
    - `"names"` - implied by every other row-bearing section; the class identifier (a short stem,
      full name on a same-response collision) plus the always-present authoritative `classPath`.
    - `"doc"` - reflected `description` + the hand-authored `doc.guidance`/`doc.tags`/`doc.hasDoc`.
      `"brief"` preset = `names` + `description` only, one line each (no `doc` object).
    - `"properties"` - a flat `properties[]` array covering the full ancestor chain (not just the
      queried class), each row tagged with `declaringClass` and a terse stringified `value`.
    - `"pins"` - `inputPins[]`/`outputPins[]`, read through the class default object's catalog-pin
      hook, so pins generated at class-construction time are included, not just static `InputPins`/
      `OutputPins` arrays.
    - `"articles"` - the `pattern:<slug>`/`concept:<slug>` list the class's doc carries.
    - `"usage"` - shipped-instance counts and example assets from the usage index (see
      `FindFlowNodeUsage` below for the fuller op). Omitted from the `"full"` preset - ask for it
      explicitly, since building it is comparatively expensive.
    - `"origin"` - which authoring track the class comes from.
    - `"deprecation"` - `deprecated`/`replacedBy`.
    - `"full"` preset = every section above except `usage`. **Hard-capped at 25 matches - errors
      instead of truncating** if the filter is too broad; narrow it rather than expecting truncation.
    - **Scope limit:** for a Blueprint-authored node (`_C` class path),
      `properties` reports only the flow-pin-oriented surface, not the Blueprint's own CDO member
      variables. If a node's behavior implies a property this op doesn't list, call
      `get_asset_summary(asset_path=<path without _C>)` and check `variables[]` before concluding it
      doesn't exist.
  - Returns `nodes[]`/`addons[]` (split by kind), `nodeCount`/`addonCount`, `totalNodeCount`/
    `totalAddonCount` (pre-paging), `truncated`, `effectiveSections` (echoed back), `shape{}` (only
    when requested), `unresolvedClassNames[]`/`ambiguousClassNames[]` (never silently dropped/guessed).
  - **Recommended cold-start recipe for a no-KB project:** `ListFlowAssetTypes` ->
    `FindFlowNodeTypes(assetClassName=X)` (default `shape`) to see the shape before committing to a
    row-bearing call - never `sections="full"` with no filter.
  - **Avoid re-fetching an already-known class:** once a `names`/`shape` discovery query has given
    you the `classPath`, go straight to a `classNames[]` lookup for the sections you still need
    rather than re-running the broad filter with more sections turned on.
  - **The emitted schema marks every field `required`, but this op does not enforce that.** The
    bridge deliberately accepts omitted keys and falls back to their defaults - see the comment in
    `RGIUnrealMCPEpicBridgeModule.cpp` near `CollectUnknownArgumentKeys`. A partial request such as
    `{"classNames": [...], "sections": "names"}` is normal, supported usage, not an error waiting to
    happen; do not pad a call with every declared field just because the schema lists them all.
- **`SetFlowAgentDoc`** (`className`, `guidance?`, `tags[]?`, `articles[]?`,
  `mutation:{bDryRun,bSave,...}`) - writes the hand-authored documentation layer onto one class.
  **Origin decides what actually happens:** a Blueprint (`_C`) class is checked out and saved for
  real (`persisted: true`); a native or scripted class cannot be authored at runtime, so the op
  reports `persisted: false` and returns the exact source to paste
  (`sourceSnippet`) - a `GetAgentDoc()` C++ override for a native class, or the equivalent
  editor-only defaults block for a scripted one. Never treat a `persisted: false` response as a
  completed write.
- **`FindFlowNodeUsage`** (`className`, `limit?`, `bIncludeSnippets?`) - where a class is actually
  used across the project's Flow Assets: instance count, distinct asset count, smallest containing
  assets first, and the classes most often attached alongside it. The only place to see pins an addon
  contributes to a live parent instance (a class default object can't show that). Snippets are the
  expensive part of the payload and are opt-in only.

Mining corpus usage examples for KB articles is a KB-*authoring* concern, not a live-editing one -
that recipe lives in a project's own domain skill, not here.

**Not yet available:** spawn-summary ops, planned as a project-specific domain-skill layer if needed
(`FindOrCreateFlowNode` already covers the generic create-node case).

**Tool placement:** these ops live in `FlowGraphCourier`'s `UFlowMCPToolset` (the
`FlowGraphCourier.FlowMCPToolset` toolset on the `unreal-mcp` server). Update this file if that
transport migrates.
