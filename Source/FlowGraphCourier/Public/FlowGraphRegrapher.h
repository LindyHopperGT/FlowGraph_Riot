// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FlowGraphRegrapher.generated.h"

class UFlowAsset;

/**
 * Helper class for rebuilding UEdGraph from UFlowAsset runtime nodes.
 * 
 * This class provides functionality to reconstruct the editor graph (UFlowGraph and UFlowGraphNodes)
 * from a UFlowAsset that contains runtime UFlowNode instances with connections.
 * 
 * Use case: After programmatically creating a UFlowAsset with UFlowNodes and setting up their
 * Connections maps, call RegraphFlowAsset() to generate the corresponding editor graph nodes
 * and wire them up properly.
 */
UCLASS()
class FLOWGRAPHCOURIER_API UFlowGraphRegrapher : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Imports a FlowGraph from text and creates the editor graph.
	 * 
	 * @param ImportText The text representation of the FlowGraph
	 * @param AssetPath Full asset path where the FlowAsset should be created (e.g., "/Game/MyFlows/MyFlow")
	 * @return The imported and regraphed FlowAsset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static UFlowAsset* ImportAndRegraphFromText(const FString& ImportText, const FString& AssetPath);

	/**
	 * Imports a FlowGraph from a file and creates the editor graph.
	 * 
	 * @param FilePath Absolute path to the file containing the FlowGraph text representation
	 * @param AssetPath Full asset path where the FlowAsset should be created (e.g., "/Game/MyFlows/MyFlow")
	 * @return The imported and regraphed FlowAsset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static UFlowAsset* ImportAndRegraphFromFile(const FString& FilePath, const FString& AssetPath);

	/**
	 * Rebuilds the editor graph (UFlowGraph) from a UFlowAsset's runtime nodes.
	 * 
	 * This function:
	 * 1. Ensures the FlowAsset has a UFlowGraph
	 * 2. Creates UFlowGraphNodes for all UFlowNodes
	 * 3. Wires up the editor pins based on the runtime Connections maps
	 * 4. Calls HarvestNodeConnections() to ensure consistency
	 * 
	 * @param FlowAsset The FlowAsset to rebuild the graph for
	 * @return True if successful, false if FlowAsset is null or graph creation failed
	 */
	UFUNCTION(BlueprintCallable, Category = "Flow Graph Courier")
	static bool RegraphFlowAsset(UFlowAsset* FlowAsset);

private:
	/**
	 * Creates missing data pins on EdGraphNodes that exist on runtime FlowNodes.
	 * AllocateDefaultPins() only creates default pins, not dynamic data pins from properties.
	 * 
	 * @param FlowAsset The FlowAsset containing the nodes
	 */
	static void CreateMissingDataPins(UFlowAsset* FlowAsset);

	/**
	 * Creates editor pin connections based on runtime node connections.
	 * 
	 * @param FlowAsset The FlowAsset containing the nodes
	 */
	static void CreateEditorPinConnections(UFlowAsset* FlowAsset);
};
