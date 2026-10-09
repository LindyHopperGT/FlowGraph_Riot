// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "UObject/Object.h"
#include "Nodes/FlowPin.h"
#include "FlowGraphValidation.h"
#include "FlowAssetPropertyPolicy.h"
#include "FlowGraphImporter.generated.h"

class UFlowAsset;
class UFlowNode;
class UFlowNodeBase;
class UFlowNodeAddOn;
class FMapProperty;

// Keep changed property paths grouped by AddOn until its complete sibling and child tree
// is attached. The dispatcher then builds callback chains against final authored values.
using FFlowPendingAddOnPropertyChanges = TMap<UFlowNodeAddOn*, TArray<FString>>;

// Per-pin info stored in FFlowGraphDeclaredOutputPinTypes.
// TypeStr: "Exec", "Vector", "wildcard", etc. (case-insensitive "Exec" == exec pin).
// SubCategoryPath: object path from a "PinName [PinType:/Path]" bracket; empty when absent.
struct FDeclaredPinInfo
{
	// Authored pin type name, such as Exec, Vector, or wildcard.
	FString TypeStr;
	// Optional object path used as the pin type's subcategory.
	FString SubCategoryPath;
};

// Inner map: pin name -> FDeclaredPinInfo. Populated from parsed OutputPins declarations.
using FFlowGraphDeclaredOutputPinTypes = TMap<FGuid, TMap<FName, FDeclaredPinInfo>>;

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedNodeAddOn
{
	GENERATED_BODY()

	// Stable identity when the source operation supplied a GUID; invalid for a newAlias AddOn.
	FGuid AddOnGuid;
	// Resolved class or short name of the AddOn to create or update.
	FString AddOnType;
	// Authored AddOn property values, keyed by reflected property path.
	TMap<FString, FString> Properties;

	// Mutation-document-only (Reconciler). Never set when parsing a full-graph export.
	// A DeleteAddon op removes a single addon by GUID from its owning node, leaving siblings intact.
	// AddOnType/Properties are unused when this is set.
	bool bIsDeleteMarker = false;

	// Nested addon-of-addon children. Courier v2 documents address every addon flat via
	// parentGuid/parentAlias; FFlowCourierConverter::ConvertToParsedGraph resolves that flat list
	// into this nested shape before CreateAddOnsRecursive walks it. Empty for a leaf addon. Unused
	// (always empty) when bIsDeleteMarker is set.
	TArray<FFlowGraphParsedNodeAddOn> AddOns;
};

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedNode
{
	GENERATED_BODY()

	// Stable identity used to look up this node in the FlowAsset.
	FGuid NodeGuid;
	// Resolved class or short name of the node to create or update.
	FString NodeType;
	// Authored node property values, keyed by reflected property name.
	TMap<FString, FString> Properties;
	// Declared additional input pin names from the Courier document.
	TArray<FString> InputPins;
	// Declared additional output pin specifications from the Courier document.
	TArray<FString> OutputPins;
	// Child AddOns nested under this node after flat document relationships are resolved.
	TArray<FFlowGraphParsedNodeAddOn> AddOns;

	// Present when the source op carried an explicit position. Optional/advisory - absent nodes
	// are placed by FFlowGraphLayout::ComputeAutoPlacedPositions.
	// True when the source operation supplied an explicit editor position.
	bool bHasPos = false;
	// Requested editor position when bHasPos is true.
	FIntPoint Pos = FIntPoint::ZeroValue;

	// Optional per-node comment text, from an op's "comment" field (WITH_EDITOR only). Applied
	// to UEdGraphNode::NodeComment during regraph.
	// True when the source operation supplied node comment text.
	bool bHasComment = false;
	// Requested editor comment when bHasComment is true.
	FString NodeComment;

	// Mutation-document-only. Never emitted by the exporter.
	// Set for a DeleteNode op.
	bool bIsDeleteMarker = false;
	// Set when the source op carried a newAlias instead of a guid, minting a fresh GUID. Alias is
	// the document-local name other ops reference to resolve this node.
	bool bIsNewAlias = false;
	// Document-local name used to reference a node whose GUID is minted during conversion.
	FString Alias;

	// Set from an UpsertNode op's bReplaceAddons: listed AddOns become the complete authoritative
	// list for this node - an existing addon absent from the list is deleted, not merged. Default
	// (false) is merge: only listed sub-sections are reconciled, everything else on the node is
	// left alone. bReplaceAddons currently governs AddOns only, not pins or connections.
	bool bIsFullBlock = false;
};

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedConnection
{
	GENERATED_BODY()

	// GUID of the node whose output pin starts the connection.
	FGuid SourceNodeGuid;
	// Name of the source node's output pin.
	FName SourcePinName;
	// GUID of the node whose input pin receives the connection.
	FGuid TargetNodeGuid;
	// Name of the target node's input pin.
	FName TargetPinName;

	// Mutation-document-only. Never set when parsing a full-graph export.
	// Set for a RemoveConnection op.
	bool bIsDeleteMarker = false;
};

UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphImporter : public UObject
{
	GENERATED_BODY()

	// FFlowGraphReconciler reuses the parse/create/connect helpers rather than duplicating them.
	friend class FFlowGraphReconciler;

	// FFlowGraphValidation reuses ResolveNodeClass for class-resolution and palette checks.
	friend class FFlowGraphValidation;
	friend class FFlowNodeClassReplacement;

	friend class UFlowGraphDiff;

public:
	// Takes already-parsed nodes/connections (e.g. from FFlowCourierConverter::ConvertToParsedGraph)
	// rather than parsing text itself. This class stays document-agnostic - callers own parsing.
	static UFlowAsset* ImportFlowGraphFromDocument(
		const FString& TargetAssetPath,
		const FString& AssetClassPath,
		bool bWorldBound,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		FString& OutErrorMessage,
		const FFlowProjectedPinSchema* ProjectedPinSchema = nullptr,
		const TMap<FName, FFlowStagedAssetProperty>* StagedAssetProperties = nullptr);

	static bool PopulateFlowAssetFromDocument(
		UFlowAsset* TargetAsset,
		bool bWorldBound,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		FString& OutErrorMessage);

	// Parses a single pin declaration string into its components. Handles two bracket forms:
	//   "PinName [PinType]"                           -> OutPinType="PinType", OutSubCategoryPath=""
	//   "PinName [PinType:/Object/Path]"              -> OutPinType="PinType", OutSubCategoryPath="/Object/Path"
	// OutPinType and OutSubCategoryPath are empty when the bracket section is absent.
	// Returns false only if the declaration is entirely empty after trimming.
	static bool ParsePinDecl(const FString& PinDecl, FName& OutPinName, FString& OutPinType, FString& OutSubCategoryPath);

	// Resolves a node/addon type string to a concrete UClass, then rejects classes that cannot be
	// placed in a Flow graph. Tries LoadObject (full path), FindObject short-name, and a last-resort
	// as-is lookup, and finally fails closed on a Blueprint-generated class whose generating asset is
	// a plain UBlueprint rather than a UFlowNodeBlueprint or UFlowNodeAddOnBlueprint. Such a class
	// compiles and derives from UFlowNode, but both the palette and the Courier catalog gather by
	// asset class, so placing one yields a node no editor surface can offer or round-trip.
	// Returns nullptr when nothing matches or the match is rejected.
	static UClass* ResolveNodeClass(const FString& NodeType);

	// Resolution without the placement check, for read-only callers that must still be able to name a
	// malformed class - notably FindNodes' ClassFilter, which is how an author locates existing bad
	// nodes in order to repair them. Never use this to decide what may be created.
	static UClass* ResolveNodeClassForQuery(const FString& NodeType);

	// True when InClass was generated by a Blueprint that is neither a Flow node nor a Flow addon
	// Blueprint, filling OutReason with an author-facing explanation. False for native and
	// script-generated classes, which have no generating Blueprint and are always allowed.
	static bool IsClassFromNonFlowBlueprint(const UClass* InClass, FString& OutReason);

private:
	static bool CreateFlowNodes(UFlowAsset* FlowAsset, const TArray<FFlowGraphParsedNode>& ParsedNodes, TMap<FGuid, UFlowNode*>& OutNodeMap, FString& OutErrorMessage);
	static bool SetNodeProperties(UFlowNode* FlowNode, const TMap<FString, FString>& Properties, FString& OutErrorMessage);
	static bool SetupConnections(UFlowAsset* FlowAsset, const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedConnection>& Connections, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage);
	static bool CreateNodeAddOns(const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage);
	static bool CreateNodeAddOns(const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedNode>& ParsedNodes,
		FFlowPendingAddOnPropertyChanges& OutPendingPropertyChanges, FString& OutErrorMessage);

	// Recursively creates ParsedAddOns under Owner (a UFlowNode or UFlowNodeAddOn, both of which
	// have an AddOns array), then recurses into each newly-created addon's own nested AddOns.
	// Shared by CreateNodeAddOns and FFlowGraphReconciler::ReconcileNodeAddOns.
	static bool CreateAddOnsRecursive(UFlowNodeBase* Owner, const TArray<FFlowGraphParsedNodeAddOn>& ParsedAddOns,
		FString& OutErrorMessage);
	static bool CreateAddOnsRecursive(UFlowNodeBase* Owner, const TArray<FFlowGraphParsedNodeAddOn>& ParsedAddOns,
		FFlowPendingAddOnPropertyChanges& OutPendingPropertyChanges, FString& OutErrorMessage);
	// Dispatches queued property changes after the entire root-owned AddOn tree has been attached.
	static bool DispatchPendingAddOnPropertyChanges(const TArray<UFlowNodeBase*>& RootOwners,
		const FFlowPendingAddOnPropertyChanges& PendingPropertyChanges, FString& OutErrorMessage);

	// SetupConnections helpers
	static FFlowGraphDeclaredOutputPinTypes BuildDeclaredOutputPinTypes(const TArray<FFlowGraphParsedNode>& ParsedNodes);
	static bool ResolveSourcePin(UFlowNode* SourceNode, FName SourcePinName, const FFlowGraphDeclaredOutputPinTypes& DeclaredOutputPinTypes, bool& bOutIsExecPin);
	static bool EnsureTargetDataPinExists(UFlowNode* TargetNode, FName TargetPinName);
	static bool WriteConnectionToNode(UFlowNode* NodeToModify, FName KeyName, const FConnectedPin& ConnectedPin, FMapProperty* ConnectionsProperty);

	// Assigns reflected values immediately and queues changed paths for the later tree-complete dispatch.
	static bool SetAddOnProperties(UFlowNodeAddOn* AddOn, const TMap<FString, FString>& Properties,
		FFlowPendingAddOnPropertyChanges& OutPendingPropertyChanges, FString& OutErrorMessage);

	// Resolves and writes one AddOn member or nested property path, queuing a callback only when its value changes.
	static bool SetAddOnProperty(UFlowNodeAddOn* AddOn, const TPair<FString, FString>& PropertyChange,
		FFlowPendingAddOnPropertyChanges& OutPendingPropertyChanges, FString& OutErrorMessage);

#if WITH_EDITORONLY_DATA
	static void SyncAddOnDataPinValueDirections(UFlowNodeAddOn* AddOn);
#endif

	// OwnerForInstancedSubobjects owns any instanced subobject this call constructs from a
	// "ClassPath(Field=Value,...)" value. Pass the node or addon whose property is being written.
	// Pass nullptr when ValuePtr is scratch memory rather than a live object's property (see
	// FFlowGraphReconciler::NormalizePropertyValueString) - a null owner makes the class-plus-fields
	// form an error instead of constructing an object with nowhere to live.
	static bool SetPropertyFromString(FProperty* Property, void* ValuePtr, const FString& ValueString, FString& OutErrorMessage,
		UObject* OwnerForInstancedSubobjects = nullptr);

	// Resolves a dotted property path (e.g. "SpawnActorInfo.bProjectLocationToGround" or
	// "Items.0.Name") against OwnerStruct/ContainerPtr, descending through nested FStructProperty
	// members and FArrayProperty indices one path segment at a time. Used as a fallback by
	// SetNodeProperties/SetAddOnProperties when a flat FindPropertyByName lookup fails and the key
	// contains a dot. Resolution stays within this module so the importer can process nested
	// properties without a separate toolset dependency.
	static bool ResolvePropertyPath(UStruct* OwnerStruct, void* ContainerPtr, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr, FString& OutErrorMessage);

	// Applies parsed InputPins/OutputPins from ParsedNode to NewNode, creating any pins that are
	// absent from the node's CDO (dynamic pins or explicitly declared overrides). Existing pins
	// whose name + type already match are skipped (idempotent). Called from CreateFlowNodes so
	// declared pins survive round-trip even when they carry no connections.
	static void ApplyDeclaredPins(UFlowNode* NewNode, const FFlowGraphParsedNode& ParsedNode);
};
