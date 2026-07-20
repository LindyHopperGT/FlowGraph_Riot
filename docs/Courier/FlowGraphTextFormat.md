# Flow Graph Text Format

## Overview
This document defines the text format used by `UFlowGraphExporter` and `UFlowGraphImporter` to serialize and reconstruct `UFlowAsset` graphs. The format is human-readable and machine-parseable, containing all information required for complete round-trip reconstruction.

The format is implemented in the `FlowGraphCourier` module of the Flow plugin.

## Format Version
**Version:** 1.0

## File Structure
An export consists of three sections in order:
1. **Header** — asset metadata
2. **Nodes** — all nodes with properties, pins, and add-ons
3. **Connections** — all pin connections between nodes

---

## Header Section

```
FlowGraphExport v1.0
AssetPath: <string>
AssetGuid: <guid>
WorldBound: <bool>
ExpectedOwnerClass: <string>
---
```

**Fields:**
- `AssetPath` — full Unreal asset path (e.g., `/Game/MyFlows/MyFlow.MyFlow`)
- `AssetGuid` — unique identifier for the asset (`XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`)
- `WorldBound` — whether the asset is world-bound (`true` / `false`)
- `ExpectedOwnerClass` — expected owner class name, or `None`

---

## Nodes Section

```
[NODES]
Node: <guid>
  Type: <class_name>
  Properties:
    <property_name>: <property_value>
    ...
  InputPins:
    - <pin_name> [<pin_type>]
    ...
  OutputPins:
    - <pin_name> [<pin_type>]
    ...
  AddOns:
    AddOn:
      Type: <addon_class_name>
      Properties:
        <property_name>: <property_value>
        ...

Node: <guid>
  ...
---
```

**Node fields:**
- `guid` — unique node identifier (`XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`)
- `Type` — full class name (e.g., `FlowNode_Start`, `FlowNode_Branch`)
- `Properties` — serialized key-value pairs (see [Property Value Formats](#property-value-formats))
- `InputPins` — input pins with name and type; omitted if empty
- `OutputPins` — output pins with name and type; omitted if empty
- `AddOns` — zero or more `UFlowNodeAddOn` instances; omitted if none

**Excluded from Properties** (exported in dedicated sections):
- `NodeGuid` — in the node header line
- `InputPins` / `OutputPins` — in the pins sections
- `Connections` — in the Connections section
- Transient and deprecated properties

---

## Connections Section

```
[CONNECTIONS]
<source_node_guid>.<source_pin_name> -> <target_node_guid>.<target_pin_name>
...
---
```

**Format:**
- Each line is one connection: `SourceGuid.SourcePin -> TargetGuid.TargetPin`
- GUIDs are in standard format (`XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX`)
- Pin names are case-sensitive and match exactly as defined on the node

**Connection types:**
- **Exec connections** — connect exec output pins to exec input pins (control flow); stored on the **source** node
- **Data connections** — connect data output pins to data input pins (data flow); stored on the **target** node

Both types use identical syntax. Lines beginning with `;` are comments (ignored by the importer).

---

## Property Value Formats

### Simple Types
| Type | Format | Example |
|------|--------|---------|
| Bool | `true` / `false` | `true` |
| Int / Float | Numeric | `42`, `3.14` |
| String | Quoted | `"Hello World"` |
| Name | Unquoted identifier | `MyName` |
| Enum | Enum value name | `Enabled` |

### Complex Types
| Type | Format | Example |
|------|--------|---------|
| Struct | JSON object | `{"X": 1.0, "Y": 2.0, "Z": 3.0}` |
| Array | JSON array | `["Item1", "Item2"]` |
| Object reference | Asset path | `/Game/MyAsset.MyAsset` |
| Class reference | Class path | `/Script/Engine.Actor` |

### Special Values
- `None` — null / unset reference
- `""` — empty string
- `[]` — empty array

---

## Data Pin Types

Common pin types used in `[<pin_type>]` annotations:

| Type | Description |
|------|-------------|
| `Exec` | Execution flow (control pin) |
| `Bool` | Boolean |
| `Int` | 32-bit integer |
| `Int64` | 64-bit integer |
| `Float` | 32-bit float |
| `Double` | 64-bit float |
| `String` | Text string |
| `Name` | FName |
| `Text` | FText (localized) |
| `Vector` | FVector (X, Y, Z) |
| `Rotator` | FRotator (Pitch, Yaw, Roll) |
| `Transform` | FTransform |
| `Object` | UObject reference |
| `Class` | UClass reference |
| `Enum` | Enumeration value |
| `GameplayTag` | FGameplayTag |
| `GameplayTagContainer` | FGameplayTagContainer |
| `InstancedStruct` | Polymorphic struct |

---

## Complete Example

```
FlowGraphExport v1.0
AssetPath: /Game/TestFlow.TestFlow
AssetGuid: 12345678-1234-1234-1234-123456789ABC
WorldBound: true
ExpectedOwnerClass: None
---

[NODES]
Node: AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA
  Type: FlowNode_Start
  Properties:
    SignalMode: Enabled
  OutputPins:
    - Out [Exec]

Node: BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB
  Type: FN_Spawn_C
  Properties:
    ActorType: /Game/Actors/GoalActor.GoalActor_C
  InputPins:
    - In [Exec]
  OutputPins:
    - OnSpawned [Exec]
    - ActorID [InstancedStruct]
    - Location [Vector]

Node: CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC
  Type: FN_Listen_C
  Properties:
    EventType: "OnTriggered"
  InputPins:
    - In [Exec]
    - ActorID [InstancedStruct]
  OutputPins:
    - OnEvent [Exec]

Node: DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD
  Type: FlowNode_Finish
  Properties:
    FinishPolicy: Keep
  InputPins:
    - In [Exec]
---

[CONNECTIONS]
; Exec connections (control flow)
AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA.Out -> BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB.In
BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB.OnSpawned -> DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD.In
CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC.OnEvent -> DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD.In
; Data connections (data flow)
BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB.ActorID -> CCCCCCCC-CCCC-CCCC-CCCC-CCCCCCCCCCCC.ActorID
BBBBBBBB-BBBB-BBBB-BBBB-BBBBBBBBBBBB.Location -> DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD.MapMarkerLocation
---
```

---

## Reconstruction Notes

When importing from this format:

1. **Parse header** — extract asset metadata
2. **Create nodes** — instantiate each node type, assign its GUID, set properties, register with the asset
3. **Create add-ons** — instantiate each add-on type on its parent node, set properties
4. **Apply connections** — exec connections go on the source node's `Connections` map; data connections go on the target node's `Connections` map
5. **Finalize** — call `HarvestNodeConnections()` to sync the editor graph

**Important:**
- GUIDs must be preserved exactly — they are the identity of nodes across references
- Pin names are case-sensitive
- Node order in the file is irrelevant; connections must reference nodes that exist
- Custom node types must be loaded in the project for reconstruction to succeed
