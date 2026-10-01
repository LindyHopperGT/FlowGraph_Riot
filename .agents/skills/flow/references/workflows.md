# Flow workflows

Which ops to call, in what order, for a given shape of request. `flow-ops.md` describes what each op
does; this file describes how they combine. Every archetype below applies to any `FlowAsset`
subclass, and most domain-specific requests decompose into one or more of them.

## The research loop

Work from cheap and broad to expensive and narrow. Each step should cut the search space before the
next one runs.

**A shipped encounter's full `ExportFlowAsset` can exceed the response size limit and be refused
outright.** Run the export *inside* a ToolScript and return a digest - one line per node/addon/
connection - instead of the raw document:

```python
import json
def run():
    r = epic_tool("FlowGraphCourier.FlowMCPToolset.ExportFlowAsset",
                  request={"assetPath": "..."})
    doc = json.loads(r["returnValue"]["exportedText"])
    lines = []
    for op in doc["ops"]:
        if op["kind"] == "UpsertNode":
            lines.append(f'N {op["guid"][:8]} {op["type"].split(".")[-1]} {op["properties"]}')
        elif op["kind"] == "UpsertAddon":
            lines.append(f'  A {op["guid"][:8]} {op["type"].split(".")[-1]} parent={op["parentGuid"][:8]}')
        elif op["kind"] == "AddConnection":
            lines.append(f'C {op["source"]["nodeGuid"][:8]}.{op["source"]["pin"]} -> '
                         f'{op["target"]["nodeGuid"][:8]}.{op["target"]["pin"]}')
    return {"lines": lines}
```

Two ToolScript quirks: `run()` must return a dict, not a list; and the sandbox dict rejects
`.get(key, default)` - use direct indexing.

1. `ListFlowAssetTypes` when you do not yet know which `FlowAsset` subclass the request concerns.
2. `FindFlowNodeTypes` with `assetClassName` set and the default `shape` sections, to see the
   categories and keywords available without pulling a single row.
3. The same op again, narrowed by `query`, `kind`, `keywords` or `categories`, at `brief`, to get
   candidate class names.
4. The same op once more at `full`, on the two or three classes that survived, for pins and
   properties. `full` includes the `deprecation` section - if the class you are about to recommend
   comes back `deprecated: true`, do not use it; look up `replacedBy` instead and settle on that
   class. `FindFlowNodeTypes` already excludes deprecated classes from a filtered query by default,
   so this mainly bites an exact `classNames[]` lookup (which always resolves regardless of the
   flag) or a class you found by reading existing content rather than by searching the catalog.
5. `FindFlowNodeUsage` on the class you settled on, to see how shipped content actually wires it.
6. `CheckAddonAttachmentEligibility` before committing to an addon, because the answer comes from a
   handshake rather than a rule you can derive.

### Which Find op answers which question

These four sound alike and answer different questions. Picking the wrong one wastes a round trip:

- **What classes exist, and which are legal here?** `FindFlowNodeTypes`. It searches *classes*,
  project-wide.
- **Which nodes are on this one asset?** `FindFlowNodes`. It searches *instances*, within one asset.
- **How is this class used in practice, and what pins does it really end up with?**
  `FindFlowNodeUsage`. It is the only view of pins an attached addon contributes to a live parent.
- **I need a node of this type to exist and I want its GUID.** `FindOrCreateFlowNode`. It is a
  mutation, not a query, and it is idempotent, so it will not mint a second node on a second run.

For a string rather than a structure - a comment, a property value, a title - use
`SearchFlowAssets` instead of any of them.

**Identifying one specific node by name or role is an instance question, not a class question -
answer it with `FindFlowNodes`, not `ExportFlowAsset`.** `FindFlowNodes`' `titleFilter` searches the
resolved, live state of each node (its `title`), which can reflect data a generic property export
never shows - some fields are read-only-by-reflection or editor-synced rather than plain editable
UPROPERTYs, so `ExportFlowAsset`/`FindFlowNodeTypes`' `properties` can legitimately come back empty
for a node whose state is not actually empty. Do not read an empty `properties` object as "this field
is unset" or "this data isn't reachable" - it may just mean the read-side reflection this tooling
walks doesn't cover that field. If `FindFlowNodes` can resolve the node you want (by title, class, or
entry-point flag), that settles the identity question in one call; reach for a full `ExportFlowAsset`/
`ExportFlowSubgraph` only after that, to read the resolved node's pins, connections and addons - and
reach for source only if usage (`FindFlowNodeUsage`) and instance search both leave a genuine class-
mechanics question unanswered (see Verifying a claim in `../SKILL.md`), not to explain why a property
came back empty.

## The edit loop

Every archetype below is a variation of this. Do not vary the order.

1. Read the current state: `ExportFlowAsset`, or `FindFlowNodes` plus `ExportFlowSubgraph` when the
   graph is large. You need real GUIDs and real pin names, not remembered ones.
2. Write the smallest Courier v2 document that achieves the request. Omission means "leave alone",
   so a small document is a safe one.
3. `ApplyFlowPatch` with `mutation.bDryRun: true`. Read `bHasBlockingError`, then
   `validationFindings`, then the `plan` - and check the plan says what you meant, not merely that
   nothing failed.
4. `ApplyFlowPatch` with `bDryRun: false`, once step 3 is clean or its findings are understood and
   acceptable.
5. `DiffFlowAsset` with the pre-edit export as `old` and the asset path as `new`, to confirm what
   changed. This exists so you do not have to compare two exports by eye.
6. `AutoFormatFlowGraph` with `selectionGuids` set to the patch's `plan.nodesAdded`, if it added
   nodes. Pass the new GUIDs, not the whole graph, unless a full relayout is what you want.

Step 3 is not optional. A dry run costs one call and is the only cheap way to find out that a
document says something other than what you intended.

## Archetype: create a sub-graph from an exact selection

Before selecting nodes, decide whether the extraction is for readability, reuse, or both. For a
reusable chunk, design the child contract first: use `Start` data pins for inputs,
`OutputDataPinDeclarations` plus `Set Graph Output`/`Finish` for results, and named
`CustomInput`/`CustomOutput` only for execution control. Prefer one narrow child over copied
sequences when the call sites differ only in those declared inputs and outputs. A `CustomInput`
signals an already-running child and is not by itself a reentrant function call; use separate
`SubGraph` instances or a domain-supported lifecycle when independent invocations are needed.

Use `FindFlowNodes` or `ExportFlowAsset` to obtain the top-level node GUIDs, then call
`PlanFlowSubgraphFromSelection` with a proposed `Subgraph_` asset name. Inspect `plan.errors`,
`plan.warnings`, the one incoming entry, and the generated interface names. This operation is
different from `ExportFlowSubgraph`: it does not discover a connected region and it does not
mutate anything.

If the plan is valid, call `CreateFlowSubgraphFromSelection` with the same fields and
`mutation.bDryRun: true`, with `mutation.bUseTransaction: true` and no existing editor transaction.
Confirm `mutation.bRolledBack` and that the result identifies the child asset and replacement node
without leaving a saved child package. Then repeat with
`mutation.bDryRun: false` and verify both `targetAssetPath` and `newAssetPath` with `ExportFlowAsset` or
`DiffFlowAsset`. The operation moves only exec connections across the boundary: it requires one
incoming execution connection, rejects data-pin crossings, and leaves source CustomInput,
CustomOutput, and Finish nodes outside the selection.
It cannot create a crossing data-pin interface; if the extracted abstraction needs data pins, create
or patch that child and parent interface separately after the extraction.

After applying, verify more than graph topology: inspect the generated asset's class and any
domain-specific asset configuration required by the parent. A subgraph is a separate asset package,
so parent configuration is not automatically inherited. Follow the domain skill for required
context properties before considering the refactor complete.

**Do not declare the edit done on `bApplySucceeded`/no blocking error alone.** That confirms the
apply call did not fail; it does not confirm the graph now has the connections you meant to make.
Check step 5's `DiffFlowAsset` output against your actual intent: every connection you meant to add
appears in `addedConnections`, every node you meant to keep reachable still has the exec links it
needs, and nothing you needed is sitting in `removedConnections` unexpectedly. A node that was
clearly supposed to end up wired into the graph but has no surviving connection to or from it is not
a finished edit, whatever the apply response said. If a node in the diff still has an unconnected pin
that your intent required to be connected, go back and connect it before reporting completion -
do not report a task done with a graph you have not looked at post-apply.

**Treat a connection that silently fails to persist as a possible reconciler bug, not a mistake to
paper over.** If `ApplyFlowPatch` reports success (or a dry run reports a clean plan) for a
connection, but a subsequent `ExportFlowAsset`/`DiffFlowAsset` shows that connection missing - with no
intervening op that would explain removing it - this may be a defect in the plugin's patch/reconciler
path rather than an authoring error. Do not silently retry, re-derive a workaround, or assume you
misread the GUIDs and move on without comment. Tell the user what you observed (the op you sent, the
apply/dry-run result, and the subsequent read that contradicts it) so a human can decide whether to
investigate or file it - this is exactly the kind of correctness gap that is expensive to leave
undiscovered.

## Archetype: change a property

Export or find the node, then send one `UpsertNode` op carrying its `guid` and only the properties
that change. Upsert is a merge, so unmentioned properties, pins, addons and position are all
preserved - there is no need to restate them. Confirm `metrics.nodesTouched` is 1.

For a change across many assets, repeat per asset. There is no cross-asset batch op.

## Archetype: insert a node between two others

Export for the upstream and downstream GUIDs and the exact pin names on the connection you are
splitting. Then one document with three ops: an `UpsertNode` introducing the new node by `newAlias`
with its `type` and pins, and two `AddConnection` ops wiring upstream into the alias and the alias
into downstream.

You do not need to remove the original connection. Adding a connection from an exec output pin that
already has a target retargets it, and the plan reports the superseded link under
`connectionsRemoved`. On the dry run, check `aliasMap` shows the alias resolved to a real GUID.

## Archetype: delete a node and heal the graph

Decide the healing behavior before writing anything: does the upstream node now feed the deleted
node's old target, or does the path simply end? That is a design decision. Ask rather than pick
silently when the context does not make it obvious.

Then one document with the `AddConnection` that bridges the gap and a `DeleteNode` for the node
itself. Ops apply in a fixed order by kind, with deletes last, so a document may connect around a
node it also deletes without any ordering care. On the dry run, confirm no connection still
references the deleted GUID; a leftover reference is a blocking error, not a silent success.

Do not stop at that GUID check alone - it only catches a dangling reference, not a stale editor
node left behind by the delete. Read `findings[]` for any `"Graph integrity: "`-prefixed entries
(sourced from `CollectGraphParityIssues`, also surfaced directly as `graphIntegrityIssues[]` on
`ExportFlowAsset`/`ReconstructFlowGraph`). These report the editor graph and the runtime node map
disagreeing: an editor node with no runtime counterpart, a runtime node with no editor node, two
editor nodes for one runtime node, or a wire that is drawn but will not execute (or the reverse).
An empty list is the actual confirmation that the delete is clean.

## Archetype: retarget a connection

One `AddConnection` op with the new target. Do not add a `RemoveConnection` for the old link, and do
not delete and re-add. On the dry run, expect exactly one added and one removed connection.

If instead you see a fan-out error, you connected a second target rather than moving the first - and
that error only fires for exec pins, because an exec output drives one target while a data output may
legitimately feed several.

## Archetype: attach an addon

Check eligibility first with `CheckAddonAttachmentEligibility`, and proceed only on
`TentativeAccept`. Then one `UpsertAddon` op naming the owner through `parentGuid` and the addon
through `newAlias`, with its `type` and properties. To nest an addon under another addon, give the
child a `parentAlias` pointing at the parent's alias in the same document; depth is expressed by
parentage, never by ordering or nesting.

Attaching an addon does not require restating the owner node. On the dry run, check `addonsAdded`
shows it under the owner you intended.

## Archetype: diagnose a symptom

Export and read the structure before theorising. Then check the shapes that are cheap to falsify:
an exec output pin with two targets, a connection referencing a node that no longer exists, an addon
sitting somewhere the eligibility handshake would reject, a property whose value is not what the
description implies.

Describe the finding in plain terms before proposing a fix, propose the fix, dry-run it, and get
agreement before applying. A symptom with no established cause is the case where a fix should be
agreed rather than assumed.

## Limits worth knowing before you promise an outcome

**Cloning copies whatever classes the reference used, deprecated or not.** A reference graph found
by pattern match or by grep is not guaranteed to be current - it may predate a node's replacement.
Before treating a clone as done, spot-check its node classes with `FindFlowNodeTypes`
(`classNames[]`, `sections: "deprecation"`); if one comes back `deprecated: true`, prefer a
different reference that already uses `replacedBy`, or say the graph needs the swap rather than
shipping it as-is - remember a class cannot be changed in place on an existing node (below).

**A node's class cannot be changed in place.** Preserving a GUID while swapping the class behind it
is not something the reconciler can do. Supplying a `type` that differs from the existing object's
class is a hard error, so this fails loudly rather than reporting success - but it still fails.
Rebuilding the node under a new GUID and rewiring it is the only route, and that loses whatever was
attached to the old one. Surface this rather than working around it silently.

**A freshly duplicated asset exports dangling connections until its graph is reconstructed.**
Duplicating a `FlowAsset` regenerates every node GUID, but leaves some connection targets pointing at
the *source* asset's GUIDs, so the duplicate exports references to nodes it does not contain. Call
`ReconstructFlowGraph` on the duplicate once to repoint them at the duplicate's own nodes.

Always call `ReconstructFlowGraph` on a duplicate before exporting it or building a patch from its
GUIDs. Verified by duplicating assets of two different `FlowAsset` subclasses and diffing the export
before and after reconstructing: connection targets change, node counts do not. It does not affect
every asset, and reconstructing is harmless when it was not needed. Skipping this no longer corrupts
the graph silently - the stale targets now fail validation naming the missing node - but it still
fails.

**`ReconstructFlowGraph` also repairs pre-existing ghost nodes.** It prunes any editor node whose
runtime counterpart is gone before it rewires anything, so running it on an asset that already has
a stale editor node (for example, one left behind by a delete performed before this pruning
existed) removes the ghost rather than preserving it. Check `graphIntegrityIssues[]` on the result
to confirm nothing is left disagreeing.

**Inline object properties can survive a copy still pointing at their old owner.** An instanced or
inline object property copied between assets can keep referencing the source asset's private
subobject. This does not surface as an apply error; it surfaces later as a save failure complaining
about an illegal reference to a private object. If a patch touches an inline object property,
confirm the destination asset still saves before calling the task done.

**A clean `ApplyFlowPatch` result is not proof the connection is actually there - re-read to confirm.**
See "Do not declare the edit done" in the edit loop above: a reported success is the reconciler's own
account of what it did, not an independent observation. If shipped content or a later session shows a
node left unconnected that an earlier apply's plan claimed to have wired, do not assume the earlier
agent simply forgot to send the op - re-check whether the connection ever reached the saved asset, and
say so to the user as a possible patch/reconciler defect if it did not, rather than quietly re-adding
it and moving on.

**Do not duplicate a FlowAsset into a path a rename just vacated.** A rename can leave a redirector
at the old path rather than truly freeing it; duplicating over that path can pick up a hidden
dependency on the renamed asset that survives a recompile and a forced re-save, and is not visible
to `ExportFlowAsset`, `DiffFlowAsset`, or graph reflection. If the path must be reused, delete the
asset at that path outright instead of renaming it aside, or confirm with the asset toolset that no
redirector remains before duplicating into it.
