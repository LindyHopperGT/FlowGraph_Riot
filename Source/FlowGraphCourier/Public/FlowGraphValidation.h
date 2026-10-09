// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "UObject/Object.h"
#include "Nodes/FlowPin.h"
#include "FlowAssetPropertyPolicy.h"
#include "FlowGraphValidation.generated.h"

class UFlowAsset;
class UClass;
struct FFlowGraphParsedNode;
struct FFlowGraphParsedNodeAddOn;
struct FFlowGraphParsedConnection;

// One node's active schema after the planned AddOn tree has been applied. Orphan editor
// pins are intentionally absent because Courier must not treat them as valid connection endpoints.
struct FFlowProjectedPinSet
{
	// Active runtime input pins, including the final AddOn context inputs.
	TArray<FFlowPin> InputPins;
	// Active runtime output pins, including the final AddOn context outputs.
	TArray<FFlowPin> OutputPins;
};

// Maps each surviving node identity to the schema used for both dry-run and commit validation.
using FFlowProjectedPinSchema = TMap<FGuid, FFlowProjectedPinSet>;

UENUM(BlueprintType)
enum class EFlowValidationSeverity : uint8
{
	// Advisory only - never blocks an apply.
	Info,

	// Non-fatal - surfaced to the caller but does not block a non-dry-run apply.
	Warning,

	// Fatal - blocks a non-dry-run apply.
	Error,
};

/**
 * A single structured validation result. Optional fields are left at their default
 * (invalid FGuid / NAME_None) when they don't apply to a particular finding.
 */
USTRUCT(BlueprintType)
struct FLOWGRAPHCOURIER_API FFlowValidationFinding
{
	GENERATED_BODY()

	// Severity controls whether a finding blocks commit or is advisory only.
	UPROPERTY()
	EFlowValidationSeverity Severity = EFlowValidationSeverity::Error;

	// Stable machine-readable identifier, e.g. "ExecFanOut", "DanglingEndpoint". Shares one code set
	// with FFlowCourierIssue::Code so a caller can branch on the code regardless of which system
	// produced the finding.
	UPROPERTY()
	FString Code;

	// Optional source document path for findings about a supplied document field. Graph findings
	// about pre-existing elements leave this empty and identify those elements by GUID.
	UPROPERTY()
	FString SourceDocumentPath;

	// Human-readable explanation of the validation result.
	UPROPERTY()
	FString Message;

	// The node the finding is about (invalid when not node-specific).
	UPROPERTY()
	FGuid NodeGuid;

	// The addon the finding is about (invalid when not addon-specific).
	UPROPERTY()
	FGuid AddOnGuid;

	// The pin the finding is about (NAME_None when not pin-specific).
	UPROPERTY()
	FName PinName;
};

// A pluggable per-asset-class validation hook. Pure/read-only: given the target asset and parsed
// mutation document, append findings - never mutate anything. Takes the same (asset, nodes,
// connections) shape as ValidateDocument. This header has no dependency on FlowGraphReconciler.h.
DECLARE_DELEGATE_FourParams(FFlowValidationHookDelegate,
	const UFlowAsset* /*TargetAsset*/,
	const TArray<FFlowGraphParsedNode>& /*ParsedNodes*/,
	const TArray<FFlowGraphParsedConnection>& /*ParsedConnections*/,
	TArray<FFlowValidationFinding>& /*OutFindings*/);

struct FLOWGRAPHCOURIER_API FFlowValidationContext
{
	const UFlowAsset* TargetAsset = nullptr;
	const UFlowAsset* EffectiveAsset = nullptr;
	const UClass* ResolvedAssetClass = nullptr;
	const TMap<FName, FFlowStagedAssetProperty>* StagedAssetProperties = nullptr;

	const FFlowStagedAssetProperty* FindStagedAssetProperty(FName PropertyName) const
	{
		return StagedAssetProperties ? StagedAssetProperties->Find(PropertyName) : nullptr;
	}
};

DECLARE_DELEGATE_FourParams(FFlowContextValidationHookDelegate,
	const FFlowValidationContext& /*Context*/,
	const TArray<FFlowGraphParsedNode>& /*ParsedNodes*/,
	const TArray<FFlowGraphParsedConnection>& /*ParsedConnections*/,
	TArray<FFlowValidationFinding>& /*OutFindings*/);

/**
 * Deterministic, pluggable pre-commit validator for a Flow Courier mutation document. Runs on the
 * parsed document and target asset and produces structured findings. Errors block a non-dry-run
 * apply; warnings/info do not. Lives in the agnostic FlowGraphCourier module with no game-module
 * symbols. Per-asset-class rules are supplied by pluggable hooks registered from the owning
 * game module.
 *
 * Core checks:
 *  - Class resolution: every node/addon Type resolves to a real UClass.
 *  - Palette allowedness: every node/addon class passes UFlowAsset::IsNodeOrAddOnClassAllowed.
 *  - Attachment eligibility: every addon can legally attach to its parent per
 *    UFlowNodeBase::CheckAcceptFlowNodeAddOnChild (only an explicit Reject fails).
 *  - Connection topology: no fan-out (two connections from one output pin); when PostPlanNodeGuids
 *    is supplied, also no dangling node/pin references.
 *  - Pin type-compatibility: when PostPlanNodeGuids is supplied and both endpoints/pins exist, an
 *    exec-to-data cross connection is an Error, and a data-to-data connection the asset's
 *    FFlowPinConnectionPolicy rejects is a Warning (skipped for wildcard/Unknown/custom types the
 *    policy does not recognize, to avoid false positives).
 *  - GUID integrity: no duplicate node GUIDs; addon GUIDs unique within their owner; when
 *    PostPlanNodeGuids is supplied, every connection resolves to a node that still exists.
 *
 * Hook registries attach read-only checks when the resolved asset class IsChildOf their registered
 * class. Legacy hooks receive the target asset and parsed document; context hooks also receive the
 * effective preview asset and typed staged asset-property values. Both hook forms run when present.
 * The agnostic core ships with empty registries.
 *
 * Plain static-method class, not a UObject: it has no UFUNCTIONs and is never instantiated.
 */
class FLOWGRAPHCOURIER_API FFlowGraphValidation
{
public:
	/**
	 * Validates ParsedNodes/ParsedConnections against TargetAsset (whose concrete class supplies
	 * palette allowedness). Appends findings to OutFindings. Returns true when there are no
	 * Error-severity findings (safe to apply); false otherwise. Delete-marker nodes/addons and
	 * connections are skipped - a deletion cannot violate palette/attachment/topology.
	 *
	 * PostPlanNodeGuids: when supplied, it is the full set of node GUIDs that will exist after
	 * this document applies (existing nodes plus adds minus deletes). When present, two extra
	 * checks run: every non-delete connection's source/target node must be in this set, and its
	 * pin must actually exist on that node. Left null, both checks are skipped.
	 */
	static bool ValidateDocument(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		TArray<FFlowValidationFinding>& OutFindings,
		const TSet<FGuid>* PostPlanNodeGuids = nullptr,
		const FFlowProjectedPinSchema* ProjectedPinSchema = nullptr,
		const FFlowValidationContext* ValidationContext = nullptr);

	// True if OutFindings contains no Error-severity entries.
	static bool HasNoErrors(const TArray<FFlowValidationFinding>& Findings);

	// Confirms that execution produced the same active runtime pins used by pre-commit validation.
	// Retained editor orphan pins are not part of this runtime schema.
	static bool DoesRuntimePinSchemaMatchProjected(
		const UFlowAsset* FlowAsset,
		const FFlowProjectedPinSchema& ProjectedPinSchema,
		FString& OutErrorMessage);

	// Registers Hook to run (in addition to the core checks) whenever ValidateDocument's TargetAsset
	// class IsChildOf AssetClass. Call from your module's StartupModule - there is no automatic
	// unregistration on shutdown, so a module that can be unloaded should call UnregisterHooksForClass.
	static void RegisterHook(TSubclassOf<UFlowAsset> AssetClass, FFlowValidationHookDelegate Hook);

	// Removes every hook previously registered for exactly AssetClass (not its subclasses/parents -
	// this must match the same UClass pointer RegisterHook was called with).
	static void UnregisterHooksForClass(TSubclassOf<UFlowAsset> AssetClass);

	static void RegisterContextHook(TSubclassOf<UFlowAsset> AssetClass, FFlowContextValidationHookDelegate Hook);
	// Removes every context hook registered for exactly AssetClass.
	static void UnregisterContextHooksForClass(TSubclassOf<UFlowAsset> AssetClass);

private:
	// Recursively validates an addon subtree (class resolution, palette, attachment eligibility,
	// per-owner GUID uniqueness) under the given resolved owner class + owner node GUID.
	static void ValidateAddOnsRecursive(
		const UFlowAsset* TargetAsset,
		const UClass* OwnerClass,
		const FGuid& OwnerNodeGuid,
		const TArray<FFlowGraphParsedNodeAddOn>& ParsedAddOns,
		TArray<FFlowValidationFinding>& OutFindings);

	static void AddFinding(
		TArray<FFlowValidationFinding>& OutFindings,
		EFlowValidationSeverity Severity,
		const FString& Code,
		const FString& Message,
		const FGuid& NodeGuid = FGuid(),
		const FGuid& AddOnGuid = FGuid(),
		FName PinName = NAME_None);

	// True if NodeGuid has a pin named PinName in the given direction. Checks the document's own
	// declared pins first if this document touches that node, then the class's default pins;
	// falls back to the live node's pins via TargetAsset for a node this document doesn't mention.
	static bool DoesNodeHavePin(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const FGuid& NodeGuid,
		FName PinName,
		bool bIsOutputPin,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	// Resolves the declared pin type name (e.g. "Exec", "Text", "Vector") for PinName on NodeGuid,
	// using the same resolution order as DoesNodeHavePin (document declared pins -> class CDO pins ->
	// live node pins). Returns NAME_None when the pin or its type cannot be determined - callers must
	// treat that as "unknown, do not type-check".
	static FName GetConnectionPinType(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const FGuid& NodeGuid,
		FName PinName,
		bool bIsOutputPin,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	// Helper functions for ValidateDocument.

	static void ValidateNodes(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		TArray<FFlowValidationFinding>& OutFindings);

	static void ValidateConnections(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		TArray<FFlowValidationFinding>& OutFindings,
		const TSet<FGuid>* PostPlanNodeGuids,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	// Exec-only: an exec output pin may drive at most one target, but a data output pin may legally
	// fan out to several inputs and must not be flagged. Needs TargetAsset/ParsedNodes purely to
	// resolve the source pin's type - a fan-out whose source pin type cannot be resolved is skipped,
	// since ValidateConnectionEndpoints already reports the missing pin as PinNotFound.
	static void ValidateConnectionFanOut(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const FFlowGraphParsedConnection& Connection,
		TMap<TPair<FGuid, FName>, FString>& OutSeenSourcePins,
		TArray<FFlowValidationFinding>& OutFindings,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	static void ValidateConnectionEndpoints(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const FFlowGraphParsedConnection& Connection,
		const TSet<FGuid>& PostPlanNodeGuids,
		TArray<FFlowValidationFinding>& OutFindings,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	static void ValidateConnectionPinTypes(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const FFlowGraphParsedConnection& Connection,
		TArray<FFlowValidationFinding>& OutFindings,
		const FFlowProjectedPinSchema* ProjectedPinSchema);

	static void RunValidationHooks(
		const UFlowAsset* TargetAsset,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		TArray<FFlowValidationFinding>& OutFindings);

	static void RunContextValidationHooks(
		const FFlowValidationContext& Context,
		const TArray<FFlowGraphParsedNode>& ParsedNodes,
		const TArray<FFlowGraphParsedConnection>& ParsedConnections,
		TArray<FFlowValidationFinding>& OutFindings);
};
