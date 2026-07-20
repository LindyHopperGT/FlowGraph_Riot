# Creating a Node/AddOn in C++ (Guide)

$KB:flow:guide:creating-a-node-in-cpp
signature: n/a (guide article, not code-derived)
keywords: create new node addon author cpp native subclass uflownode uflownodeaddon

## What this is

The top-level workflow for authoring a **native** `UFlowNode`/`UFlowNodeAddOn` subclass. Pin
declaration and addon-eligibility are cross-cutting and live in their own articles (linked below) -
this covers only how to stand up a new native class.

## Workflow

1. Subclass `UFlowNode` (a standalone graph step) or `UFlowNodeAddOn` (a modifier that attaches to a
   node/addon) - or an existing intermediate base if one fits. See
   $KB:flow:guide:choosing-a-node-authoring-approach.
2. **No asset step is needed.** Unlike Blueprint ($KB:flow:guide:creating-a-node-in-blueprint), a
   native class is discovered automatically once compiled: the graph schema gathers native
   nodes/addons straight from the reflection system (`GatherNativeNodesOrAddOns`), not the asset
   registry.
3. Override the execution surface you need (all from `IFlowCoreExecutableInterface`):
   - `ExecuteInput(PinName)` - a signal arrived on an input pin; the main entry point.
   - `TriggerOutput(PinName, bFinish)` / `TriggerFirstOutput(bFinish)` - fire an output pin.
   - `OnActivate`, `Cleanup`, `Finish`, `InitializeInstance`/`DeinitializeInstance` as needed.
4. Declare pins - see $KB:flow:concept:declaring-pins (exec, data, auto, context).
5. Restrict which AddOns may attach (if relevant) by overriding `AcceptFlowNodeAddOnChild` /
   `AcceptFlowNodeAddOnParent` - see $KB:flow:concept:addon-attachment-eligibility-for-authors.
6. Editor presentation is set in the constructor under `WITH_EDITOR`/`WITH_EDITORONLY_DATA`:
   `Category`, `NodeDisplayStyle`, node color, tooltip. Add `Keywords` UCLASS metadata so the node is
   discoverable by intent, not just class name.
7. SaveGame/preload hooks (`OnSave`/`OnLoad`, `IFlowPreloadableInterface`) only if the node needs them.

## When to prefer C++

- The class is a **base** other Blueprint nodes will subclass (a shared base is simplest
  native).
- The logic is perf-sensitive or needs engine access not exposed to Blueprint.
- Otherwise, Blueprint is fine for one-off, designer-iterable nodes - but read the Blueprint guide
  first, because Flow requires its own Blueprint asset type.

## See also

- $KB:flow:guide:creating-a-node-in-blueprint
- $KB:flow:concept:declaring-pins
- $KB:flow:concept:addon-attachment-eligibility-for-authors
