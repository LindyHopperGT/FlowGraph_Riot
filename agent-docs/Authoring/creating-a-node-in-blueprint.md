# Creating a Node/AddOn in Blueprint (Guide)

$KB:flow:guide:creating-a-node-in-blueprint
signature: n/a (guide article, not code-derived)
keywords: create new node addon author blueprint asset type factory flow node blueprint gotcha palette

## What this is

The top-level workflow for authoring a `UFlowNode`/`UFlowNodeAddOn` as a **Blueprint**, and the one
trap that catches everyone.

## The trap (read first)

**A plain Blueprint that subclasses `UFlowNode` will never appear in the Flow graph editor's node
palette.** Flow uses its own Blueprint asset classes - `UFlowNodeBlueprint` and
`UFlowNodeAddOnBlueprint` - and both return `SupportedByDefaultBlueprintFactory() -> false`, so the
standard "Blueprint Class" content-browser picker cannot create the right asset. The graph schema
discovers Blueprint nodes/addons by filtering the asset registry **specifically for those two asset
classes** (`UFlowGraphSchema::GatherNodes`), so a normal Blueprint asset is invisible to it no matter
what it derives from.

## Workflow

1. Create the asset via the **Flow** asset category (the "Flow Node Blueprint" / "Flow Node AddOn
   Blueprint" asset-type actions), *not* the generic Blueprint Class option. This routes through
   `UFlowNodeBlueprintFactory` / `UFlowNodeAddOnBlueprintFactory`.
2. The factory shows a **parent-class picker** - choose the `UFlowNode` (or `UFlowNodeAddOn`)
   subclass to derive from. It must be a `CanCreateBlueprintOfClass` class descending the correct
   default parent, or the factory refuses.
3. The new Blueprint is seeded with default `ExecuteInput` and `Cleanup` event nodes (if
   `bSpawnDefaultBlueprintNodes` is on). Implement your logic in the EventGraph off those events.
4. Declare pins - see $KB:flow:concept:declaring-pins. In Blueprint you get data pins by adding a
   `FFlowDataPinValue_*`-typed property (Blueprint can't set property metadata, so it uses the typed
   wrapper), and exec pins by editing the `InputPins`/`OutputPins` arrays in class defaults.
5. AddOn eligibility overrides (`AcceptFlowNodeAddOnChild`) are `BlueprintNativeEvent`, so they *can*
   be overridden in Blueprint - see $KB:flow:concept:addon-attachment-eligibility-for-authors.
6. Set category/keywords/tooltip in class defaults for discoverability.

## Constraints

- `SupportsDelegates() -> false` on these Blueprint types - Flow node/addon Blueprints do not support
  Blueprint delegates.
- Logic lives in the EventGraph (there is no separate construction-script story to rely on for
  runtime behavior).

## See also

- $KB:flow:guide:creating-a-node-in-cpp
- $KB:flow:concept:declaring-pins
- $KB:flow:guide:choosing-a-node-authoring-approach
