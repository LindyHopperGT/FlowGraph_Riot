// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Nodes/FlowPin.h"
#include "FlowGraphImporter.generated.h"

class UFlowAsset;
class UFlowNode;
class UFlowNodeAddOn;
class FMapProperty;

using FFlowGraphDeclaredOutputPinTypes = TMap<FGuid, TMap<FName, bool>>;

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedNodeAddOn
{
	GENERATED_BODY()

	FString AddOnType;
	TMap<FString, FString> Properties;
};

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedNode
{
	GENERATED_BODY()

	FGuid NodeGuid;
	FString NodeType;
	TMap<FString, FString> Properties;
	TArray<FString> InputPins;
	TArray<FString> OutputPins;
	TArray<FFlowGraphParsedNodeAddOn> AddOns;
};

USTRUCT()
struct FLOWGRAPHCOURIER_API FFlowGraphParsedConnection
{
	GENERATED_BODY()

	FGuid SourceNodeGuid;
	FName SourcePinName;
	FGuid TargetNodeGuid;
	FName TargetPinName;
};

UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphImporter : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static UFlowAsset* ImportFlowGraphFromText(const FString& TargetAssetPath, const FString& ImportText, FString& OutErrorMessage);

	// Populates an existing FlowAsset in-place from import text, preserving its derived class.
	// Clears existing runtime nodes and editor graph nodes, then reconstructs from the provided text.
	// Use this instead of ImportFlowGraphFromText when the target asset must retain a derived class (e.g. MKTActionFlowAsset).
	static bool PopulateFlowAssetFromText(UFlowAsset* TargetAsset, const FString& ImportText, FString& OutErrorMessage);

private:
	static bool ParseHeader(const FString& ImportText, FString& OutAssetPath, FGuid& OutAssetGuid, bool& OutWorldBound, FString& OutExpectedOwnerClass, FString& OutErrorMessage);
	static bool ParseNodes(const FString& ImportText, TArray<FFlowGraphParsedNode>& OutNodes, FString& OutErrorMessage);
	static bool ParseConnections(const FString& ImportText, TArray<FFlowGraphParsedConnection>& OutConnections, FString& OutErrorMessage);
	static bool CreateFlowNodes(UFlowAsset* FlowAsset, const TArray<FFlowGraphParsedNode>& ParsedNodes, TMap<FGuid, UFlowNode*>& OutNodeMap, FString& OutErrorMessage);
	static bool SetNodeProperties(UFlowNode* FlowNode, const TMap<FString, FString>& Properties, FString& OutErrorMessage);
	static bool SetupConnections(UFlowAsset* FlowAsset, const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedConnection>& Connections, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage);
	static bool CreateNodeAddOns(const TMap<FGuid, UFlowNode*>& NodeMap, const TArray<FFlowGraphParsedNode>& ParsedNodes, FString& OutErrorMessage);

	// SetupConnections helpers
	static FFlowGraphDeclaredOutputPinTypes BuildDeclaredOutputPinTypes(const TArray<FFlowGraphParsedNode>& ParsedNodes);
	static bool ResolveSourcePin(UFlowNode* SourceNode, FName SourcePinName, const FFlowGraphDeclaredOutputPinTypes& DeclaredOutputPinTypes, bool& bOutIsExecPin);
	static bool EnsureTargetDataPinExists(UFlowNode* TargetNode, FName TargetPinName);
	static bool WriteConnectionToNode(UFlowNode* NodeToModify, FName KeyName, const FConnectedPin& ConnectedPin, FMapProperty* ConnectionsProperty);

	// CreateFlowNodes helper
	static UClass* ResolveNodeClass(const FString& NodeType);

	static bool SetAddOnProperties(UFlowNodeAddOn* AddOn, const TMap<FString, FString>& Properties, FString& OutErrorMessage);

#if WITH_EDITORONLY_DATA
	static void SyncAddOnDataPinValueDirections(UFlowNodeAddOn* AddOn);
#endif

	static bool SetPropertyFromString(FProperty* Property, void* ValuePtr, const FString& ValueString, FString& OutErrorMessage);
	static FString ExtractSection(const FString& ImportText, const FString& SectionName);
	static bool ParseGuid(const FString& GuidString, FGuid& OutGuid);
	static bool ParseConnectionLine(const FString& Line, FFlowGraphParsedConnection& OutConnection, FString& OutErrorMessage);
};
