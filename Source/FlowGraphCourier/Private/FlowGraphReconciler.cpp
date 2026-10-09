// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphReconciler.h"
#include "FlowGraphImporter.h"
#include "FlowCourierConverter.h"
#include "FlowCourierDocument.h"
#include "FlowGraphRegrapher.h"
#include "FlowGraphLayout.h"
#include "FlowGraphExporter.h"
#include "FlowLogChannels.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "FlowAsset.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "Nodes/FlowNode.h"
#include "Nodes/FlowNodeBase.h"
#include "Nodes/FlowPin.h"
#include "AddOns/FlowNodeAddOn.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"
#include "Templates/UniquePtr.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "ScopedTransaction.h"

// Builds the set of node GUIDs that will exist after the plan applies:
// existing nodes + adds - deletes (including scope-driven implicit deletes).
static TSet<FGuid> BuildPostPlanNodeSet(
	const TMap<FGuid, UFlowNode*>& CurrentNodes,
	const TArray<FFlowGraphParsedNode>& NodesToAdd,
	const TArray<FGuid>& NodesToDelete)
{
	TSet<FGuid> PostPlanGuids;
	PostPlanGuids.Reserve(CurrentNodes.Num());
	for (const TPair<FGuid, UFlowNode*>& Pair : CurrentNodes)
	{
		PostPlanGuids.Add(Pair.Key);
	}

	for (const FFlowGraphParsedNode& NodeToAdd : NodesToAdd)
	{
		PostPlanGuids.Add(NodeToAdd.NodeGuid);
	}

	for (const FGuid& DeletedGuid : NodesToDelete)
	{
		PostPlanGuids.Remove(DeletedGuid);
	}

	return PostPlanGuids;
}

// The Courier converter resolves every alias and GUID in the complete document before producing
// parsed connections. Reconciliation consumes resolved endpoints only.

// Appends every Courier converter issue (JSON parse/structural problems) as a validation finding,
// so a malformed document is reported the same way a semantic validation problem is - as data in
// ValidationFindings, never by raising. Code carries across directly: both types share one code set.
// Document paths are preserved as optional provenance alongside graph-space GUIDs.
static void AppendCourierIssuesAsFindings(const TArray<FFlowCourierIssue>& Issues, TArray<FFlowValidationFinding>& OutFindings)
{
	for (const FFlowCourierIssue& Issue : Issues)
	{
		FFlowValidationFinding& Finding = OutFindings.AddDefaulted_GetRef();
		Finding.Severity = Issue.Severity;
		Finding.Code = Issue.Code;
		Finding.SourceDocumentPath = Issue.Path;
		Finding.Message = FString::Printf(TEXT("[%s] %s: %s"), *Issue.Code, *Issue.Path, *Issue.Message);
	}
}

static void StageAssetProperties(
	const TMap<FString, FString>& AssetProperties,
	UClass* AssetClass,
	const UObject* ExistingAsset,
	FFlowReconcileExecutionPlan& OutPlan)
{
	TArray<FString> PropertyNames;
	AssetProperties.GenerateKeyArray(PropertyNames);
	PropertyNames.Sort();

	for (const FString& PropertyNameString : PropertyNames)
	{
		const FString* const Value = AssetProperties.Find(PropertyNameString);
		const FName PropertyName(*PropertyNameString);
		FFlowStagedAssetProperty StagedProperty;
		FString Code;
		FString ErrorMessage;
		const bool bHeaderConflict = PropertyNameString.Equals(TEXT("AssetClass"), ESearchCase::IgnoreCase) ||
			PropertyNameString.Equals(TEXT("bWorldBound"), ESearchCase::IgnoreCase) ||
			PropertyNameString.Equals(TEXT("ExpectedOwnerClass"), ESearchCase::IgnoreCase);
		if ((!bHeaderConflict && PropertyName.ToString() != PropertyNameString) || !Value ||
			!FFlowAssetPropertyPolicy::StageObjectReferenceProperty(
				AssetClass, ExistingAsset, PropertyName, Value ? *Value : FString(), StagedProperty, Code, ErrorMessage))
		{
			if (Code.IsEmpty())
			{
				Code = TEXT("InvalidAssetPropertyName");
				ErrorMessage = FString::Printf(TEXT("Asset property names must use the registered property's exact spelling: '%s'."), *PropertyNameString);
			}
			FFlowValidationFinding& Finding = OutPlan.ValidationFindings.AddDefaulted_GetRef();
			Finding.Severity = EFlowValidationSeverity::Error;
			Finding.Code = Code;
			Finding.SourceDocumentPath = TEXT("assetProperties.") + PropertyNameString;
			Finding.Message = ErrorMessage;
			continue;
		}

		if (StagedProperty.bChanged)
		{
			FFlowReconcileAssetPropertyEntry& PlanEntry = OutPlan.Plan.AssetPropertiesChanged.AddDefaulted_GetRef();
			PlanEntry.Name = PropertyNameString;
			PlanEntry.PreviousValue = StagedProperty.PreviousValue;
			PlanEntry.ProposedValue = StagedProperty.ProposedValue;
		}
		OutPlan.StagedAssetProperties.Add(PropertyName, MoveTemp(StagedProperty));
	}
}

// Resolves an addon type string to a concrete UFlowNodeAddOn subclass. Delegates to the shared
// UFlowGraphImporter::ResolveNodeClass so class resolution stays byte-for-byte identical to what
// FFlowGraphValidation checked pre-commit - a divergent resolver here (e.g. one that only
// FindObject's the short-name form, or lacks the as-is last-resort lookup) could let a plan pass
// validation and then fail to resolve the same class at apply time, or vice versa. This matters
// most for project-specific/Blueprint/AngelScript addon type strings, exactly where the fallbacks differ.
static UClass* ResolveAddOnClass(const FString& AddOnType, FString& OutErrorMessage)
{
	UClass* AddOnClass = UFlowGraphImporter::ResolveNodeClass(AddOnType);
	if (!AddOnClass || !AddOnClass->IsChildOf(UFlowNodeAddOn::StaticClass()))
	{
		OutErrorMessage = FString::Printf(TEXT("ReconcileNodeAddOns: Could not find AddOn class: %s"), *AddOnType);
		return nullptr;
	}

	return AddOnClass;
}

// Check whether a full-block document retains an existing child by its stable AddOn identity.
static bool HasParsedAddOnGuid(const TArray<FFlowGraphParsedNodeAddOn>& ParsedAddOns, const FGuid& AddOnGuid)
{
	for (const FFlowGraphParsedNodeAddOn& ParsedAddOn : ParsedAddOns)
	{
		if (ParsedAddOn.AddOnGuid == AddOnGuid)
		{
			return true;
		}
	}

	return false;
}

// Returns true when SourcePinName is an exec output pin on SourceNode. Data connections are keyed
// on the target node's input pin; exec connections are keyed on the source node's output pin -
// querying the wrong side breaks idempotency detection for data pins. Checks SourceNode's declared
// output pins (including dynamically-added ones, not just CDO-static ones). Defaults to true
// (exec assumption) when SourceNode is null or the pin is not found - both are cases where
// bAlreadyPresent is correctly always false anyway, since a non-existent node has no connections.
static bool IsExecConnectionSourcePin(const UFlowNode* SourceNode, FName SourcePinName)
{
	if (SourceNode)
	{
		for (const FFlowPin& Pin : SourceNode->GetOutputPins())
		{
			if (Pin.PinName == SourcePinName)
			{
				return Pin.IsExecPin();
			}
		}
	}

	return true;
}

// Break links on retained orphan pins too, or regraph harvesting can restore a removed runtime connection.
static void BreakEditorConnectionLink(const FFlowGraphParsedConnection& ConnectionToRemove, UFlowAsset& FlowAsset)
{
	UFlowNode* const SourceNode = FlowAsset.GetNode(ConnectionToRemove.SourceNodeGuid);
	UFlowNode* const TargetNode = FlowAsset.GetNode(ConnectionToRemove.TargetNodeGuid);
	UFlowGraphNode* const SourceGraphNode = IsValid(SourceNode)
		? Cast<UFlowGraphNode>(SourceNode->GetGraphNode())
		: nullptr;
	UFlowGraphNode* const TargetGraphNode = IsValid(TargetNode)
		? Cast<UFlowGraphNode>(TargetNode->GetGraphNode())
		: nullptr;
	if (!IsValid(SourceGraphNode) || !IsValid(TargetGraphNode))
	{
		return;
	}

	UEdGraphPin* const SourcePin = SourceGraphNode->FindPin(ConnectionToRemove.SourcePinName, EGPD_Output);
	UEdGraphPin* const TargetPin = TargetGraphNode->FindPin(ConnectionToRemove.TargetPinName, EGPD_Input);
	if (SourcePin && TargetPin && SourcePin->LinkedTo.Contains(TargetPin))
	{
		SourceGraphNode->Modify();
		TargetGraphNode->Modify();
		SourcePin->BreakLinkTo(TargetPin);
	}
}

// Full mode only: an existing connection not re-declared by the document is implicitly removed,
// mirroring node deletion. Enumerates every connection exactly once - exec connections from the
// source node's output pin, data connections from the target node's input pin - matching the keying
// UFlowGraphExporter uses. Connections involving nodes that will be deleted are included so the
// plan reports the complete set of graph changes before node cleanup runs.
static void CollectExistingConnections(
	const TMap<FGuid, UFlowNode*>& CurrentNodes,
	TArray<FFlowGraphParsedConnection>& OutConnections)
{
	for (const TPair<FGuid, UFlowNode*>& NodePair : CurrentNodes)
	{
		UFlowNode* Node = NodePair.Value;
		if (!Node)
		{
			continue;
		}

		for (const FFlowPin& OutputPin : Node->GetOutputPins())
		{
			if (!OutputPin.IsExecPin())
			{
				continue;
			}

			const FConnectedPin Connection = Node->GetConnection(OutputPin.PinName);
			if (Connection.NodeGuid.IsValid())
			{
				FFlowGraphParsedConnection& Parsed = OutConnections.AddDefaulted_GetRef();
				Parsed.SourceNodeGuid = NodePair.Key;
				Parsed.SourcePinName = OutputPin.PinName;
				Parsed.TargetNodeGuid = Connection.NodeGuid;
				Parsed.TargetPinName = Connection.PinName;
			}
		}

		for (const FFlowPin& InputPin : Node->GetInputPins())
		{
			if (InputPin.IsExecPin())
			{
				continue;
			}

			const FConnectedPin Connection = Node->GetConnection(InputPin.PinName);
			if (Connection.NodeGuid.IsValid())
			{
				FFlowGraphParsedConnection& Parsed = OutConnections.AddDefaulted_GetRef();
				Parsed.SourceNodeGuid = Connection.NodeGuid;
				Parsed.SourcePinName = Connection.PinName;
				Parsed.TargetNodeGuid = NodePair.Key;
				Parsed.TargetPinName = InputPin.PinName;
			}
		}
	}
}

UFlowAsset* FFlowGraphReconciler::FindOrLoadFlowAsset(const FString& TargetAssetPath)
{
	// Check for an already-in-memory asset first (e.g. one open and unsaved in the editor, or just
	// created this session) - LoadObject alone only resolves packages that exist on disk and would
	// miss it, incorrectly falling through to the create-new path. FindObject needs the
	// fully-qualified dotted object path rather than the bare package path, so resolve the package
	// by name first, then find the object by name within it.
	UFlowAsset* Found = nullptr;
	const FString PackageName = FPackageName::ObjectPathToPackageName(TargetAssetPath);
	if (UPackage* ExistingPackage = FindPackage(nullptr, *PackageName))
	{
		// GetShortName only splits on '/', so it must run on the already-dot-stripped PackageName -
		// running it directly on TargetAssetPath leaves a literal ".ObjectName" suffix when the
		// caller passes a fully-qualified dotted object path (e.g. FTopLevelAssetPath::ToString()).
		Found = FindObject<UFlowAsset>(ExistingPackage, *FPackageName::GetShortName(PackageName));
	}

	if (!Found)
	{
		// Do not create a placeholder package when planning against a missing target.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			Found = LoadObject<UFlowAsset>(nullptr, *TargetAssetPath);
		}
	}

	return Found;
}

// new-asset compute: populates plan summary and execution data from an already-converted
// document. There is no existing asset to validate class-allowance/attachment rules against, but
// pin existence and connection endpoints are fully determined by the document itself (every node
// referenced is either in ParsedNodes or the document is invalid), so ValidateDocument still runs
// with a null TargetAsset and the full set of added node GUIDs as PostPlanNodeGuids.
bool FFlowGraphReconciler::ComputePlanForNewAsset(
	const FString& AssetClassPath,
	bool bWorldBound,
	const TArray<FFlowGraphParsedNode>& ParsedNodes,
	const TArray<FFlowGraphParsedConnection>& ParsedConnections,
	FFlowReconcileExecutionPlan& OutPlan,
	FString& OutErrorMessage)
{
	OutPlan.AllParsedNodes = ParsedNodes;
	OutPlan.AllParsedConnections = ParsedConnections;
	OutPlan.AssetClassPath = AssetClassPath;
	OutPlan.bWorldBound = bWorldBound;

	for (const FFlowGraphParsedNode& ParsedNode : OutPlan.AllParsedNodes)
	{
		if (!ParsedNode.bIsDeleteMarker)
		{
			OutPlan.NodesToAdd.Add(ParsedNode);
			OutPlan.Plan.NodesAdded.Add(ParsedNode.NodeGuid);
		}
	}

	// Every connection in the document is new on an asset that does not exist yet. Reporting them
	// here is what lets a dry-run create say what it would connect; ExecutePlanForNewAsset then
	// replaces this intent with what actually landed. Without it a create reports adding no
	// connections while silently making them - a claim contradicted by the asset itself.
	for (const FFlowGraphParsedConnection& ParsedConnection : OutPlan.AllParsedConnections)
	{
		if (!ParsedConnection.bIsDeleteMarker)
		{
			OutPlan.Plan.ConnectionsAdded.Add(FString::Printf(TEXT("%s.%s -> %s.%s"),
				*ParsedConnection.SourceNodeGuid.ToString(), *ParsedConnection.SourcePinName.ToString(),
				*ParsedConnection.TargetNodeGuid.ToString(), *ParsedConnection.TargetPinName.ToString()));
		}
	}

	OutPlan.NodesTouched = OutPlan.Plan.NodesAdded.Num();
	OutPlan.NodesPreserved = 0;
	OutPlan.bIsNewAsset = true;

	const TSet<FGuid> PostPlanNodeGuids = BuildPostPlanNodeSet(TMap<FGuid, UFlowNode*>(), OutPlan.NodesToAdd, TArray<FGuid>());
	// Project new nodes and their AddOn trees before validating same-document generated-pin connections.
	FString PreviewError;
	if (!BuildEffectivePinSchema(
		nullptr, OutPlan, OutPlan.ProjectedPinSchema, PreviewError, &OutPlan.EffectiveStateAsset))
	{
		FFlowValidationFinding& Finding = OutPlan.ValidationFindings.AddDefaulted_GetRef();
		Finding.Severity = EFlowValidationSeverity::Error;
		Finding.Code = TEXT("PinSchemaPreviewFailed");
		Finding.Message = PreviewError;
	}
	FFlowValidationContext ValidationContext;
	ValidationContext.EffectiveAsset = OutPlan.EffectiveStateAsset.Get();
	ValidationContext.ResolvedAssetClass = OutPlan.ResolvedAssetClass;
	ValidationContext.StagedAssetProperties = &OutPlan.StagedAssetProperties;
	FFlowGraphValidation::ValidateDocument(OutPlan.EffectiveStateAsset.Get(), OutPlan.AllParsedNodes,
		OutPlan.AllParsedConnections, OutPlan.ValidationFindings, &PostPlanNodeGuids,
		&OutPlan.ProjectedPinSchema, &ValidationContext);

	return true;
}

#if WITH_EDITOR
namespace FlowGraphEffectiveStateDetail
{
	// Restore authored AddOn identity after DuplicateObject's lifecycle hooks, then reconnect each
	// clone to the transient owning node so context pins see the same ownership as commit.
	void RestoreAddOnGuids(const UFlowNodeBase& SourceOwner, UFlowNodeBase& PreviewOwner, UFlowNode& PreviewFlowNode)
	{
		const TArray<UFlowNodeAddOn*>& SourceAddOns = SourceOwner.GetFlowNodeAddOnChildren();
		TArray<UFlowNodeAddOn*>& PreviewAddOns = PreviewOwner.GetFlowNodeAddOnChildrenByEditor();
		const int32 AddOnCount = FMath::Min(SourceAddOns.Num(), PreviewAddOns.Num());
		for (int32 AddOnIndex = 0; AddOnIndex < AddOnCount; ++AddOnIndex)
		{
			UFlowNodeAddOn* const SourceAddOn = SourceAddOns[AddOnIndex];
			UFlowNodeAddOn* const PreviewAddOn = PreviewAddOns[AddOnIndex];
			if (!IsValid(SourceAddOn) || !IsValid(PreviewAddOn))
			{
				continue;
			}

			PreviewAddOn->SetGuid(SourceAddOn->GetGuid());
			PreviewAddOn->ClearPreviousGuidForPinRemapping();
			PreviewAddOn->SetFlowNodeForEditor(&PreviewFlowNode);
			RestoreAddOnGuids(*SourceAddOn, *PreviewAddOn, PreviewFlowNode);
		}
	}

	void ClearGraphNodeReference(UFlowNodeBase& Node)
	{
		// SetGraphNode has editor side effects; clear the reflected pointer without touching a live graph.
		if (FObjectPropertyBase* GraphNodeProperty = FindFProperty<FObjectPropertyBase>(
			UFlowNodeBase::StaticClass(), TEXT("GraphNode")))
		{
			GraphNodeProperty->SetObjectPropertyValue_InContainer(&Node, nullptr);
		}
	}

	bool AddNodeToPreviewAsset(UFlowAsset& PreviewAsset, UFlowNode& Node, FString& OutErrorMessage)
	{
		// Insert only the runtime GUID-to-node entry. RegisterNode would harvest editor connections.
		FMapProperty* NodesProperty = FindFProperty<FMapProperty>(UFlowAsset::StaticClass(), TEXT("Nodes"));
		if (!NodesProperty)
		{
			OutErrorMessage = TEXT("Could not find the FlowAsset Nodes map for the isolated preview");
			return false;
		}

		void* NodesPtr = NodesProperty->ContainerPtrToValuePtr<void>(&PreviewAsset);
		FScriptMapHelper NodesHelper(NodesProperty, NodesPtr);
		const int32 NewIndex = NodesHelper.AddDefaultValue_Invalid_NeedsRehash();
		uint8* PairPtr = NodesHelper.GetPairPtr(NewIndex);

		FStructProperty* KeyProperty = CastField<FStructProperty>(NodesProperty->KeyProp);
		FObjectPropertyBase* ValueProperty = CastField<FObjectPropertyBase>(NodesProperty->ValueProp);
		if (!KeyProperty || !ValueProperty)
		{
			OutErrorMessage = TEXT("FlowAsset Nodes map has an unexpected key or value type");
			return false;
		}

		KeyProperty->Struct->CopyScriptStruct(KeyProperty->ContainerPtrToValuePtr<void>(PairPtr), &Node.GetGuid());
		ValueProperty->SetObjectPropertyValue_InContainer(PairPtr, &Node);
		NodesHelper.Rehash();
		return true;
	}

	// Keep the projected pin set unique by name while preserving metadata from its first source.
	bool HasPinNamed(const TArray<FFlowPin>& Pins, FName PinName)
	{
		for (const FFlowPin& Pin : Pins)
		{
			if (Pin.PinName == PinName)
			{
				return true;
			}
		}

		return false;
	}

	void AppendUniquePins(TArray<FFlowPin>& InOutPins, const TArray<FFlowPin>& NewPins)
	{
		// Preserve first-seen pin metadata and skip duplicate names, matching runtime aggregation order.
		for (const FFlowPin& Pin : NewPins)
		{
			if (!Pin.PinName.IsNone() && !HasPinNamed(InOutPins, Pin.PinName))
			{
				InOutPins.Add(Pin);
			}
		}
	}

	FFlowProjectedPinSet BuildProjectedPinSet(const UFlowNode& Node)
	{
		// Combine static/auto pins with the AddOn-derived context in each independent direction.
		FFlowProjectedPinSet Result;
		AppendUniquePins(Result.InputPins, Node.GetInputPins());
		AppendUniquePins(Result.InputPins, Node.GetContextInputs());
		AppendUniquePins(Result.OutputPins, Node.GetOutputPins());
		AppendUniquePins(Result.OutputPins, Node.GetContextOutputs());
		return Result;
	}

	// Carries the isolated post-plan graph while each preview phase updates one part of its state.
	struct FFlowEffectivePinSchemaPreview
	{
		// Transient asset used as the outer and ownership context for every preview node.
		TStrongObjectPtr<UFlowAsset> Asset;

		// Nodes that will survive execution, indexed by their stable graph GUID.
		TMap<FGuid, UFlowNode*> Nodes;

		// Strong references keep transient preview nodes alive until schema collection completes.
		TArray<TStrongObjectPtr<UFlowNode>> NodeKeepAlive;

		// Authored post-plan node blocks, used to identify which cloned nodes need projected edits.
		TMap<FGuid, const FFlowGraphParsedNode*> ParsedNodesByGuid;

		// Node identities whose pin schema comes from newly-instantiated preview nodes.
		TSet<FGuid> AddedNodeGuids;

		// Existing node identities removed by this plan and therefore excluded from the preview.
		TSet<FGuid> DeletedNodeGuids;
	};

	bool CreatePreviewAsset(
		UFlowAsset* ExistingAsset,
		const FFlowReconcileExecutionPlan& ExecutionPlan,
		TStrongObjectPtr<UFlowAsset>& OutPreviewAsset,
		FString& OutErrorMessage)
	{
		UClass* PreviewAssetClass = ExistingAsset ? ExistingAsset->GetClass() : UFlowAsset::StaticClass();
		if (!ExistingAsset && !ExecutionPlan.AssetClassPath.IsEmpty())
		{
			UClass* RequestedAssetClass = LoadObject<UClass>(nullptr, *ExecutionPlan.AssetClassPath);
			if (!RequestedAssetClass || !RequestedAssetClass->IsChildOf(UFlowAsset::StaticClass()) ||
				RequestedAssetClass->HasAnyClassFlags(CLASS_Abstract))
			{
				OutErrorMessage = FString::Printf(TEXT("Could not create an isolated asset preview for class '%s'"),
					*ExecutionPlan.AssetClassPath);
				return false;
			}
			PreviewAssetClass = RequestedAssetClass;
		}

		OutPreviewAsset.Reset(NewObject<UFlowAsset>(
			GetTransientPackage(), PreviewAssetClass, NAME_None, RF_Transient));
		if (!OutPreviewAsset.IsValid())
		{
			OutErrorMessage = FString::Printf(TEXT("Could not create an isolated preview of FlowAsset class '%s'"),
				*PreviewAssetClass->GetPathName());
			return false;
		}
		return true;
	}

	void IndexPreviewPlan(
		const FFlowReconcileExecutionPlan& ExecutionPlan,
		FFlowEffectivePinSchemaPreview& Preview)
	{
		// Index authored node blocks separately from adds so unchanged nodes can keep their live read-only schema.
		for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
		{
			if (!ParsedNode.bIsDeleteMarker)
			{
				Preview.ParsedNodesByGuid.Add(ParsedNode.NodeGuid, &ParsedNode);
			}
		}

		for (const FGuid& DeletedNodeGuid : ExecutionPlan.NodesToDelete)
		{
			Preview.DeletedNodeGuids.Add(DeletedNodeGuid);
		}

		for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.NodesToAdd)
		{
			Preview.AddedNodeGuids.Add(ParsedNode.NodeGuid);
		}
	}

	bool DuplicateExistingNodes(
		UFlowAsset* ExistingAsset,
		FFlowEffectivePinSchemaPreview& Preview,
		FString& OutErrorMessage)
	{
		if (!ExistingAsset)
		{
			return true;
		}

		for (const TPair<FGuid, UFlowNode*>& ExistingNodePair : ExistingAsset->GetNodes())
		{
			if (Preview.DeletedNodeGuids.Contains(ExistingNodePair.Key) || !IsValid(ExistingNodePair.Value))
			{
				continue;
			}

			UFlowNode* PreviewNode = DuplicateObject<UFlowNode>(ExistingNodePair.Value, Preview.Asset.Get());
			if (!IsValid(PreviewNode))
			{
				OutErrorMessage = FString::Printf(TEXT("Could not duplicate node %s for the isolated pin preview"),
					*ExistingNodePair.Key.ToString());
				return false;
			}

			PreviewNode->SetGuid(ExistingNodePair.Key);
			PreviewNode->ClearPreviousGuidForPinRemapping();
			ClearGraphNodeReference(*PreviewNode);
			RestoreAddOnGuids(*ExistingNodePair.Value, *PreviewNode, *PreviewNode);
			Preview.NodeKeepAlive.Emplace(PreviewNode);
			Preview.Nodes.Add(ExistingNodePair.Key, PreviewNode);
		}

		return true;
	}

	bool InstantiateAddedNodes(
		const FFlowReconcileExecutionPlan& ExecutionPlan,
		FFlowEffectivePinSchemaPreview& Preview,
		FString& OutErrorMessage)
	{
		// New nodes use the same resolved classes and GUIDs as the live importer.
		for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.NodesToAdd)
		{
			UClass* NodeClass = UFlowGraphImporter::ResolveNodeClass(ParsedNode.NodeType);
			if (!NodeClass || !NodeClass->IsChildOf(UFlowNode::StaticClass()) || NodeClass->HasAnyClassFlags(CLASS_Abstract))
			{
				OutErrorMessage = FString::Printf(TEXT("Could not create a node preview for class '%s'"), *ParsedNode.NodeType);
				return false;
			}

			UFlowNode* PreviewNode = NewObject<UFlowNode>(Preview.Asset.Get(), NodeClass, NAME_None, RF_Transient);
			if (!IsValid(PreviewNode))
			{
				OutErrorMessage = FString::Printf(TEXT("Could not create a transient node preview for '%s'"), *ParsedNode.NodeType);
				return false;
			}

			PreviewNode->SetGuid(ParsedNode.NodeGuid);
			Preview.NodeKeepAlive.Emplace(PreviewNode);
			Preview.Nodes.Add(ParsedNode.NodeGuid, PreviewNode);
		}

		return true;
	}

	bool RegisterPreviewNodes(
		FFlowEffectivePinSchemaPreview& Preview,
		FString& OutErrorMessage)
	{
		// Ownership-dependent context queries need the full post-plan GUID-to-node map first.
		for (const TPair<FGuid, UFlowNode*>& PreviewNodePair : Preview.Nodes)
		{
			if (!AddNodeToPreviewAsset(*Preview.Asset, *PreviewNodePair.Value, OutErrorMessage))
			{
				return false;
			}
		}
		return true;
	}

	void RefreshPreviewAutoPins(FFlowEffectivePinSchemaPreview& Preview)
	{
		// AddOn property callbacks have run; now refresh node auto pins against their final state.
		for (const TPair<FGuid, const FFlowGraphParsedNode*>& ParsedNodePair : Preview.ParsedNodesByGuid)
		{
			if (UFlowNode* PreviewNode = Preview.Nodes.FindRef(ParsedNodePair.Key))
			{
				PreviewNode->TryUpdateAutoDataPins();
			}
		}
	}

	bool CollectProjectedPinSchema(
		UFlowAsset* ExistingAsset,
		const FFlowEffectivePinSchemaPreview& Preview,
		FFlowProjectedPinSchema& OutProjectedPinSchema,
		FString& OutErrorMessage)
	{
		// Store active pins for every surviving node; an unmentioned existing node keeps its live schema.
		for (const TPair<FGuid, UFlowNode*>& PreviewNodePair : Preview.Nodes)
		{
			const UFlowNode* NodeForSchema = PreviewNodePair.Value;
			if (!Preview.AddedNodeGuids.Contains(PreviewNodePair.Key) &&
				!Preview.ParsedNodesByGuid.Contains(PreviewNodePair.Key) && ExistingAsset)
			{
				NodeForSchema = ExistingAsset->GetNode(PreviewNodePair.Key);
			}
			if (!IsValid(NodeForSchema))
			{
				OutErrorMessage = FString::Printf(TEXT("Could not resolve effective node state for %s"),
					*PreviewNodePair.Key.ToString());
				return false;
			}

			OutProjectedPinSchema.Add(PreviewNodePair.Key, BuildProjectedPinSet(*NodeForSchema));
		}
		return true;
	}
}
#endif // WITH_EDITOR

bool FFlowGraphReconciler::BuildEffectivePinSchema(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	FFlowProjectedPinSchema& OutProjectedPinSchema,
	FString& OutErrorMessage,
	TStrongObjectPtr<UFlowAsset>* OutEffectiveAsset)
{
	OutErrorMessage.Reset();
	OutProjectedPinSchema.Reset();

#if WITH_EDITOR
	// Validation must preview the exact AddOn-generated pins from the requested final state,
	// because Courier resolves connections before its live mutation begins.
	FlowGraphEffectiveStateDetail::FFlowEffectivePinSchemaPreview Preview;
	if (!FlowGraphEffectiveStateDetail::CreatePreviewAsset(
		ExistingAsset, ExecutionPlan, Preview.Asset, OutErrorMessage))
	{
		return false;
	}
	if (ExistingAsset)
	{
		if (!FFlowAssetPropertyPolicy::CopyRegisteredProperties(ExistingAsset, Preview.Asset.Get(), OutErrorMessage))
		{
			return false;
		}
	}
	else
	{
		Preview.Asset->bWorldBound = ExecutionPlan.bWorldBound;
	}

	TArray<FName> StagedPropertyNames;
	ExecutionPlan.StagedAssetProperties.GenerateKeyArray(StagedPropertyNames);
	StagedPropertyNames.Sort(FNameLexicalLess());
	for (const FName PropertyName : StagedPropertyNames)
	{
		const FFlowStagedAssetProperty& StagedProperty = ExecutionPlan.StagedAssetProperties.FindChecked(PropertyName);
		if (StagedProperty.bChanged && !FFlowAssetPropertyPolicy::SetStagedObjectReference(
			Preview.Asset.Get(), StagedProperty, /*bSendEditNotifications=*/true, OutErrorMessage))
		{
			return false;
		}
	}
	FlowGraphEffectiveStateDetail::IndexPreviewPlan(ExecutionPlan, Preview);

	// Build isolated node identity and state first, then attach those nodes to the preview asset map.
	if (!FlowGraphEffectiveStateDetail::DuplicateExistingNodes(ExistingAsset, Preview, OutErrorMessage) ||
		!FlowGraphEffectiveStateDetail::InstantiateAddedNodes(ExecutionPlan, Preview, OutErrorMessage) ||
		!FlowGraphEffectiveStateDetail::RegisterPreviewNodes(Preview, OutErrorMessage))
	{
		return false;
	}

	// Apply node properties and declared pins after preview nodes have their final identity and parent map.
	for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
	{
		if (ParsedNode.bIsDeleteMarker)
		{
			continue;
		}

		UFlowNode* PreviewNode = Preview.Nodes.FindRef(ParsedNode.NodeGuid);
		if (!IsValid(PreviewNode))
		{
			OutErrorMessage = FString::Printf(
				TEXT("Could not find isolated node preview for %s"), *ParsedNode.NodeGuid.ToString());
			return false;
		}

		if (!UFlowGraphImporter::SetNodeProperties(PreviewNode, ParsedNode.Properties, OutErrorMessage))
		{
			return false;
		}
		PreviewNode->TryUpdateAutoDataPins();
		UFlowGraphImporter::ApplyDeclaredPins(PreviewNode, ParsedNode);
	}

	// Apply AddOn edits into a shared notification set so callbacks wait until every tree is complete.
	FFlowPendingAddOnPropertyChanges PendingPropertyChanges;
	TArray<FFlowReconcileAddonEntry> PreviewAddOnsAdded;
	TArray<FFlowReconcileAddonEntry> PreviewAddOnsUpdated;
	TArray<FFlowReconcileAddonEntry> PreviewAddOnsDeleted;
	for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
	{
		if (ParsedNode.bIsDeleteMarker)
		{
			continue;
		}

		UFlowNode* PreviewNode = Preview.Nodes.FindRef(ParsedNode.NodeGuid);
		if (Preview.AddedNodeGuids.Contains(ParsedNode.NodeGuid))
		{
			if (!UFlowGraphImporter::CreateAddOnsRecursive(
				PreviewNode, ParsedNode.AddOns, PendingPropertyChanges, OutErrorMessage))
			{
				return false;
			}
		}
		else if (!ParsedNode.AddOns.IsEmpty() || ParsedNode.bIsFullBlock)
		{
			if (!ReconcileNodeAddOns(PreviewNode, ParsedNode.NodeGuid, ParsedNode.AddOns, PendingPropertyChanges,
				PreviewAddOnsAdded, PreviewAddOnsUpdated, PreviewAddOnsDeleted, OutErrorMessage, ParsedNode.bIsFullBlock))
			{
				return false;
			}
		}
	}

	// Run the same child-before-parent callbacks used during execution, then refresh auto data pins.
	TArray<UFlowNodeBase*> PreviewRootOwners;
	PreviewRootOwners.Reserve(Preview.Nodes.Num());
	for (const TPair<FGuid, UFlowNode*>& PreviewNodePair : Preview.Nodes)
	{
		PreviewRootOwners.Add(PreviewNodePair.Value);
	}
	if (!UFlowGraphImporter::DispatchPendingAddOnPropertyChanges(
		PreviewRootOwners, PendingPropertyChanges, OutErrorMessage))
	{
		return false;
	}

	FlowGraphEffectiveStateDetail::RefreshPreviewAutoPins(Preview);
	if (!FlowGraphEffectiveStateDetail::CollectProjectedPinSchema(
		ExistingAsset, Preview, OutProjectedPinSchema, OutErrorMessage))
	{
		return false;
	}
	if (OutEffectiveAsset)
	{
		*OutEffectiveAsset = MoveTemp(Preview.Asset);
	}
	return true;
#else
	(void)ExistingAsset;
	(void)ExecutionPlan;
	(void)OutEffectiveAsset;
	OutErrorMessage = TEXT("Courier pin schema preview requires editor support");
	return false;
#endif // WITH_EDITOR
}

// new-asset execute: full import + regraph.
bool FFlowGraphReconciler::ExecutePlanForNewAsset(
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	const FString& TargetAssetPath,
	FFlowReconcileResult& Result,
	FString& OutErrorMessage)
{
	UFlowAsset* CreatedAsset = UFlowGraphImporter::ImportFlowGraphFromDocument(
		TargetAssetPath,
		ExecutionPlan.AssetClassPath,
		ExecutionPlan.bWorldBound,
		ExecutionPlan.AllParsedNodes,
		ExecutionPlan.AllParsedConnections,
		OutErrorMessage,
		&ExecutionPlan.ProjectedPinSchema,
		&ExecutionPlan.StagedAssetProperties);
	if (!CreatedAsset)
	{
		Result.Findings.Add(OutErrorMessage);
		return false;
	}

	// Extract positions from the already-parsed node list (same as ApplyPlanToExistingAsset does)
	// so aliased nodes keep the GUIDs already minted by the converter.
	TMap<FGuid, FIntPoint> NodePositions;
	for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
	{
		if (ParsedNode.bHasPos && !ParsedNode.bIsDeleteMarker)
		{
			NodePositions.Add(ParsedNode.NodeGuid, ParsedNode.Pos);
		}
	}
	if (!UFlowGraphRegrapher::RegraphFlowAsset(CreatedAsset, NodePositions))
	{
		OutErrorMessage = TEXT("Created the asset but failed to regraph it");
		Result.Findings.Add(OutErrorMessage);
		return false;
	}

	// Hold the create path to the same standard as the patch path: report what the asset shows, not
	// what the import was asked to do. ImportFlowGraphFromDocument succeeding is not evidence that
	// every connection took.
	TArray<FFlowGraphParsedConnection> ExpectedConnections;
	ExpectedConnections.Reserve(ExecutionPlan.AllParsedConnections.Num());
	for (const FFlowGraphParsedConnection& ParsedConnection : ExecutionPlan.AllParsedConnections)
	{
		if (!ParsedConnection.bIsDeleteMarker)
		{
			ExpectedConnections.Add(ParsedConnection);
		}
	}

	TArray<FString> VerifiedConnectionsAdded, NotLandedConnections;
	FFlowGraphReconciler::VerifyConnectionsLanded(CreatedAsset, ExpectedConnections, VerifiedConnectionsAdded, NotLandedConnections);
	for (const FString& NotLandedLabel : NotLandedConnections)
	{
		Result.Findings.Add(FString::Printf(TEXT("Requested connection did not land: %s"), *NotLandedLabel));
	}
	Result.Plan.ConnectionsAdded = MoveTemp(VerifiedConnectionsAdded);

	return true;
}

void FFlowGraphReconciler::VerifyConnectionsLanded(
	UFlowAsset* FlowAsset,
	const TArray<FFlowGraphParsedConnection>& ExpectedConnections,
	TArray<FString>& OutVerified,
	TArray<FString>& OutNotLanded)
{
	OutVerified.Reset();
	OutNotLanded.Reset();
	if (!FlowAsset)
	{
		for (const FFlowGraphParsedConnection& ExpectedConnection : ExpectedConnections)
		{
			OutNotLanded.Add(FString::Printf(TEXT("%s.%s -> %s.%s"),
				*ExpectedConnection.SourceNodeGuid.ToString(), *ExpectedConnection.SourcePinName.ToString(),
				*ExpectedConnection.TargetNodeGuid.ToString(), *ExpectedConnection.TargetPinName.ToString()));
		}
		return;
	}

	OutVerified.Reserve(ExpectedConnections.Num());
	for (const FFlowGraphParsedConnection& ExpectedConnection : ExpectedConnections)
	{
		UFlowNode* ExpectedSourceNode = FlowAsset->GetNodes().FindRef(ExpectedConnection.SourceNodeGuid);
		UFlowNode* ExpectedTargetNode = FlowAsset->GetNodes().FindRef(ExpectedConnection.TargetNodeGuid);
		const bool bIsExecPin = IsExecConnectionSourcePin(ExpectedSourceNode, ExpectedConnection.SourcePinName);
		UFlowNode* KeyNode = bIsExecPin ? ExpectedSourceNode : ExpectedTargetNode;
		const FName KeyPinName = bIsExecPin ? ExpectedConnection.SourcePinName : ExpectedConnection.TargetPinName;
		const FGuid ExpectedOtherGuid = bIsExecPin ? ExpectedConnection.TargetNodeGuid : ExpectedConnection.SourceNodeGuid;
		const FName ExpectedOtherPinName = bIsExecPin ? ExpectedConnection.TargetPinName : ExpectedConnection.SourcePinName;

		const FString ConnectionLabel = FString::Printf(TEXT("%s.%s -> %s.%s"),
			*ExpectedConnection.SourceNodeGuid.ToString(), *ExpectedConnection.SourcePinName.ToString(),
			*ExpectedConnection.TargetNodeGuid.ToString(), *ExpectedConnection.TargetPinName.ToString());

		bool bConnectionVerified = false;
		if (KeyNode)
		{
			const FConnectedPin ActualConnection = KeyNode->GetConnection(KeyPinName);
			bConnectionVerified = ActualConnection.NodeGuid == ExpectedOtherGuid
				&& ActualConnection.PinName == ExpectedOtherPinName;
		}

		if (bConnectionVerified)
		{
			OutVerified.Add(ConnectionLabel);
		}
		else
		{
			OutNotLanded.Add(ConnectionLabel);
		}
	}
}

void FFlowGraphReconciler::ClassifyParsedNodes(
	const TArray<FFlowGraphParsedNode>& ParsedNodes,
	const TArray<FGuid>& ScopedNodeGuids,
	const TMap<FGuid, UFlowNode*>& CurrentNodes,
	TArray<FFlowGraphParsedNode>& OutNodesToAdd,
	TArray<FFlowGraphParsedNode>& OutNodesToUpdate,
	TArray<FGuid>& OutNodesToDelete,
	TMap<FString, FGuid>& OutAliasMap)
{
	for (const FFlowGraphParsedNode& ParsedNode : ParsedNodes)
	{
		if (ParsedNode.bIsDeleteMarker)
		{
			if (CurrentNodes.Contains(ParsedNode.NodeGuid))
			{
				OutNodesToDelete.Add(ParsedNode.NodeGuid);
			}
			continue;
		}

		if (ParsedNode.bIsNewAlias || !CurrentNodes.Contains(ParsedNode.NodeGuid))
		{
			OutNodesToAdd.Add(ParsedNode);
			if (ParsedNode.bIsNewAlias && !ParsedNode.Alias.IsEmpty())
			{
				OutAliasMap.Add(ParsedNode.Alias, ParsedNode.NodeGuid);
			}
		}
		else
		{
			OutNodesToUpdate.Add(ParsedNode);
		}
	}

	// A node named in scope but absent from this document's [NODES] section is treated as an
	// implicit delete, as if "-Node: <Guid>" had been written. A node not named in scope is
	// never affected by this loop - unscoped omission means "untouched".
	// Build the upsert set once so the per-scope-entry lookup is O(1), not O(ParsedNodes).
	TSet<FGuid> UpsertedGuids;
	UpsertedGuids.Reserve(ParsedNodes.Num());
	for (const FFlowGraphParsedNode& ParsedNode : ParsedNodes)
	{
		if (!ParsedNode.bIsDeleteMarker)
		{
			UpsertedGuids.Add(ParsedNode.NodeGuid);
		}
	}

	for (const FGuid& ScopedGuid : ScopedNodeGuids)
	{
		if (!CurrentNodes.Contains(ScopedGuid) || OutNodesToDelete.Contains(ScopedGuid))
		{
			continue;
		}

		if (!UpsertedGuids.Contains(ScopedGuid))
		{
			OutNodesToDelete.Add(ScopedGuid);
		}
	}
}

void FFlowGraphReconciler::ClassifyParsedConnections(
	const TArray<FFlowGraphParsedConnection>& ParsedConnections,
	const TMap<FGuid, UFlowNode*>& CurrentNodes,
	TArray<FFlowGraphParsedConnection>& OutConnectionsToAdd,
	TArray<FFlowGraphParsedConnection>& OutConnectionsToRemove,
	FFlowReconcilePlan& OutPlan)
{
	// Distinguish genuinely-new/removable connections from ones already in the requested state,
	// so both add and delete are idempotent and only real changes appear in the plan.
	for (const FFlowGraphParsedConnection& ParsedConnection : ParsedConnections)
	{
		UFlowNode* SourceNode = CurrentNodes.FindRef(ParsedConnection.SourceNodeGuid);
		UFlowNode* TargetNode = CurrentNodes.FindRef(ParsedConnection.TargetNodeGuid);
		const bool bIsExecPin = IsExecConnectionSourcePin(SourceNode, ParsedConnection.SourcePinName);

		// Exec connections key on SourceNode+SourcePinName; data connections key on
		// TargetNode+TargetPinName (see UFlowGraphImporter::WriteConnectionToNode).
		UFlowNode* KeyNode = bIsExecPin ? SourceNode : TargetNode;
		const FName KeyPinName = bIsExecPin ? ParsedConnection.SourcePinName : ParsedConnection.TargetPinName;
		const FGuid ExpectedOtherGuid = bIsExecPin ? ParsedConnection.TargetNodeGuid : ParsedConnection.SourceNodeGuid;
		const FName ExpectedOtherPinName = bIsExecPin ? ParsedConnection.TargetPinName : ParsedConnection.SourcePinName;

		FConnectedPin ExistingConnection;
		bool bAlreadyPresent = false;
		if (KeyNode)
		{
			ExistingConnection = KeyNode->GetConnection(KeyPinName);
			bAlreadyPresent = ExistingConnection.NodeGuid == ExpectedOtherGuid && ExistingConnection.PinName == ExpectedOtherPinName;
		}

		const FString ConnectionLabel = FString::Printf(TEXT("%s.%s -> %s.%s"),
			*ParsedConnection.SourceNodeGuid.ToString(), *ParsedConnection.SourcePinName.ToString(),
			*ParsedConnection.TargetNodeGuid.ToString(), *ParsedConnection.TargetPinName.ToString());

		if (ParsedConnection.bIsDeleteMarker)
		{
			if (bAlreadyPresent)
			{
				OutConnectionsToRemove.Add(ParsedConnection);
				OutPlan.ConnectionsRemoved.Add(ConnectionLabel);
			}

			continue;
		}

		if (!bAlreadyPresent)
		{
			OutConnectionsToAdd.Add(ParsedConnection);
			OutPlan.ConnectionsAdded.Add(ConnectionLabel);

			// A pin has at most one connection at runtime - adding a connection to a pin that
			// already points elsewhere implicitly retargets it. Report the superseded connection
			// as removed so the plan reflects what actually changed.
			if (ExistingConnection.NodeGuid.IsValid())
			{
				const FString SupersededLabel = bIsExecPin
					? FString::Printf(TEXT("%s.%s -> %s.%s"), *ParsedConnection.SourceNodeGuid.ToString(), *ParsedConnection.SourcePinName.ToString(), *ExistingConnection.NodeGuid.ToString(), *ExistingConnection.PinName.ToString())
					: FString::Printf(TEXT("%s.%s -> %s.%s"), *ExistingConnection.NodeGuid.ToString(), *ExistingConnection.PinName.ToString(), *ParsedConnection.TargetNodeGuid.ToString(), *ParsedConnection.TargetPinName.ToString());
				OutPlan.ConnectionsRemoved.Add(SupersededLabel);
			}
		}
	}
}

bool FFlowGraphReconciler::ApplyNodeChanges(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	TMap<FGuid, UFlowNode*>& OutNewNodeMap,
	FString& OutErrorMessage)
{
	// Phase 1: create nodes and apply authored properties or declarations to existing nodes.
	if (!ExecutionPlan.NodesToAdd.IsEmpty() &&
		!UFlowGraphImporter::CreateFlowNodes(ExistingAsset, ExecutionPlan.NodesToAdd, OutNewNodeMap, OutErrorMessage))
	{
		return false;
	}

	for (const FFlowGraphParsedNode& NodeToUpdate : ExecutionPlan.NodesToUpdate)
	{
		UFlowNode* ExistingNode = ExistingAsset->GetNodes().FindRef(NodeToUpdate.NodeGuid);
		if (!ExistingNode)
		{
			continue;
		}
		ExistingNode->SetFlags(RF_Transactional);
		ExistingNode->Modify();

		if (!UFlowGraphImporter::SetNodeProperties(ExistingNode, NodeToUpdate.Properties, OutErrorMessage))
		{
			return false;
		}

#if WITH_EDITOR
		// Descriptor properties determine auto pins, so refresh those before applying authored pin declarations.
		ExistingNode->TryUpdateAutoDataPins();
#endif
		UFlowGraphImporter::ApplyDeclaredPins(ExistingNode, NodeToUpdate);
	}

	return true;
}

bool FFlowGraphReconciler::ApplyAddOnChanges(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	const TMap<FGuid, UFlowNode*>& NewNodeMap,
	FFlowReconcileResult& Result,
	FString& OutErrorMessage)
{
	TMap<FGuid, UFlowNode*> CombinedNodeMap = ExistingAsset->GetNodes();
	FFlowPendingAddOnPropertyChanges PendingPropertyChanges;

	// Phase 2: attach complete new and updated AddOn trees while callbacks remain deferred.
	if (!ExecutionPlan.NodesToAdd.IsEmpty() &&
		!UFlowGraphImporter::CreateNodeAddOns(
			CombinedNodeMap, ExecutionPlan.NodesToAdd, PendingPropertyChanges, OutErrorMessage))
	{
		return false;
	}

	for (const FFlowGraphParsedNode& NodeToUpdate : ExecutionPlan.NodesToUpdate)
	{
		UFlowNode* ExistingNode = ExistingAsset->GetNodes().FindRef(NodeToUpdate.NodeGuid);
		if (!ExistingNode || (NodeToUpdate.AddOns.IsEmpty() && !NodeToUpdate.bIsFullBlock))
		{
			continue;
		}

		if (!ReconcileNodeAddOns(ExistingNode, NodeToUpdate.NodeGuid, NodeToUpdate.AddOns, PendingPropertyChanges,
			Result.Plan.AddOnsAdded, Result.Plan.AddOnsUpdated, Result.Plan.AddOnsDeleted,
			OutErrorMessage, NodeToUpdate.bIsFullBlock))
		{
			return false;
		}
	}

	// Phase 3: callbacks run only after attachment, against each AddOn's complete final tree.
	TArray<UFlowNodeBase*> AddOnRootOwners;
	for (const TPair<FGuid, UFlowNode*>& NewNodePair : NewNodeMap)
	{
		AddOnRootOwners.Add(NewNodePair.Value);
	}
	for (const FFlowGraphParsedNode& NodeToUpdate : ExecutionPlan.NodesToUpdate)
	{
		if (!NodeToUpdate.AddOns.IsEmpty() || NodeToUpdate.bIsFullBlock)
		{
			if (UFlowNode* ExistingNode = ExistingAsset->GetNodes().FindRef(NodeToUpdate.NodeGuid))
			{
				AddOnRootOwners.Add(ExistingNode);
			}
		}
	}
	if (!UFlowGraphImporter::DispatchPendingAddOnPropertyChanges(
		AddOnRootOwners, PendingPropertyChanges, OutErrorMessage))
	{
		return false;
	}

#if WITH_EDITOR
	// Phase 4: refresh parent auto pins only after AddOn callbacks refreshed their cached pins.
	for (const TPair<FGuid, UFlowNode*>& NewNodeEntry : NewNodeMap)
	{
		if (UFlowNode* NewNode = NewNodeEntry.Value)
		{
			NewNode->TryUpdateAutoDataPins();
		}
	}
	for (const FFlowGraphParsedNode& NodeToUpdate : ExecutionPlan.NodesToUpdate)
	{
		if (NodeToUpdate.AddOns.IsEmpty() && !NodeToUpdate.bIsFullBlock)
		{
			continue;
		}
		if (UFlowNode* ExistingNode = ExistingAsset->GetNodes().FindRef(NodeToUpdate.NodeGuid))
		{
			ExistingNode->TryUpdateAutoDataPins();
		}
	}
#endif

	// Runtime endpoints must match the dry-run preview before any connection is written.
	return FFlowGraphValidation::DoesRuntimePinSchemaMatchProjected(
		ExistingAsset, ExecutionPlan.ProjectedPinSchema, OutErrorMessage);
}

bool FFlowGraphReconciler::ApplyConnectionChanges(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	const TMap<FGuid, UFlowNode*>& CombinedNodeMap,
	FString& OutErrorMessage)
{
	// Phase 5: install and remove requested connections against the verified active schema.
	if (!ExecutionPlan.ConnectionsToAdd.IsEmpty() &&
		!UFlowGraphImporter::SetupConnections(ExistingAsset, CombinedNodeMap, ExecutionPlan.ConnectionsToAdd,
			ExecutionPlan.AllParsedNodes, OutErrorMessage))
	{
		return false;
	}

	for (const FFlowGraphParsedConnection& ConnectionToRemove : ExecutionPlan.ConnectionsToRemove)
	{
		UFlowNode* RemoveSourceNode = ExistingAsset->GetNodes().FindRef(ConnectionToRemove.SourceNodeGuid);
		UFlowNode* RemoveTargetNode = ExistingAsset->GetNodes().FindRef(ConnectionToRemove.TargetNodeGuid);
		const bool bRemoveIsExecPin = IsExecConnectionSourcePin(RemoveSourceNode, ConnectionToRemove.SourcePinName);

		// Data connection entries are stored on the target node; exec entries are stored on the source.
		if (UFlowNode* KeyNodeToClear = bRemoveIsExecPin ? RemoveSourceNode : RemoveTargetNode)
		{
			const FName KeyPinToClear = bRemoveIsExecPin
				? ConnectionToRemove.SourcePinName
				: ConnectionToRemove.TargetPinName;
			RemoveConnectionFromSourceNode(KeyNodeToClear, KeyPinToClear);
			BreakEditorConnectionLink(ConnectionToRemove, *ExistingAsset);
		}
	}

	return true;
}

bool FFlowGraphReconciler::RegraphAndVerify(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	FFlowReconcileResult& Result,
	FString& OutErrorMessage)
{
	if (UPackage* Package = ExistingAsset->GetOutermost())
	{
		Package->MarkPackageDirty();
	}

	// Retain parsed GUIDs so aliases and explicit positions use the same identities as the applied plan.
	TMap<FGuid, FIntPoint> NodePositions;
	for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
	{
		if (ParsedNode.bHasPos && !ParsedNode.bIsDeleteMarker)
		{
			NodePositions.Add(ParsedNode.NodeGuid, ParsedNode.Pos);
		}
	}

	if (!ExecutionPlan.NodesToAdd.IsEmpty())
	{
		TArray<FGuid> NewNodeGuidsForLayout;
		NewNodeGuidsForLayout.Reserve(ExecutionPlan.NodesToAdd.Num());
		for (const FFlowGraphParsedNode& AddedNode : ExecutionPlan.NodesToAdd)
		{
			NewNodeGuidsForLayout.Add(AddedNode.NodeGuid);
		}
		FFlowGraphLayout::ComputeAutoPlacedPositions(
			ExistingAsset, NewNodeGuidsForLayout, ExecutionPlan.AllParsedConnections, NodePositions);
	}

	TMap<FGuid, FString> NodeComments;
	for (const FFlowGraphParsedNode& ParsedNode : ExecutionPlan.AllParsedNodes)
	{
		if (ParsedNode.bHasComment && !ParsedNode.bIsDeleteMarker)
		{
			NodeComments.Add(ParsedNode.NodeGuid, ParsedNode.NodeComment);
		}
	}

	if (!UFlowGraphRegrapher::RegraphFlowAsset(ExistingAsset, NodePositions, NodeComments))
	{
		OutErrorMessage = TEXT("Applied the mutation but failed to regraph the asset");
		Result.Findings.Add(OutErrorMessage);
		return false;
	}

	// Verify only after destructive phases and the final regraph have completed.
	TArray<FString> VerifiedConnectionsAdded;
	TArray<FString> NotLandedConnections;
	FFlowGraphReconciler::VerifyConnectionsLanded(
		ExistingAsset, ExecutionPlan.ConnectionsToAdd, VerifiedConnectionsAdded, NotLandedConnections);
	for (const FString& NotLandedLabel : NotLandedConnections)
	{
		Result.Findings.Add(FString::Printf(TEXT("Requested connection did not land: %s"), *NotLandedLabel));
	}
	Result.Plan.ConnectionsAdded = MoveTemp(VerifiedConnectionsAdded);

	TArray<FString> GraphIntegrityIssues;
	UFlowGraphRegrapher::CollectGraphParityIssues(ExistingAsset, GraphIntegrityIssues);
	for (const FString& GraphIntegrityIssue : GraphIntegrityIssues)
	{
		Result.Findings.Add(GraphIntegrityIssue);
	}
	if (!NotLandedConnections.IsEmpty() || !GraphIntegrityIssues.IsEmpty())
	{
		OutErrorMessage = TEXT("Final Flow graph postcondition verification failed");
		return false;
	}

	return true;
}

bool FFlowGraphReconciler::ApplyPlanToExistingAsset(
	UFlowAsset* ExistingAsset,
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	FFlowReconcileResult& Result,
	FString& OutErrorMessage)
{
	// Apply the same AddOn callback ordering as preview, verify refreshed pins, and only then
	// install generated-pin connections. An owned transaction lets the MCP caller abort any failed step.
	TUniquePtr<FScopedTransaction> LocalTransaction;
	if (!GEditor || !GEditor->Trans || !GEditor->Trans->IsActive())
	{
		LocalTransaction = MakeUnique<FScopedTransaction>(
			NSLOCTEXT("FlowGraphCourier", "ReconcileFlowGraph", "Reconcile Flow Graph"));
	}
	TArray<FName> AssetPropertyNames;
	ExecutionPlan.StagedAssetProperties.GenerateKeyArray(AssetPropertyNames);
	AssetPropertyNames.Sort(FNameLexicalLess());
	for (const FName PropertyName : AssetPropertyNames)
	{
		const FFlowStagedAssetProperty& StagedProperty = ExecutionPlan.StagedAssetProperties.FindChecked(PropertyName);
		if (!StagedProperty.bChanged)
		{
			continue;
		}
		if (!FFlowAssetPropertyPolicy::SetStagedObjectReference(
			ExistingAsset, StagedProperty, /*bSendEditNotifications=*/true, OutErrorMessage))
		{
			Result.Findings.Add(OutErrorMessage);
			return false;
		}
		if (UPackage* Package = ExistingAsset->GetOutermost())
		{
			Package->MarkPackageDirty();
		}
	}
	if (UEdGraph* Graph = ExistingAsset->GetGraph())
	{
		Graph->SetFlags(RF_Transactional);
		Graph->Modify();
	}

	FString ApplyError;

	// Apply order follows spec section 6's fixed kind order: UpsertNode, UpsertAddon,
	// AddConnection, RemoveConnection, DeleteAddon, DeleteNode. DeleteAddon is folded into the
	// UpsertAddon step below (ReconcileNodeAddOns reconciles a node's whole addon set - add,
	// update, and delete - in one pass), which does not create a cross-node hazard since addon
	// deletes only ever affect their own owning node. DeleteNode runs last so a document may
	// delete a node it also connected or addon'd earlier without ordering hazards;
	// RemoveNodeAndReferencingConnections also cleans up any connections still pointing at it.

	// Phase 1: create nodes and apply their reflected properties and declared pins.
	TMap<FGuid, UFlowNode*> NewNodeMap;
	if (!ApplyNodeChanges(ExistingAsset, ExecutionPlan, NewNodeMap, ApplyError))
	{
		OutErrorMessage = ApplyError;
		Result.Findings.Add(ApplyError);
		return false;
	}

	// Re-fetch after node adds so the addon and connection steps below can resolve newly-created
	// nodes alongside pre-existing ones.
	TMap<FGuid, UFlowNode*> CombinedNodeMap = ExistingAsset->GetNodes();

	// Phases 2-4: reconcile full AddOn trees, dispatch callbacks, and refresh parent pins.
	if (!ApplyAddOnChanges(ExistingAsset, ExecutionPlan, NewNodeMap, Result, ApplyError))
	{
		OutErrorMessage = ApplyError;
		Result.Findings.Add(ApplyError);
		return false;
	}

	// Phase 5: mutate connections only after live pins match the projected active schema.
	if (!ApplyConnectionChanges(ExistingAsset, ExecutionPlan, CombinedNodeMap, ApplyError))
	{
		OutErrorMessage = ApplyError;
		Result.Findings.Add(ApplyError);
		return false;
	}

	// Phase 6: remove nodes last, after connection updates and AddOn reconciliation are complete.
	for (const FGuid& NodeGuidToDelete : ExecutionPlan.NodesToDelete)
	{
		if (!RemoveNodeAndReferencingConnections(ExistingAsset, NodeGuidToDelete))
		{
			OutErrorMessage = FString::Printf(TEXT("Failed to delete node %s"), *NodeGuidToDelete.ToString());
			Result.Findings.Add(OutErrorMessage);
			return false;
		}
	}

	// Phase 7: rebuild editor state and verify that the final connections and graph structure agree.
	if (!RegraphAndVerify(ExistingAsset, ExecutionPlan, Result, OutErrorMessage))
	{
		return false;
	}
	return true;
}

TSharedPtr<FFlowReconcileExecutionPlan> FFlowGraphReconciler::ComputeReconcilePlan(
	const FString& TargetAssetPath,
	const FString& CourierDocumentJson,
	FString& OutErrorMessage)
{
	OutErrorMessage.Empty();

	if (CourierDocumentJson.IsEmpty())
	{
		OutErrorMessage = TEXT("Courier document is empty");
		return nullptr;
	}

	TSharedPtr<FFlowReconcileExecutionPlan> Plan = MakeShared<FFlowReconcileExecutionPlan>();
	Plan->InputSizeBytes = CourierDocumentJson.Len();

	// Hard failure only for a document too malformed to reason about at all (bad JSON, wrong
	// formatVersion). Every other problem the converter finds (identity ambiguity, bad GUIDs,
	// unresolved aliases, field legality) is appended to ValidationFindings below and handled the
	// same way a semantic FFlowGraphValidation finding is - as data, never by failing this call.
	FFlowCourierDocument Document;
	TArray<FFlowCourierIssue> CourierIssues;
	if (!FFlowCourierConverter::ParseDocument(CourierDocumentJson, Document, CourierIssues, OutErrorMessage))
	{
		return nullptr;
	}

	TArray<FFlowGraphParsedNode> ParsedNodes;
	TArray<FFlowGraphParsedConnection> ParsedConnections;
	TArray<FGuid> ScopedNodeGuids;
	TMap<FString, FGuid> AliasMap;
	FFlowCourierConverter::ConvertToParsedGraph(Document, ParsedNodes, ParsedConnections, ScopedNodeGuids, AliasMap, CourierIssues);

	AppendCourierIssuesAsFindings(CourierIssues, Plan->ValidationFindings);

	UFlowAsset* ExistingAsset = FindOrLoadFlowAsset(TargetAssetPath);
	UClass* ResolvedAssetClass = ExistingAsset ? ExistingAsset->GetClass() : UFlowAsset::StaticClass();
	if (!ExistingAsset && !Document.AssetClass.IsEmpty() && !Document.AssetClass.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		UClass* const DocumentAssetClass = LoadObject<UClass>(nullptr, *Document.AssetClass);
		if (DocumentAssetClass && DocumentAssetClass->IsChildOf(UFlowAsset::StaticClass()))
		{
			ResolvedAssetClass = DocumentAssetClass;
		}
	}
	Plan->ResolvedAssetClass = ResolvedAssetClass;
	StageAssetProperties(Document.AssetProperties, ResolvedAssetClass, ExistingAsset, *Plan);

	// No asset yet: all nodes are adds, no validation possible.
	if (!ExistingAsset)
	{
		if (!ComputePlanForNewAsset(Document.AssetClass, Document.bWorldBound, ParsedNodes, ParsedConnections, *Plan, OutErrorMessage))
		{
			return nullptr;
		}
		Plan->AliasMap = AliasMap;
		return Plan;
	}

	// Asset exists: classify, validate. Aliasing/GUID resolution already happened in ConvertToParsedGraph.
	Plan->AllParsedNodes = ParsedNodes;
	Plan->AllParsedConnections = ParsedConnections;
	Plan->AssetClassPath = Document.AssetClass;
	Plan->bWorldBound = Document.bWorldBound;

	// Spec section 4: on an existing asset, assetClass must match the real class or it is an
	// error. An empty assetClass is not a mismatch - it means the document is silent on class,
	// which is legal on a patch against an asset that already exists.
	if (!Document.AssetClass.IsEmpty() && !Document.AssetClass.Equals(ExistingAsset->GetClass()->GetPathName(), ESearchCase::CaseSensitive))
	{
		FFlowValidationFinding& Finding = Plan->ValidationFindings.AddDefaulted_GetRef();
		Finding.Severity = EFlowValidationSeverity::Error;
		Finding.Code = TEXT("ClassMismatch");
		Finding.Message = FString::Printf(TEXT("[ClassMismatch] document assetClass '%s' does not match existing asset class '%s'"), *Document.AssetClass, *ExistingAsset->GetClass()->GetPathName());
	}

	const TMap<FGuid, UFlowNode*>& CurrentNodes = ExistingAsset->GetNodes();
	const int32 TotalExistingBeforeApply = CurrentNodes.Num();

	// Full mode is authoritative over the whole asset: the effective scope is every existing node,
	// not just ScopedNodeGuids (which the converter forces empty in Full mode - ScopeWithFullMode
	// is a validation error otherwise). Patch mode keeps the document-declared scope unchanged.
	TArray<FGuid> EffectiveScopedNodeGuids = ScopedNodeGuids;
	if (Document.Mode == EFlowCourierMode::Full)
	{
		EffectiveScopedNodeGuids.Reset();
		CurrentNodes.GenerateKeyArray(EffectiveScopedNodeGuids);
	}

	ClassifyParsedNodes(Plan->AllParsedNodes, EffectiveScopedNodeGuids, CurrentNodes,
		Plan->NodesToAdd, Plan->NodesToUpdate, Plan->NodesToDelete, Plan->AliasMap);

	// The converter's alias map is authoritative because ClassifyParsedNodes tracks node aliases only.
	Plan->AliasMap = AliasMap;

	Plan->Plan.NodesDeleted = Plan->NodesToDelete;
	for (const FFlowGraphParsedNode& NodeToAdd : Plan->NodesToAdd)
	{
		Plan->Plan.NodesAdded.Add(NodeToAdd.NodeGuid);
	}
	for (const FFlowGraphParsedNode& NodeToUpdate : Plan->NodesToUpdate)
	{
		Plan->Plan.NodesUpdated.Add(NodeToUpdate.NodeGuid);
	}

	// Full mode also implicitly removes any existing connection the document doesn't re-declare.
	// Appended as synthetic delete markers so ClassifyParsedConnections' existing idempotency
	// check (only report a removal if the connection is actually present) applies unchanged.
	if (Document.Mode == EFlowCourierMode::Full)
	{
		TArray<FFlowGraphParsedConnection> ExistingConnections;
		CollectExistingConnections(CurrentNodes, ExistingConnections);

		TSet<FString> DeclaredConnectionKeys;
		DeclaredConnectionKeys.Reserve(Plan->AllParsedConnections.Num());
		for (const FFlowGraphParsedConnection& ParsedConnection : Plan->AllParsedConnections)
		{
			if (!ParsedConnection.bIsDeleteMarker)
			{
				DeclaredConnectionKeys.Add(FString::Printf(TEXT("%s.%s -> %s.%s"),
					*ParsedConnection.SourceNodeGuid.ToString(), *ParsedConnection.SourcePinName.ToString(),
					*ParsedConnection.TargetNodeGuid.ToString(), *ParsedConnection.TargetPinName.ToString()));
			}
		}

		for (const FFlowGraphParsedConnection& ExistingConnection : ExistingConnections)
		{
			const FString Key = FString::Printf(TEXT("%s.%s -> %s.%s"),
				*ExistingConnection.SourceNodeGuid.ToString(), *ExistingConnection.SourcePinName.ToString(),
				*ExistingConnection.TargetNodeGuid.ToString(), *ExistingConnection.TargetPinName.ToString());
			if (!DeclaredConnectionKeys.Contains(Key))
			{
				FFlowGraphParsedConnection& DeleteMarker = Plan->AllParsedConnections.Add_GetRef(ExistingConnection);
				DeleteMarker.bIsDeleteMarker = true;
			}
		}
	}

	ClassifyParsedConnections(Plan->AllParsedConnections, CurrentNodes,
		Plan->ConnectionsToAdd, Plan->ConnectionsToRemove, Plan->Plan);

	Plan->NodesTouched = Plan->NodesToAdd.Num() + Plan->NodesToUpdate.Num() + Plan->NodesToDelete.Num();
	Plan->NodesPreserved = TotalExistingBeforeApply - (Plan->NodesToUpdate.Num() + Plan->NodesToDelete.Num());

	// Pre-commit validation on the fully-resolved document and its isolated final pin schema.
	const TSet<FGuid> PostPlanNodeGuids = BuildPostPlanNodeSet(CurrentNodes, Plan->NodesToAdd, Plan->NodesToDelete);
	FString PreviewError;
	if (!BuildEffectivePinSchema(
		ExistingAsset, *Plan, Plan->ProjectedPinSchema, PreviewError, &Plan->EffectiveStateAsset))
	{
		FFlowValidationFinding& Finding = Plan->ValidationFindings.AddDefaulted_GetRef();
		Finding.Severity = EFlowValidationSeverity::Error;
		Finding.Code = TEXT("PinSchemaPreviewFailed");
		Finding.Message = PreviewError;
	}
	FFlowValidationContext ValidationContext;
	ValidationContext.TargetAsset = ExistingAsset;
	ValidationContext.EffectiveAsset = Plan->EffectiveStateAsset.Get();
	ValidationContext.ResolvedAssetClass = ResolvedAssetClass;
	ValidationContext.StagedAssetProperties = &Plan->StagedAssetProperties;
	const UFlowAsset* ValidationAsset = Plan->EffectiveStateAsset.IsValid()
		? Plan->EffectiveStateAsset.Get()
		: ExistingAsset;
	FFlowGraphValidation::ValidateDocument(ValidationAsset, Plan->AllParsedNodes,
		Plan->AllParsedConnections, Plan->ValidationFindings, &PostPlanNodeGuids,
		&Plan->ProjectedPinSchema, &ValidationContext);

	return Plan;
}

FFlowReconcileResult FFlowGraphReconciler::ExecuteReconcilePlan(
	const FFlowReconcileExecutionPlan& ExecutionPlan,
	const FString& TargetAssetPath,
	FString& OutErrorMessage)
{
	FFlowReconcileResult Result;
	OutErrorMessage.Empty();

	Result.Plan = ExecutionPlan.Plan;
	Result.AliasMap = ExecutionPlan.AliasMap;
	Result.ValidationFindings = ExecutionPlan.ValidationFindings;
	Result.NodesTouched = ExecutionPlan.NodesTouched;
	Result.NodesPreserved = ExecutionPlan.NodesPreserved;
	Result.InputSizeBytes = ExecutionPlan.InputSizeBytes;

	// Refuse to apply if validation found any blocking errors.
	const bool bHasBlockingError = ExecutionPlan.ValidationFindings.ContainsByPredicate(
		[](const FFlowValidationFinding& F) { return F.Severity == EFlowValidationSeverity::Error; });

	if (bHasBlockingError)
	{
		OutErrorMessage = TEXT("Validation failed - see ValidationFindings; the asset was not modified");
		Result.Findings.Add(OutErrorMessage);
		return Result;
	}

	if (ExecutionPlan.bIsNewAsset)
	{
		if (ExecutePlanForNewAsset(ExecutionPlan, TargetAssetPath, Result, OutErrorMessage))
		{
			Result.bSuccess = true;
		}
		return Result;
	}

	UFlowAsset* ExistingAsset = FindOrLoadFlowAsset(TargetAssetPath);
	if (!ExistingAsset)
	{
		OutErrorMessage = FString::Printf(TEXT("Asset no longer exists at %s; cannot apply plan"), *TargetAssetPath);
		Result.Findings.Add(OutErrorMessage);
		return Result;
	}

	if (ApplyPlanToExistingAsset(ExistingAsset, ExecutionPlan, Result, OutErrorMessage))
	{
		Result.bSuccess = true;
	}

	return Result;
}

bool FFlowGraphReconciler::RemoveNodeAndReferencingConnections(UFlowAsset* FlowAsset, const FGuid& NodeGuidToRemove)
{
	if (!FlowAsset)
	{
		return false;
	}

	FMapProperty* NodesProperty = FindFProperty<FMapProperty>(UFlowAsset::StaticClass(), TEXT("Nodes"));
	if (!NodesProperty)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowGraphReconciler::RemoveNodeAndReferencingConnections: Could not find Nodes property on UFlowAsset"));
		return false;
	}

	FStructProperty* KeyProperty = CastField<FStructProperty>(NodesProperty->KeyProp);
	FObjectProperty* ValueProperty = CastField<FObjectProperty>(NodesProperty->ValueProp);
	if (!KeyProperty || !ValueProperty)
	{
		return false;
	}

	void* NodesPtr = NodesProperty->ContainerPtrToValuePtr<void>(FlowAsset);
	FScriptMapHelper NodesMapHelper(NodesProperty, NodesPtr);

	FMapProperty* ConnectionsProperty = FindFProperty<FMapProperty>(UFlowNode::StaticClass(), TEXT("Connections"));
	FStructProperty* ConnectionsValueProperty = ConnectionsProperty ? CastField<FStructProperty>(ConnectionsProperty->ValueProp) : nullptr;

	// Pass 1: find the internal index of the node to remove and drop any connection entries on
	// other nodes that reference it. Collect first, mutate after - FScriptMapHelper is sparse,
	// so removing a captured index after the scan is safe.
	int32 InternalIndexToRemove = INDEX_NONE;

	for (int32 Index = 0; Index < NodesMapHelper.GetMaxIndex(); ++Index)
	{
		if (!NodesMapHelper.IsValidIndex(Index))
		{
			continue;
		}

		uint8* PairPtr = NodesMapHelper.GetPairPtr(Index);
		const FGuid* KeyGuid = reinterpret_cast<const FGuid*>(KeyProperty->ContainerPtrToValuePtr<void>(PairPtr));
		if (KeyGuid && *KeyGuid == NodeGuidToRemove)
		{
			InternalIndexToRemove = Index;
		}

		UFlowNode* Node = Cast<UFlowNode>(ValueProperty->GetObjectPropertyValue(ValueProperty->ContainerPtrToValuePtr<void>(PairPtr)));
		if (!Node || !ConnectionsProperty || !ConnectionsValueProperty)
		{
			continue;
		}

		void* ConnectionsPtr = ConnectionsProperty->ContainerPtrToValuePtr<void>(Node);
		FScriptMapHelper ConnMapHelper(ConnectionsProperty, ConnectionsPtr);

		TArray<int32> ConnIndicesToRemove;
		for (int32 ConnIndex = 0; ConnIndex < ConnMapHelper.GetMaxIndex(); ++ConnIndex)
		{
			if (!ConnMapHelper.IsValidIndex(ConnIndex))
			{
				continue;
			}

			uint8* ConnPairPtr = ConnMapHelper.GetPairPtr(ConnIndex);
			const FConnectedPin* ConnectedPin = reinterpret_cast<const FConnectedPin*>(ConnectionsValueProperty->ContainerPtrToValuePtr<void>(ConnPairPtr));
			if (ConnectedPin && ConnectedPin->NodeGuid == NodeGuidToRemove)
			{
				ConnIndicesToRemove.Add(ConnIndex);
			}
		}

		for (int32 ConnIndex : ConnIndicesToRemove)
		{
			ConnMapHelper.RemoveAt(ConnIndex);
		}

		if (!ConnIndicesToRemove.IsEmpty())
		{
			ConnMapHelper.Rehash();
		}
	}

	if (InternalIndexToRemove == INDEX_NONE)
	{
		return true;
	}

	NodesMapHelper.RemoveAt(InternalIndexToRemove);
	NodesMapHelper.Rehash();

	// A node has two halves and both have to go. Dropping only the runtime map entry leaves the
	// UFlowGraphNode in the editor graph, where it is still drawn, still holds the removed UFlowNode
	// alive in the package, and still carries pin links that the regraph at the end of this apply
	// would harvest straight back into the runtime Connections map - silently undoing this delete.
	UFlowGraphRegrapher::DestroyEditorNodeForRuntimeNode(FlowAsset, NodeGuidToRemove);
	return true;
}

bool FFlowGraphReconciler::RemoveConnectionFromSourceNode(UFlowNode* SourceNode, FName SourcePinName)
{
	if (!SourceNode)
	{
		return false;
	}
	if (!SourceNode->HasAnyFlags(RF_Transient))
	{
		SourceNode->SetFlags(RF_Transactional);
		SourceNode->Modify();
	}

	FMapProperty* ConnectionsProperty = FindFProperty<FMapProperty>(UFlowNode::StaticClass(), TEXT("Connections"));
	if (!ConnectionsProperty)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowGraphReconciler::RemoveConnectionFromSourceNode: Could not find Connections property on UFlowNode"));
		return false;
	}

	void* ConnectionsPtr = ConnectionsProperty->ContainerPtrToValuePtr<void>(SourceNode);
	FScriptMapHelper ConnMapHelper(ConnectionsProperty, ConnectionsPtr);

	const int32 Index = ConnMapHelper.FindMapIndexWithKey(&SourcePinName);
	if (Index == INDEX_NONE)
	{
		return true;
	}

	ConnMapHelper.RemoveAt(Index);
	ConnMapHelper.Rehash();
	return true;
}

bool FFlowGraphReconciler::ReconcileParsedAddOn(
	UFlowNodeBase* OwnerNode,
	const FGuid& OwnerNodeGuid,
	const FFlowGraphParsedNodeAddOn& ParsedAddOn,
	FArrayProperty* AddOnsProperty,
	FObjectProperty* ElementProperty,
	void* AddOnsValue,
	TMap<UFlowNodeAddOn*, TArray<FString>>& OutPendingPropertyChanges,
	TArray<FFlowReconcileAddonEntry>& OutAdded,
	TArray<FFlowReconcileAddonEntry>& OutUpdated,
	TArray<FFlowReconcileAddonEntry>& OutDeleted,
	FString& OutErrorMessage,
	bool bFullBlock)
{
	// Match one authored entry by GUID, retaining the live UObject whenever the identity already exists.
	FScriptArrayHelper AddOnsHelper(AddOnsProperty, AddOnsValue);
	int32 ExistingIndex = INDEX_NONE;
	UFlowNodeAddOn* ExistingAddOn = nullptr;
	for (int32 Index = 0; Index < AddOnsHelper.Num(); ++Index)
	{
		UFlowNodeAddOn* Candidate = Cast<UFlowNodeAddOn>(
			ElementProperty->GetObjectPropertyValue(AddOnsHelper.GetRawPtr(Index)));
		if (Candidate && Candidate->GetGuid() == ParsedAddOn.AddOnGuid)
		{
			ExistingIndex = Index;
			ExistingAddOn = Candidate;
			break;
		}
	}

	if (ParsedAddOn.bIsDeleteMarker)
	{
		if (ExistingIndex != INDEX_NONE)
		{
			FFlowReconcileAddonEntry DeletedEntry;
			DeletedEntry.OwnerNodeGuid = OwnerNodeGuid;
			DeletedEntry.AddOnGuid = ParsedAddOn.AddOnGuid;
			DeletedEntry.AddOnType = ExistingAddOn ? ExistingAddOn->GetClass()->GetPathName() : FString();
			AddOnsHelper.RemoveValues(ExistingIndex, 1);
			OutDeleted.Add(MoveTemp(DeletedEntry));
		}
		return true;
	}

	if (ExistingAddOn)
	{
		// Apply values now but let the caller notify after every sibling and child is reconciled.
		if (!UFlowGraphImporter::SetAddOnProperties(
			ExistingAddOn, ParsedAddOn.Properties, OutPendingPropertyChanges, OutErrorMessage))
		{
			return false;
		}

		FFlowReconcileAddonEntry UpdatedEntry;
		UpdatedEntry.OwnerNodeGuid = OwnerNodeGuid;
		UpdatedEntry.AddOnGuid = ParsedAddOn.AddOnGuid;
		UpdatedEntry.AddOnType = ExistingAddOn->GetClass()->GetPathName();
		UpdatedEntry.Properties = ParsedAddOn.Properties;
		OutUpdated.Add(MoveTemp(UpdatedEntry));

		if (ParsedAddOn.AddOns.IsEmpty() && !bFullBlock)
		{
			return true;
		}

		return ReconcileNodeAddOns(ExistingAddOn, ParsedAddOn.AddOnGuid, ParsedAddOn.AddOns,
			OutPendingPropertyChanges, OutAdded, OutUpdated, OutDeleted, OutErrorMessage, bFullBlock);
	}

	// New identities are attached only after authored values are set, then their children are built.
	UClass* AddOnClass = ResolveAddOnClass(ParsedAddOn.AddOnType, OutErrorMessage);
	if (!AddOnClass)
	{
		return false;
	}

	UFlowNodeAddOn* NewAddOn = NewObject<UFlowNodeAddOn>(OwnerNode, AddOnClass, NAME_None, RF_Transactional);
	if (!NewAddOn)
	{
		OutErrorMessage = FString::Printf(
			TEXT("ReconcileNodeAddOns: Failed to create AddOn of type: %s"), *ParsedAddOn.AddOnType);
		return false;
	}

	const FGuid NewAddOnGuid = ParsedAddOn.AddOnGuid.IsValid() ? ParsedAddOn.AddOnGuid : FGuid::NewGuid();
	NewAddOn->SetGuid(NewAddOnGuid);
	if (!UFlowGraphImporter::SetAddOnProperties(
		NewAddOn, ParsedAddOn.Properties, OutPendingPropertyChanges, OutErrorMessage))
	{
		return false;
	}

	const int32 NewIndex = AddOnsHelper.AddValue();
	ElementProperty->SetObjectPropertyValue(AddOnsHelper.GetRawPtr(NewIndex), NewAddOn);

	FFlowReconcileAddonEntry AddedEntry;
	AddedEntry.OwnerNodeGuid = OwnerNodeGuid;
	AddedEntry.AddOnGuid = NewAddOnGuid;
	AddedEntry.AddOnType = AddOnClass->GetPathName();
	AddedEntry.Properties = ParsedAddOn.Properties;
	OutAdded.Add(MoveTemp(AddedEntry));

	// New descendants are part of this AddOn tree but only its root is reported as a plan entry.
	return ParsedAddOn.AddOns.IsEmpty() || UFlowGraphImporter::CreateAddOnsRecursive(
		NewAddOn, ParsedAddOn.AddOns, OutPendingPropertyChanges, OutErrorMessage);
}

bool FFlowGraphReconciler::ReconcileNodeAddOns(UFlowNodeBase* OwnerNode, const FGuid& OwnerNodeGuid,
	const TArray<FFlowGraphParsedNodeAddOn>& ParsedAddOns,
	TMap<UFlowNodeAddOn*, TArray<FString>>& OutPendingPropertyChanges,
	TArray<FFlowReconcileAddonEntry>& OutAdded,
	TArray<FFlowReconcileAddonEntry>& OutUpdated,
	TArray<FFlowReconcileAddonEntry>& OutDeleted,
	FString& OutErrorMessage,
	bool bFullBlock)
{
	// Reconcile AddOn identity and properties without firing pin callbacks until the complete
	// sibling/child tree is attached; the shared dispatcher then refreshes caches before parent regraph.
	if (!OwnerNode)
	{
		OutErrorMessage = TEXT("ReconcileNodeAddOns: OwnerNode is null");
		return false;
	}

	if ((!ParsedAddOns.IsEmpty() || bFullBlock) && !OwnerNode->HasAnyFlags(RF_Transient))
	{
		OwnerNode->SetFlags(RF_Transactional);
		OwnerNode->Modify();
	}

	FArrayProperty* AddOnsProperty = FindFProperty<FArrayProperty>(UFlowNodeBase::StaticClass(), TEXT("AddOns"));
	if (!AddOnsProperty)
	{
		OutErrorMessage = TEXT("ReconcileNodeAddOns: Could not find AddOns property on UFlowNodeBase");
		return false;
	}

	FObjectProperty* ElemProp = CastField<FObjectProperty>(AddOnsProperty->Inner);
	if (!ElemProp)
	{
		OutErrorMessage = TEXT("ReconcileNodeAddOns: AddOns array element property is not an object property");
		return false;
	}

	void* AddOnsPtr = AddOnsProperty->ContainerPtrToValuePtr<void>(OwnerNode);

	// Resolve each parsed identity independently so delete, update, and add behavior stay focused.
	for (const FFlowGraphParsedNodeAddOn& ParsedAddOn : ParsedAddOns)
	{
		if (!ReconcileParsedAddOn(OwnerNode, OwnerNodeGuid, ParsedAddOn, AddOnsProperty, ElemProp,
			AddOnsPtr, OutPendingPropertyChanges, OutAdded, OutUpdated, OutDeleted,
			OutErrorMessage, bFullBlock))
		{
			return false;
		}
	}

	// Full blocks make the parsed list authoritative; remove any existing child it did not mention.
	if (bFullBlock)
	{
		FScriptArrayHelper FullBlockHelper(AddOnsProperty, AddOnsPtr);
		for (int32 Index = FullBlockHelper.Num() - 1; Index >= 0; --Index)
		{
			UFlowNodeAddOn* Candidate = Cast<UFlowNodeAddOn>(ElemProp->GetObjectPropertyValue(FullBlockHelper.GetRawPtr(Index)));
			if (!Candidate)
			{
				continue;
			}

			const bool bMentioned = HasParsedAddOnGuid(ParsedAddOns, Candidate->GetGuid());

			if (!bMentioned)
			{
				FFlowReconcileAddonEntry DeletedEntry;
				DeletedEntry.OwnerNodeGuid = OwnerNodeGuid;
				DeletedEntry.AddOnGuid = Candidate->GetGuid();
				DeletedEntry.AddOnType = Candidate->GetClass()->GetPathName();

				FullBlockHelper.RemoveValues(Index, 1);
				OutDeleted.Add(MoveTemp(DeletedEntry));
			}
		}
	}

	return true;
}

bool FFlowGraphReconciler::NormalizePropertyValueString(FProperty* Property, const FString& RawValue, FString& OutNormalizedValue, FString& OutErrorMessage)
{
	void* ScratchValue = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(ScratchValue);

	FString InnerError;
	// No owner on purpose: ScratchValue is raw malloc'd memory, not a live object's property, so
	// there is nothing an instanced subobject could be outered to. Passing an owner here would
	// construct a UObject that this function immediately destroys. A class-plus-fields value
	// therefore fails to normalize, which only affects FindOrCreateNode's match comparison.
	const bool bParsed = UFlowGraphImporter::SetPropertyFromString(Property, ScratchValue, RawValue, InnerError, nullptr);
	if (bParsed)
	{
		OutNormalizedValue = UFlowGraphExporter::GetPropertyValueAsString(Property, ScratchValue);
	}
	else
	{
		OutErrorMessage = FString::Printf(TEXT("could not parse value '%s' for property '%s': %s"), *RawValue, *Property->GetName(), *InnerError);
	}

	Property->DestroyValue(ScratchValue);
	FMemory::Free(ScratchValue);
	return bParsed;
}

bool FFlowGraphReconciler::FindOrCreateNode(
	const FString& TargetAssetPath,
	const FString& NodeType,
	const TMap<FString, FString>& MatchProperties,
	bool bDryRun,
	FGuid& OutNodeGuid,
	bool& OutCreated,
	FString& OutErrorMessage)
{
	OutNodeGuid.Invalidate();
	OutCreated = false;

	UClass* ResolvedClass = UFlowGraphImporter::ResolveNodeClass(NodeType);
	if (!ResolvedClass)
	{
		OutErrorMessage = FString::Printf(TEXT("FindOrCreateNode: could not resolve NodeType: %s"), *NodeType);
		return false;
	}

	// RS13: an unknown property name is a hard error, checked once against the class up front -
	// independent of whether any node instance exists yet to compare against. Each raw value is
	// also normalized here (parse -> re-stringify through the same path the create path uses) so
	// the match comparison below compares canonical forms on both sides - otherwise a caller
	// passing e.g. "123" for a float property would never match a node created from "123.0".
	TArray<FProperty*> MatchFProperties;
	TArray<FString> NormalizedMatchValues;
	MatchFProperties.Reserve(MatchProperties.Num());
	NormalizedMatchValues.Reserve(MatchProperties.Num());
	for (const TPair<FString, FString>& Pair : MatchProperties)
	{
		FProperty* Property = ResolvedClass->FindPropertyByName(FName(*Pair.Key));
		if (!Property)
		{
			OutErrorMessage = FString::Printf(TEXT("FindOrCreateNode: %s has no property named '%s'"), *ResolvedClass->GetName(), *Pair.Key);
			return false;
		}

		FString NormalizedValue;
		FString NormalizeError;
		if (!NormalizePropertyValueString(Property, Pair.Value, NormalizedValue, NormalizeError))
		{
			OutErrorMessage = FString::Printf(TEXT("FindOrCreateNode: %s"), *NormalizeError);
			return false;
		}

		MatchFProperties.Add(Property);
		NormalizedMatchValues.Add(NormalizedValue);
	}

	UFlowAsset* ExistingAsset = FindOrLoadFlowAsset(TargetAssetPath);
	if (!ExistingAsset)
	{
		OutErrorMessage = FString::Printf(TEXT("FindOrCreateNode: no existing asset at %s (this helper does not create the asset itself)"), *TargetAssetPath);
		return false;
	}

	// Exact-class match only (not IsA) - a subclass is a different type as far as this helper is
	// concerned, matching RS13's exact-equality philosophy applied to identity as well as values.
	TArray<FGuid> MatchingGuids;
	for (const TPair<FGuid, UFlowNode*>& NodePair : ExistingAsset->GetNodes())
	{
		UFlowNode* Node = NodePair.Value;
		if (!Node || Node->GetClass() != ResolvedClass)
		{
			continue;
		}

		bool bAllMatch = true;
		for (int32 PropIndex = 0; PropIndex < MatchFProperties.Num(); ++PropIndex)
		{
			FProperty* Property = MatchFProperties[PropIndex];
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
			const FString CurrentValue = UFlowGraphExporter::GetPropertyValueAsString(Property, ValuePtr);
			if (CurrentValue != NormalizedMatchValues[PropIndex])
			{
				bAllMatch = false;
				break;
			}
		}

		if (bAllMatch)
		{
			MatchingGuids.Add(NodePair.Key);
		}
	}

	if (!MatchingGuids.IsEmpty())
	{
		// Deterministic tiebreak (RS13): lowest FGuid::ToString() wins, same convention the
		// exporter already uses for stable ordering - never an ambiguity error, so a repeated
		// idempotent call never starts failing just because an unrelated later change added a
		// second coincidentally-matching node.
		MatchingGuids.Sort([](const FGuid& A, const FGuid& B) { return A.ToString() < B.ToString(); });
		OutNodeGuid = MatchingGuids[0];
		OutCreated = false;
		return true;
	}

	// No match: create exactly one new node via the same ComputeReconcilePlan/ExecuteReconcilePlan
	// pipeline apply_flow_patch uses, so the same pre-commit validation, auto-placement, and
	// regraph apply - never a hand-rolled parallel creation path.
	static const FString NewNodeAlias = TEXT("findorcreate");

	TSharedRef<FJsonObject> PropertiesObject = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : MatchProperties)
	{
		PropertiesObject->SetStringField(Pair.Key, Pair.Value);
	}

	TSharedRef<FJsonObject> OpObject = MakeShared<FJsonObject>();
	OpObject->SetStringField(TEXT("kind"), TEXT("UpsertNode"));
	OpObject->SetStringField(TEXT("newAlias"), NewNodeAlias);
	OpObject->SetStringField(TEXT("type"), ResolvedClass->GetPathName());
	OpObject->SetObjectField(TEXT("properties"), PropertiesObject);

	TArray<TSharedPtr<FJsonValue>> OpsArray;
	OpsArray.Add(MakeShared<FJsonValueObject>(OpObject));

	UClass* ExpectedOwnerClass = ExistingAsset->GetExpectedOwnerClass();
	TSharedRef<FJsonObject> DocumentObject = MakeShared<FJsonObject>();
	DocumentObject->SetNumberField(TEXT("formatVersion"), 2);
	DocumentObject->SetStringField(TEXT("mode"), TEXT("Patch"));
	DocumentObject->SetStringField(TEXT("assetClass"), ExistingAsset->GetClass()->GetPathName());
	DocumentObject->SetBoolField(TEXT("bWorldBound"), ExistingAsset->bWorldBound);
	DocumentObject->SetStringField(TEXT("expectedOwnerClass"), ExpectedOwnerClass ? ExpectedOwnerClass->GetName() : FString());
	DocumentObject->SetArrayField(TEXT("ops"), OpsArray);

	FString MutationDocumentJson;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&MutationDocumentJson);
	FJsonSerializer::Serialize(DocumentObject, Writer);

	FString ComputeError;
	TSharedPtr<FFlowReconcileExecutionPlan> ExecutionPlan = ComputeReconcilePlan(TargetAssetPath, MutationDocumentJson, ComputeError);
	if (!ExecutionPlan)
	{
		OutErrorMessage = ComputeError.IsEmpty() ? TEXT("FindOrCreateNode: failed to compute creation plan") : ComputeError;
		return false;
	}

	const FGuid* MintedGuid = ExecutionPlan->AliasMap.Find(NewNodeAlias);
	if (!MintedGuid)
	{
		OutErrorMessage = TEXT("FindOrCreateNode: creation plan did not mint a GUID for the new node");
		return false;
	}

	if (bDryRun)
	{
		if (!FFlowGraphValidation::HasNoErrors(ExecutionPlan->ValidationFindings))
		{
			OutErrorMessage = TEXT("FindOrCreateNode: dry-run plan has blocking validation errors");
			return false;
		}

		OutNodeGuid = *MintedGuid;
		OutCreated = true;
		return true;
	}

	FString ExecuteError;
	FFlowReconcileResult Result = ExecuteReconcilePlan(*ExecutionPlan, TargetAssetPath, ExecuteError);
	if (!Result.bSuccess)
	{
		OutErrorMessage = ExecuteError.IsEmpty() ? TEXT("FindOrCreateNode: failed to execute creation plan") : ExecuteError;
		return false;
	}

	const FGuid* ExecutedGuid = Result.AliasMap.Find(NewNodeAlias);
	OutNodeGuid = ExecutedGuid ? *ExecutedGuid : *MintedGuid;
	OutCreated = true;
	return true;
}
