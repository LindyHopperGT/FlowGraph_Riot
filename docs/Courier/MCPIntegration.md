# Integrating FlowGraphCourier with an MCP Server

The `FlowGraphCourier` module provides the core export/import/glossary functionality for Flow Assets. This document describes how to wire those APIs into any MCP server implementation.

> **Future:** When Epic ships a native MCP service in Unreal Editor, a first-party `FlowGraphCourier` adapter will be written for it. Until then, integration is left to early adopters using the pattern described here.

---

## What FlowGraphCourier Provides

Four static utility classes, all **editor-only** (require `WITH_EDITOR` / `UnrealEd`):

| Class | Key method | What it does |
|---|---|---|
| `UFlowGraphExporter` | `ExportFlowGraphToString(UFlowAsset*)` | Serializes a FlowAsset to the text format |
| `UFlowGraphImporter` | `ImportFlowGraphFromString(text, path, error)` | Deserializes text into a new or existing UFlowAsset |
| `UFlowGraphRegrapher` | `ImportAndRegraphFromText(text, path)` | Import + rebuild editor graph in one call |
| `UFlowGraphGlossaryExporter` | `ExportGlossaryToString()` | Markdown glossary of all node types in the project |

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

### 3. Glossary Resource — `flow-graph-glossary`

Returns a markdown document listing all available `UFlowNode` and `UFlowNodeAddOn` types, their properties, and their pins. Intended to be served as a **resource** (read-once context) rather than a tool (called per-request).

```
Input:  (none)
Output: markdown string
```

```cpp
// Pseudocode
FString Glossary = UFlowGraphGlossaryExporter::ExportGlossaryToString();
return Glossary;
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

The `RGIUnrealMCPFlowGraph` plugin (at `CoreSDK/Plugins/Runtime/RGIUnrealMCPFlowGraph`) is a concrete example that wires `FlowGraphCourier` into Riot's internal `UMCPToolLibrary` framework:

- **`URGIUnrealMCPFlowGraphToolLibrary`** subclasses `UMCPToolLibrary` and exposes three `UFUNCTION`s — `ExportFlowAsset`, `ImportAndRegraphFlowAsset`, `GetFlowGraphGlossary`
- Each function calls the corresponding `FlowGraphCourier` static method and wraps the result in a JSON response via `CreateSuccessJson` / `CreateErrorJson`
- The glossary function is tagged with `MCPResource = "true"` metadata so the framework serves it as a resource rather than a tool

The `UMCPToolLibrary` base class and discovery mechanism (`FMCPToolDiscovery`) are Riot-internal and not part of this plugin. The pattern of subclassing a base tool library and using `UFUNCTION` metadata for tool registration is one viable approach; your MCP framework may use a different mechanism (e.g., explicit registration calls, a JSON schema file, or a reflection-based scanner).

---

## Constraints & Edge Cases

- **Editor-only.** `FlowGraphCourier` is an Editor module. Do not reference it from Runtime modules.
- **GEditor must be valid.** All operations require the Unreal Editor to be running.
- **Not PIE-safe.** Do not call import/regraph during Play-in-Editor; the asset registry and package system are in an inconsistent state.
- **Asset saving.** `ImportAndRegraphFromText` marks the package dirty but does not save it. If you need the asset persisted to disk, call `UEditorLoadingAndSavingUtils::SavePackages()` or equivalent after a successful import.
- **Glossary is project-specific.** `ExportGlossaryToString()` reflects the node types and Blueprint assets loaded in the current project. It also contains a project-specific section (Actor Types, Event Types) in `FlowGraphGlossaryText.inl` — replace or remove that section for your project.
- **Thread safety.** All `FlowGraphCourier` methods are designed to be called from the game thread. If your MCP server dispatches on a background thread, marshal calls back to the game thread via `AsyncTask(ENamedThreads::GameThread, ...)`.
