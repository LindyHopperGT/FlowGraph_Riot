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

## This copy ships upstream

The plugin copy is the source of truth, and it is kept self-contained so it can ship to the
open-source project. Nothing in this directory may name a game-specific class, content prefix,
scripting language or domain vocabulary, and no angle-bracketed vendor markers belong here. Push
every project-specific dependency up into a domain skill. This is grep-enforced before commit.

## The shim description must stay in sync

A thin `SKILL.md` also exists at the repo root, under `.agents/skills/flow/`, purely so the skill is
discoverable by name. The loader indexes `.agents/skills/*/SKILL.md` literally: it never resolves
through to this file, and pointing that path here with a directory junction de-registers the skill
entirely.

That means the root shim's own frontmatter `description:` is what the loader reads to decide whether
to trigger this skill. Keep the two `description:` blocks byte-identical, and change both in the same
edit. Content goes in this copy; the shim stays a pointer.

