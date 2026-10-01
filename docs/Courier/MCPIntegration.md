# Integrating FlowGraphCourier with an MCP Server

The `FlowGraphCourier` module provides the core export/import/catalog-query functionality for Flow Assets. This document describes how to wire those APIs into any MCP server implementation.

> **Renamed from `UFlowGraphGlossaryExporter`:** if you are tracking an older revision of this plugin, the markdown-glossary exporter (`ExportGlossaryToString`/`ExportGlossaryToFile`, an unparameterized ~500 KB dump) was replaced by `UFlowCatalogQuery` - a filterable, paginated JSON query core (`QueryCatalog`) plus a file-only bulk export (`ExportFlowCatalogToFile`) for KB-gardening use cases. There is no drop-in string-returning replacement by design; callers filter first.

> **Future:** When Epic ships a native MCP service in Unreal Editor, a first-party `FlowGraphCourier` adapter will be written for it. Until then, integration is left to early adopters using the pattern described here.

---

## What FlowGraphCourier Provides

Four static utility classes, all **editor-only** (require `WITH_EDITOR` / `UnrealEd`):

| Class | Key method | What it does |
|---|---|---|
| `UFlowGraphExporter` | `ExportFlowGraphToString(UFlowAsset*)` | Serializes a FlowAsset to the text format |
| `UFlowGraphImporter` | `ImportFlowGraphFromString(text, path, error)` | Deserializes text into a new or existing UFlowAsset |
| `UFlowGraphRegrapher` | `ImportAndRegraphFromText(text, path)` | Import + rebuild editor graph in one call |
| `UFlowCatalogQuery` | `QueryCatalog(params, result)` | Filterable/paginated query over every node/addon/asset-type class in the project |

The text format these classes use is documented in [`FlowGraphTextFormat.md`](FlowGraphTextFormat.md).

---

## The Three MCP Surface Points

A complete MCP integration needs three things:

### 1. Export Tool — `export_flow_asset`

Reads a FlowAsset and returns its text representation. Safe to call at any time; does not modify anything.

```
Input:  asset_path  (string)  — e.g. "/Game/MyFlows/MyFlow"
Output: exported_text (string), text_length (int)
```

```cpp
// Pseudocode
UFlowAsset* Asset = LoadObject<UFlowAsset>(nullptr, *AssetPath);
if (!Asset) { return Error("Failed to load asset"); }
FString Text = UFlowGraphExporter::ExportFlowGraphToString(Asset);
return SuccessJson({ "exported_text", Text, "text_length", Text.Len() });
```

### 2. Import + Regraph Tool — `import_and_regraph_flow_asset`

Creates or overwrites a FlowAsset from text and rebuilds its editor graph. Marks the package dirty; caller is responsible for saving if needed.

```
Input:  flow_graph_text (string), asset_path (string)
Output: asset_path, node_count (int)
```

```cpp
// Pseudocode
UFlowAsset* Asset = UFlowGraphRegrapher::ImportAndRegraphFromText(FlowGraphText, AssetPath);
if (!Asset) { return Error("Import failed"); }
return SuccessJson({ "asset_path", AssetPath, "node_count", Asset->GetNodes().Num() });
```

### 3. Catalog Discovery Tools — `list_flow_asset_types` / `find_flow_node_types`

Two tools, not one - a full node/addon catalog is too large to serve as a single read-once resource
(this plugin alone ships hundreds of classes). `list_flow_asset_types` is the cold-start entry point
(counts only, one row per `FlowAsset` subclass); `find_flow_node_types` is a filter-then-project
query over node/addon classes, called per-request like any other tool.

```
Input (list_flow_asset_types):  (none)
Output: assetTypes[] (className, classPath, description, keywords, allowedNodeCount, allowedAddonCount), foundCount

Input (find_flow_node_types):  assetClassName?, classNames[]?, kind? ("node"|"addon"|"any"),
  query?, keywords[]?, categories[]?, pinTypes[]?, detail? ("names"|"summary"|"full"),
  bIncludeFacets?, limit?, offset?
Output: nodes[], addons[] (split, projected per `detail`), nodeCount, addonCount,
  totalNodeCount, totalAddonCount, truncated, detail, keywordFacets[], categoryFacets[],
  unresolvedClassNames[]
```

```cpp
// Pseudocode
FFlowCatalogQueryParams Params;
Params.AssetClassName = AssetClassName;   // or ClassNames for an exact-name lookup
Params.Detail = EFlowCatalogDetail::Summary;   // never Full on an unbounded query - it hard-caps at 25 matches
FFlowCatalogQueryResult Result;
UFlowCatalogQuery::QueryCatalog(Params, Result);
return SuccessJson(Result);
```

---

## Generic Integration Pattern

Your MCP server needs to:

1. **Depend on `FlowGraphCourier`** in your editor module's `Build.cs`:
   ```csharp
   PrivateDependencyModuleNames.AddRange(new string[] {
       "FlowGraphCourier",
       "Flow",
       "FlowEditor",
   });
   ```

2. **Register tools** with your server's registration mechanism. The exact API depends on your MCP framework. The pattern is always:
   - Tool name / description
   - Input parameter schema
   - A handler function that calls the appropriate `FlowGraphCourier` static method and returns a string result

3. **Guard for editor context.** All `FlowGraphCourier` methods require a valid `GEditor`. Do not call them during PIE or from non-editor builds.

   ```cpp
   if (!GEditor)
   {
       return ErrorJson(TEXT("FlowGraphCourier requires a valid editor context"));
   }
   ```

4. **Handle asset loading.** The export tool needs to load the asset before calling the exporter. Use `FSoftObjectPath::TryLoad()` or `LoadObject<>` — avoid synchronous loads on the game thread if your MCP server is async.

---

## Reference Implementation

`FlowGraphCourier` ships its own concrete example: `UFlowMCPToolset` (`Public/FlowMCPToolset.h`), a typed toolset registered with the engine's `ToolsetRegistry` plugin.

- `UFlowMCPToolset` exposes one `UFUNCTION(meta = (AICallable))` per op — `ExportFlowAsset`, `ImportAndRegraphFlowAsset`, `ApplyFlowPatch`, `ListFlowAssetTypes`, `FindFlowNodeTypes`, and the rest of the surface documented in `CourierTextFormat.md` — each taking a typed `USTRUCT` request and returning a typed `USTRUCT` result (no hand-rolled JSON marshaling).
- `ToolsetRegistry::RegisterToolsetClass` (called from `FFlowGraphCourierModule::StartupModule`, deferred to `FCoreDelegates::OnPostEngineInit`) makes the toolset discoverable to any MCP server built on that registry, with the request/result schemas reflected automatically from the UPROPERTY layout.
- This is one viable approach; your MCP framework may use a different mechanism (e.g., explicit per-tool registration calls, a hand-authored JSON schema file, or a reflection-based scanner over a different base class). The important shape to preserve is a thin per-op wrapper that does nothing but marshal parameters into the `FlowGraphCourier` static method call and marshal the result back out.

---

## Constraints & Edge Cases

- **Editor-only.** `FlowGraphCourier` is an Editor module. Do not reference it from Runtime modules.
- **GEditor must be valid.** All operations require the Unreal Editor to be running.
- **Not PIE-safe.** Do not call import/regraph during Play-in-Editor; the asset registry and package system are in an inconsistent state.
- **Asset saving.** `ImportAndRegraphFromText` marks the package dirty but does not save it. If you need the asset persisted to disk, call `UEditorLoadingAndSavingUtils::SavePackages()` or equivalent after a successful import.
- **The catalog is project-specific.** `QueryCatalog`/`ListAssetTypes` reflect whatever node/addon/FlowAsset classes (native + Blueprint) are loaded in the current project - there is no separate project-specific text section to maintain (the old `FlowGraphGlossaryText.inl` format guide was removed; `CourierTextFormat.md` is the single grammar source now).
- **Thread safety.** All `FlowGraphCourier` methods are designed to be called from the game thread. If your MCP server dispatches on a background thread, marshal calls back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.
