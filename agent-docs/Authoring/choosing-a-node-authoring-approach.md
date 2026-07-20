# Choosing a Node/AddOn Authoring Approach (Guide)

$KB:flow:guide:choosing-a-node-authoring-approach
signature: n/a (guide article, not code-derived)
keywords: create new node addon author cpp blueprint native

## What this is

A decision guide for **starting a brand-new** `UFlowNode`/`UFlowNodeAddOn` subclass: which base
class, and C++ vs Blueprint. This article stays generic; project-specific base-class choices are
documented in the owning project's root - see $KB:kb:guide:master-index.

## Node or AddOn?

- **`UFlowNode`** - a standalone step placed directly in the graph; owns its own input/output pins
  and transitions.
- **`UFlowNodeAddOn`** - attaches *to* a node (or to another AddOn), and only exists in the context
  of its parent. Use an AddOn when the behavior is a modifier/condition/listener on an existing
  node rather than its own step - see $KB:flow:concept:addon-attachment-eligibility-for-authors for
  how a node/addon controls which AddOns may attach to it.

## C++ or Blueprint?

- **C++** when: the logic needs to be perf-sensitive, needs engine-level access not exposed to
  Blueprint, or is a *base class* other Blueprint nodes will derive from (a base class
  must be native or a Blueprint others can further subclass - native is simpler for a shared base).
- **Blueprint** when: the logic is one-off, designer-iterable, and doesn't need to be a further base
  class. **Read $KB:flow:guide:creating-a-node-in-blueprint before starting** - Flow requires its
  own Blueprint asset type; a plain Blueprint subclassing `UFlowNode` will not appear in the graph
  editor's palette at all.

## See also

- $KB:flow:guide:creating-a-node-in-cpp
- $KB:flow:guide:creating-a-node-in-blueprint
- $KB:flow:concept:declaring-pins
