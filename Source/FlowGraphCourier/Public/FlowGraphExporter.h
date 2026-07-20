// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FlowGraphExporter.generated.h"

class UFlowAsset;
class UFlowNodeBase;
struct FFlowPin;

UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphExporter : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static bool ExportFlowGraphToText(UFlowAsset* FlowAsset, const FString& OutputFilePath);

	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static FString ExportFlowGraphToString(UFlowAsset* FlowAsset);

private:
	static void ExportHeader(const UFlowAsset* FlowAsset, FString& OutText);
	static void ExportNodes(const UFlowAsset* FlowAsset, FString& OutText);
	static void ExportNodeAddOns(const UFlowNodeBase* Node, FString& OutText);
	static void ExportConnections(const UFlowAsset* FlowAsset, FString& OutText);
	static FString GetPropertyValueAsString(const FProperty* Property, const void* ValuePtr);
	static FString GetPinTypeName(const FFlowPin& Pin);
};
