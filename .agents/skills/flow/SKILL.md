---
name: flow
description: >
  Flow graph authoring and Courier JSON editing for any FlowAsset. Use for Flow nodes, addons,
  graph structure, and Courier tools. Load a project's Flow skill first for its own asset subclass.
---

# Flow

Authoring, editing and explaining Flow graph content, for any `UFlowAsset` subclass. This is the
core layer: it describes mechanics that hold for every project, and names no project-specific
class or content prefix.

## The model

A `FlowAsset` owns a flat map of nodes keyed by **GUID**. That GUID is a node's identity for its
whole life. Every edit addresses a node by GUID, never by title or index, and preserving it is
what separates a surgical edit from a rebuild.

Each node has input and output pins of two kinds. **Exec** pins carry control flow: something
fires, and execution moves on. **Data** pins carry values. A connection joins one output pin to
one input pin, and the two kinds differ in how they fan out - an exec output pin drives exactly
one target, while a data output pin may feed several inputs. That asymmetry is real, and shipped
content relies on it.

**Addons** are instanced sub-objects that extend a node. An addon attaches to a node or to another
addon, to any depth, carries its own GUID and properties, and can contribute pins to the node it
hangs off - which is why a node's live pin set is not always what its class alone implies. Whether
a given addon may attach to a given parent is settled by a two-sided handshake, where the parent
and the child each get a say, rather than by any type rule you can infer from class names. Ask the
editor instead of reasoning it out.

Some nodes are **entry points**, where execution begins. A `FlowAsset` referenced as a subgraph can
also expose named entry and exit points to the graph that references it, which appear as extra pins
on the referencing node.

Node position and comment text are editor-side decoration. Both round-trip, and both matter to the
designer reading the graph, but nothing executes differently because of them.

## Reading and writing graphs

Graphs serialize as **Courier v2**: one JSON document type that serves both a complete dump and a
narrow patch, differing only in how many ops it carries. A document has a `mode`, a few
asset-level header fields, and a list of `ops` - `UpsertNode`, `DeleteNode`, `UpsertAddon`,
`DeleteAddon`, `AddConnection`, `RemoveConnection`. Identity is explicit: an op names an existing
object by `guid` or introduces a new one by `newAlias`, and exactly one of the two.

Two ops put a document into an asset, and choosing the wrong one is destructive. `ApplyFlowPatch`
reconciles: only what the document names changes, and everything else keeps its identity, position
and content. `ImportAndRegraphFlowAsset` builds a new asset from nothing. Anything that already
exists wants the patch.

`describe_toolset` publishes the exact request and result schema for every op, reflected live from
the C++ structs. It is the authority on shapes, and prose cannot outrank it.

## Where to go next

Read `references/workflows.md` when you have a request and need the sequence of ops that fits its
shape; it also holds the research loop and the edit loop. For a graph review or refactor-opportunity
pass, also read `$KB:flow:concept:structuring-coherent-graphs` and `$KB:flow:concept:subgraphs`.
The former helps assess whether the graph has coherent entry-rooted sections; the latter covers
when to extract sprawling or repeated sections, how to design a narrow function-like subgraph
interface, and the lifecycle limits of `CustomInput`. Read `references/flow-ops.md` when you already
know which op you want and need its parameters, returns and traps. For what a document's fields
mean, and the export-text rules that govern property values, fetch
`$KB:flow:guide:courier-text-format`. Read `CONTRIBUTING.md` before adding a reference file.

## The knowledge base

Flow's agent-facing knowledge base is markdown, federated across `agent-docs/` roots:

- This plugin's `agent-docs/` - generic concepts and authoring guides.
- An extension plugin's `agent-docs/`, when that plugin is installed.
- A project's own `agent-docs/` root, for its asset subclasses and its design-decision log. A
  companion domain skill names that path; this file deliberately does not.

The plugin's `docs/` is its published site, separate from the agent knowledge base.

Cross-links are `$KB:<domain>:<kind>:<id>`, for example `$KB:flow:concept:subgraphs`. Resolve one
by grepping the exact tag string, never by guessing a file path or a section number. Per-class
documentation is not in the knowledge base at all: a node or addon class carries its own guidance
on the class itself, read through `FindFlowNodeTypes`. If an article you expect does not exist, say
so rather than reconstructing what it probably said.

`agent-docs/Scripts/kb_lint.py` checks that every `$KB:` reference resolves and that each article's
declared tag matches its filename. Pass every root the project uses in one invocation - a single
root reports false failures on any reference that crosses roots. It is plain Python and needs no
running editor.

## Verifying a claim

When a class name, pin name or property matters and you are not certain of it, escalate in this
order: query the catalog (`FindFlowNodeTypes`) for the reflected facts; then `FindFlowNodeUsage` for
how shipped content actually wires it; then, only if the catalog and usage don't settle it, read the
owning source file. Never invent an API, pin name or property - state that you could not confirm it
instead.

## Domain skills

A project-specific skill sits on top of this one. This file owns the mechanics above. A domain skill
owns the judgment that only makes sense inside its domain: which structural model its asset subclass
follows, its own vocabulary, and what may be assumed about its shipped content. When a request names
a project's own asset subclass or content prefix, start from that skill, and come back here for
generic node, addon and concept questions.
