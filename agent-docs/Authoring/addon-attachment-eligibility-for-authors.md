# AddOn Attachment Eligibility for Authors (Concept)

$KB:flow:concept:addon-attachment-eligibility-for-authors
signature: n/a (concept article - describes an authoring surface, not one class's structural surface)
keywords: addon attach eligibility accept reject AcceptFlowNodeAddOnChild AcceptFlowNodeAddOnParent handshake nesting

## What this is

How a node or addon **author controls which AddOns may attach to it** (and which parents an AddOn will
accept). This is the *authoring* side of the eligibility question - for a KB consumer, "can addon X
attach to node Y" is answered by the live query op in the RGI layer, but the *rule* it enforces is the
one you write here.

## The two-sided handshake

Attachment is agreed by **both** sides, and either can veto:

- **Parent's vote:** `AcceptFlowNodeAddOnChild(AddOnTemplate, AdditionalAddOnsToAssumeAreChildren)` -
  overridden on the node (or the addon, since AddOns can nest inside AddOns).
- **Child's vote:** `AcceptFlowNodeAddOnParent(ParentTemplate, ...)` - overridden on the AddOn.

Both return `EFlowAddOnAcceptResult`:
- `Undetermined` - no opinion (defer to the other side / default).
- `TentativeAccept` - accept if all other conditions are met.
- `Reject` - veto outright, even if the other side tentatively accepted.

Results are **priority-combined** (`CombineFlowAddOnAcceptResult` takes the max; `Reject` > 
`TentativeAccept` > `Undetermined`), so any `Reject` from either side wins. Both are
`BlueprintNativeEvent` (`_Implementation` in C++), so they can be overridden in C++ **or** Blueprint.

## The `AdditionalAddOnsToAssumeAreChildren` parameter

When evaluating eligibility during a multi-paste, the set being pasted is passed in via this array so
your rule can decide based on the *whole* incoming set (some parents only accept a certain mix of
AddOns). Account for it if your rule depends on sibling AddOns; ignore it otherwise.

## Common patterns

- **Exclusive child type:** a node accepts *only* one AddOn class and `Reject`s everything else.
- **Family accept:** a node `TentativeAccept`s any AddOn of a given base family (its logic is driven
  by the attached AddOns), deferring to `Super` otherwise.
- **Additive accept:** a node `TentativeAccept`s an extra AddOn family on top of whatever the base
  class already allows (calls `Super` for the rest).
- **Child-side veto:** an AddOn overrides `AcceptFlowNodeAddOnParent` so it only attaches to a
  specific parent type - the child enforcing its own valid parent.

## Guidance

- Always call `Super::AcceptFlowNodeAddOnChild_Implementation(...)` for cases you don't explicitly
  handle, so base-class rules still apply.
- Prefer `TentativeAccept` (not a hard accept) so other conditions and the other side's vote still
  get to weigh in; use `Reject` only to actively forbid.

## See also

- $KB:flow:guide:creating-a-node-in-cpp
- $KB:flow:concept:declaring-pins
