# Contributing to the flow skill

For anyone editing this skill's own files. If you are here to do Flow work, go back to `SKILL.md`.

## Where new content goes

- A new op the toolset exposes: `references/flow-ops.md`.
- A new request shape that works for any `FlowAsset` subclass: a new archetype section in
  `references/workflows.md`.
- A request shape that only makes sense for one project's asset subclass: that project's own domain
  skill, not here.
- Knowledge-base authoring or maintenance procedure: a project's own domain skill. `flow-ops.md` and
  `workflows.md` are about editing graph content, not the documents that describe it.
- Anything that fits none of the above: a new reference file, plus a sentence in `SKILL.md`'s
  "Where to go next" naming when to read it.

## Portable scope

This skill ships with FlowGraph. Keep every instruction usable in a checkout containing only this
plugin and Unreal Engine. Put project-specific classes, content prefixes, scripting languages,
and domain decisions in the consuming project's own skill. Keep the frontmatter description focused
on when to load this skill; keep detailed procedures in the body and references.

