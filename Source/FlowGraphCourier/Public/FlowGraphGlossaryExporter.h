// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "UObject/NoExportTypes.h"
#include "FlowGraphGlossaryExporter.generated.h"

class UFlowNode;
class UFlowNodeBase;
class UFlowNodeAddOn;
struct FFlowPin;

/** One row of the "editable properties" table shown for a node/addon - shared by the markdown and JSON export paths so they never drift apart. */
struct FFlowGlossaryPropertyRow
{
	FString Name;
	FString Type;
	FString PinBinding;
	FString Description;
};

/** One row of a node's input/output pin table - shared by the markdown and JSON export paths. */
struct FFlowGlossaryPinRow
{
	FString Name;
	FString Type;
};

/**
 * Exports a markdown glossary of all available FlowNode types in the project.
 * This includes both C++ and Blueprint-implemented nodes.
 * The glossary is intended to provide context for LLMs when generating FlowAssets.
 */
UCLASS(BlueprintType)
class FLOWGRAPHCOURIER_API UFlowGraphGlossaryExporter : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Export a markdown glossary of all FlowNode types to a file.
	 * @param OutputFilePath The path to write the markdown file to.
	 * @return True if the export succeeded, false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowGraph|Export")
	static bool ExportGlossaryToFile(const FString& OutputFilePath);

	/**
	 * Export a markdown glossary of all FlowNode types to a string.
	 * @return The markdown glossary as a string.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowGraph|Export")
	static FString ExportGlossaryToString();

	/**
	 * Returns structured (JSON) detail for the requested node/addon classes: class path, category,
	 * description, keywords, properties (name/type/pin-binding/description), input/output pins
	 * (nodes only), and a `signature` (see Meerkat's Projects/Meerkat/agent-docs/Flow/GARDENING.md
	 * Section 5 for the canonical-string + sha256[:12] algorithm this implements server-side) -
	 * so callers never need to regex-parse markdown to get this data or compute a signature themselves.
	 * Unknown class names are omitted from the result (not an error).
	 * @param ClassNames Node or addon class names (as returned by UClass::GetName(), e.g. "MKTActionEvent_OnExecute").
	 * @return A JSON array string, one object per found class.
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowGraph|Export")
	static FString GetNodeOrAddOnDetailsAsJson(const TArray<FString>& ClassNames);

public:
	static void GatherAllNodeClasses(TArray<UClass*>& OutNodeClasses);
	
	static void ExportNodeToMarkdown(const UClass* NodeClass, FString& OutMarkdown);
	
	static FString GetNodeDescription(const UClass* NodeClass);
	static FString GetNodeCategory(const UClass* NodeClass);
	
	static void ExportPropertiesToMarkdown(const UClass* NodeClass, FString& OutMarkdown);
	static void ExportPinsToMarkdown(const UClass* NodeClass, FString& OutMarkdown);
	
	static FString GetPropertyTypeName(const FProperty* Property);
	static FString GetPropertyTooltip(const FProperty* Property);
	static FString GetPropertyPinBinding(const FProperty* Property);
	
	static FString GetPinTypeName(const FFlowPin& Pin);
	
	static bool CanNodeAddUserInputPins(const UClass* NodeClass);
	static bool CanNodeAddUserOutputPins(const UClass* NodeClass);

	static void GatherAllAddOnClasses(TArray<UClass*>& OutAddOnClasses);
	static void GatherAllFlowAssetClasses(TArray<UClass*>& OutAssetClasses);

	/**
	 * Builds a map of keyword → sorted list of display names for every node and addon that carries that keyword.
	 * Keywords are split on whitespace and comma, trimmed, and lower-cased for deduplication.
	 * @param NodeClasses     The node classes to inspect (already gathered and filtered).
	 * @param AddOnClasses    The addon classes to inspect (already gathered and filtered).
	 * @param OutKeywordMap   Output map: keyword (lower-case) → sorted display-name list.
	 */
	static void GatherAllKeywords(
		const TArray<UClass*>& NodeClasses,
		const TArray<UClass*>& AddOnClasses,
		TMap<FString, TArray<FString>>& OutKeywordMap);

private:
	static void ExportAddOnToMarkdown(const UClass* AddOnClass, FString& OutMarkdown);
	static void ExportAddOnPropertiesToMarkdown(const UClass* AddOnClass, FString& OutMarkdown);
	static FString GetAddOnDescription(const UClass* AddOnClass);

	/* Returns Keywords metadata from the UCLASS, or empty string. */
	static FString GetNodeKeywords(const UClass* NodeClass);

	/** Filters a class's FProperty list down to the "editable properties" shown in the glossary (same filter `ExportPropertiesToMarkdown` renders from). */
	static void GatherEditableProperties(const UClass* NodeOrAddOnClass, TArray<FFlowGlossaryPropertyRow>& OutRows);

	/** Reads a node's input/output pins (empty for addons, which do not have their own pin lists). */
	static void GatherPins(const UClass* NodeClass, TArray<FFlowGlossaryPinRow>& OutInputPins, TArray<FFlowGlossaryPinRow>& OutOutputPins);

	/**
	 * Computes the KB staleness-diffing signature per Projects/Meerkat/agent-docs/Flow/GARDENING.md
	 * Section 5: canonical = ClassPath + "|" + sorted "name:type:pin_binding" properties + "|" +
	 * sorted input-pin names + "|" + sorted output-pin names, joined with "|"; signature =
	 * SHA-1(canonical) truncated to the first 12 hex chars (lowercase). Uses FSHA1::HashBuffer -
	 * a pure-software implementation - deliberately, NOT FPlatformMisc::GetSHA256Signature, which
	 * is unimplemented on this platform (confirmed live; see FLOW_CODEV_AGENT_PLAN.md Section 10).
	 */
	static FString ComputeSignature(
		const FString& ClassPath,
		const TArray<FFlowGlossaryPropertyRow>& Properties,
		const TArray<FFlowGlossaryPinRow>& InputPins,
		const TArray<FFlowGlossaryPinRow>& OutputPins);

	/**
	 * Exports each FlowAsset subclass's class/description/keywords, plus its resolved
	 * allowed-node and allowed-addon closure (via UFlowAsset::IsNodeOrAddOnClassAllowed),
	 * so agents can filter the node/addon catalog down to what is valid for a given
	 * asset type (e.g. the Action-Flow palette) without re-deriving Allowed/DeniedNodeClasses
	 * logic themselves.
	 * @param NodeClasses  The node classes to check (already gathered and filtered).
	 * @param AddOnClasses The addon classes to check (already gathered and filtered).
	 */
	static void ExportFlowAssetTypesToMarkdown(
		FString& OutMarkdown,
		const TArray<UClass*>& NodeClasses,
		const TArray<UClass*>& AddOnClasses);
};
