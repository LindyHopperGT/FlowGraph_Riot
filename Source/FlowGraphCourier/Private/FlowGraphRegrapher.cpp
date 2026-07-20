// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphRegrapher.h"
#include "FlowLogChannels.h"
#include "FlowAsset.h"
#include "FlowGraphExporter.h"
#include "FlowGraphImporter.h"
#include "Nodes/FlowNode.h"
#include "Graph/FlowGraph.h"
#include "Graph/FlowGraphSchema.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"

UFlowAsset* UFlowGraphRegrapher::ImportAndRegraphFromText(const FString& ImportText, const FString& AssetPath)
{
	if (ImportText.IsEmpty())
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromText: ImportText is empty"));
		return nullptr;
	}

	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromText: AssetPath is empty"));
		return nullptr;
	}

	FTopLevelAssetPath ParsedPath(AssetPath);
	if (ParsedPath.GetAssetName().IsNone())
	{
		ParsedPath.TrySetPath(*AssetPath, FPackageName::GetShortFName(AssetPath));
	}
	
	if (ParsedPath.GetAssetName().IsNone())
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromText: Failed to parse AssetName from path %s"), *AssetPath);
		return nullptr;
	}

	FString ErrorMessage;
	UFlowAsset* FlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(AssetPath, ImportText, ErrorMessage);
	
	if (!FlowAsset)
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromText: Import failed: %s"), *ErrorMessage);
		return nullptr;
	}

	if (!RegraphFlowAsset(FlowAsset))
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromText: Regraph failed for asset: %s"), *AssetPath);
		return nullptr;
	}

	return FlowAsset;
}

UFlowAsset* UFlowGraphRegrapher::ImportAndRegraphFromFile(const FString& FilePath, const FString& AssetPath)
{
	if (FilePath.IsEmpty())
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromFile: FilePath is empty"));
		return nullptr;
	}

	FString ImportText;
	if (!FFileHelper::LoadFileToString(ImportText, *FilePath))
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::ImportAndRegraphFromFile: Failed to load file: %s"), *FilePath);
		return nullptr;
	}

	return ImportAndRegraphFromText(ImportText, AssetPath);
}

bool UFlowGraphRegrapher::RegraphFlowAsset(UFlowAsset* FlowAsset)
{
	if (!FlowAsset)
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::RegraphFlowAsset: FlowAsset is null"));
		return false;
	}

	if (!GEditor)
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::RegraphFlowAsset: GEditor is not available"));
		return false;
	}

	if (GEditor->IsPlaySessionInProgress())
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowGraphRegrapher::RegraphFlowAsset: Cannot regraph during PIE"));
		return false;
	}

	// Ensure the FlowAsset has a UFlowGraph
	UFlowGraph* FlowGraph = Cast<UFlowGraph>(FlowAsset->GetGraph());
	if (!FlowGraph)
	{
		UFlowGraph::CreateGraph(FlowAsset);
		FlowGraph = CastChecked<UFlowGraph>(FlowAsset->GetGraph());
	}

	// Create UFlowGraphNodes for all UFlowNodes
	const TMap<FGuid, UFlowNode*>& RuntimeNodes = FlowAsset->GetNodes();
	
	for (const auto& NodePair : RuntimeNodes)
	{
		UFlowNode* RuntimeNode = NodePair.Value;
		if (!RuntimeNode)
		{
			continue;
		}

		// Check if an editor node already exists for this runtime node
		bool bAlreadyExists = false;
		for (UEdGraphNode* EdNode : FlowGraph->Nodes)
		{
			UFlowGraphNode* FlowEdNode = Cast<UFlowGraphNode>(EdNode);
			const UFlowNode* EdFlowNode = Cast<UFlowNode>(FlowEdNode ? FlowEdNode->GetFlowNodeBase() : nullptr);
			if (EdFlowNode && EdFlowNode->GetGuid() == RuntimeNode->GetGuid())
			{
				bAlreadyExists = true;
				break;
			}
		}

		if (!bAlreadyExists)
		{
			// Create a new UFlowGraphNode
			FGraphNodeCreator<UFlowGraphNode> NodeCreator(*FlowGraph);
			UFlowGraphNode* FlowGraphNode = NodeCreator.CreateNode();
			
			if (FlowGraphNode)
			{
				// NOTE - SetNodeTemplate (below) only sets NodeInstance on the editor node; it does 
				// NOT call SetGraphNode on the runtime node. Without this call here, any full-graph
				// HarvestNodeConnections triggered before RegraphFlowAsset finishes (e.g. from
				// CreateDefaultNodesForGraph) will crash on imported nodes whose GraphNode is null.
				RuntimeNode->SetGraphNode(FlowGraphNode);

				FlowGraphNode->SetNodeTemplate(RuntimeNode);
				FlowGraphNode->AllocateDefaultPins();
				NodeCreator.Finalize();
			}
		}
	}

	// Create missing data pins
	CreateMissingDataPins(FlowAsset);

	// Wire up the editor pins
	CreateEditorPinConnections(FlowAsset);

	FlowGraph->NotifyGraphChanged();

	// TODO (gtaylor) Downgrade these logs to Verbose after the import/export mechanism has stabilized.
	constexpr bool bLogRegraphedResult = true;
	if (bLogRegraphedResult)
	{
		const FString ExportText = UFlowGraphExporter::ExportFlowGraphToString(FlowAsset);

		UE_LOG(LogFlow, Log, TEXT("RegraphFlowAsset:\n%s"), *ExportText);
	}
	
	return true;
}

void UFlowGraphRegrapher::CreateMissingDataPins(UFlowAsset* FlowAsset)
{
	if (!FlowAsset)
	{
		return;
	}

	UFlowGraph* FlowGraph = Cast<UFlowGraph>(FlowAsset->GetGraph());
	if (!FlowGraph)
	{
		return;
	}

	const TMap<FGuid, UFlowNode*>& RuntimeNodes = FlowAsset->GetNodes();

	for (UEdGraphNode* EdNode : FlowGraph->Nodes)
	{
		UFlowGraphNode* FlowGraphNode = Cast<UFlowGraphNode>(EdNode);
		if (!FlowGraphNode)
		{
			continue;
		}

		UFlowNode* RuntimeNode = Cast<UFlowNode>(FlowGraphNode->GetFlowNodeBase()) ? RuntimeNodes.FindRef(Cast<UFlowNode>(FlowGraphNode->GetFlowNodeBase())->GetGuid()) : nullptr;
		if (!RuntimeNode)
		{
			continue;
		}

		// Check all runtime input pins - create editor pins if missing
		const TArray<FFlowPin>& InputPins = RuntimeNode->GetInputPins();
		for (const FFlowPin& RuntimePin : InputPins)
		{
			if (RuntimePin.IsExecPin())
			{
				continue;
			}

			bool bEditorPinExists = false;
			for (UEdGraphPin* EdPin : FlowGraphNode->Pins)
			{
				if (EdPin && EdPin->Direction == EGPD_Input && EdPin->PinName == RuntimePin.PinName)
				{
					bEditorPinExists = true;
					break;
				}
			}

			if (!bEditorPinExists)
			{
				FlowGraphNode->CreateInputPin(RuntimePin);
				UE_LOG(LogFlow, Log, TEXT("CreateMissingDataPins: Created input data pin %s on node %s"), 
					*RuntimePin.PinName.ToString(), *RuntimeNode->GetClass()->GetName());
			}
		}

		// Check all runtime output pins - create editor pins if missing
		const TArray<FFlowPin>& OutputPins = RuntimeNode->GetOutputPins();
		for (const FFlowPin& RuntimePin : OutputPins)
		{
			if (RuntimePin.IsExecPin())
			{
				continue;
			}

			bool bEditorPinExists = false;
			for (UEdGraphPin* EdPin : FlowGraphNode->Pins)
			{
				if (EdPin && EdPin->Direction == EGPD_Output && EdPin->PinName == RuntimePin.PinName)
				{
					bEditorPinExists = true;
					break;
				}
			}

			if (!bEditorPinExists)
			{
				FlowGraphNode->CreateOutputPin(RuntimePin);
				UE_LOG(LogFlow, Log, TEXT("CreateMissingDataPins: Created output data pin %s on node %s"), 
					*RuntimePin.PinName.ToString(), *RuntimeNode->GetClass()->GetName());
			}
		}
	}
}

void UFlowGraphRegrapher::CreateEditorPinConnections(UFlowAsset* FlowAsset)
{
	if (!FlowAsset)
	{
		return;
	}

	UFlowGraph* FlowGraph = Cast<UFlowGraph>(FlowAsset->GetGraph());
	if (!FlowGraph)
	{
		return;
	}

	// Build a lookup map from FlowNode guid to UFlowGraphNode
	TMap<FGuid, UFlowGraphNode*> NodeGuidToEditorNode;
	for (UEdGraphNode* EdNode : FlowGraph->Nodes)
	{
		UFlowGraphNode* FlowGraphNode = Cast<UFlowGraphNode>(EdNode);
		if (UFlowNode* FlowNode = Cast<UFlowNode>(FlowGraphNode ? FlowGraphNode->GetFlowNodeBase() : nullptr))
		{
			NodeGuidToEditorNode.Add(FlowNode->GetGuid(), FlowGraphNode);
		}
	}

	const TMap<FGuid, UFlowNode*>& RuntimeNodes = FlowAsset->GetNodes();

	for (const auto& NodePair : RuntimeNodes)
	{
		const FGuid& SourceGuid = NodePair.Key;
		UFlowNode* SourceRuntimeNode = NodePair.Value;

		if (!SourceRuntimeNode)
		{
			continue;
		}

		UFlowGraphNode** SourceEditorNodePtr = NodeGuidToEditorNode.Find(SourceGuid);
		if (!SourceEditorNodePtr || !*SourceEditorNodePtr)
		{
			continue;
		}

		UFlowGraphNode* SourceEditorNode = *SourceEditorNodePtr;

		// Wire up exec output pin connections (stored on source node)
		const TArray<FFlowPin>& OutputPins = SourceRuntimeNode->GetOutputPins();
		for (const FFlowPin& OutputPin : OutputPins)
		{
			if (!OutputPin.IsExecPin())
			{
				continue;
			}

			FConnectedPin Connection = SourceRuntimeNode->GetConnection(OutputPin.PinName);
			if (!Connection.NodeGuid.IsValid())
			{
				continue;
			}

			UFlowGraphNode** TargetEditorNodePtr = NodeGuidToEditorNode.Find(Connection.NodeGuid);
			if (!TargetEditorNodePtr || !*TargetEditorNodePtr)
			{
				continue;
			}

			UFlowGraphNode* TargetEditorNode = *TargetEditorNodePtr;

			UEdGraphPin* SourcePin = SourceEditorNode->FindPin(OutputPin.PinName, EGPD_Output);
			UEdGraphPin* TargetPin = TargetEditorNode->FindPin(Connection.PinName, EGPD_Input);

			if (SourcePin && TargetPin && !SourcePin->LinkedTo.Contains(TargetPin))
			{
				SourcePin->MakeLinkTo(TargetPin);
			}
		}

		// Wire up data input pin connections (stored on target/receiving node)
		const TArray<FFlowPin>& InputPins = SourceRuntimeNode->GetInputPins();
		for (const FFlowPin& InputPin : InputPins)
		{
			if (InputPin.IsExecPin())
			{
				continue;
			}

			FConnectedPin Connection = SourceRuntimeNode->GetConnection(InputPin.PinName);
			if (!Connection.NodeGuid.IsValid())
			{
				continue;
			}

			// Connection stores: source node guid and source output pin name
			UFlowGraphNode** DataSourceEditorNodePtr = NodeGuidToEditorNode.Find(Connection.NodeGuid);
			if (!DataSourceEditorNodePtr || !*DataSourceEditorNodePtr)
			{
				continue;
			}

			UFlowGraphNode* DataSourceEditorNode = *DataSourceEditorNodePtr;

			// Source's output pin -> This node's input pin
			UEdGraphPin* SourceOutputPin = DataSourceEditorNode->FindPin(Connection.PinName, EGPD_Output);
			UEdGraphPin* TargetInputPin = SourceEditorNode->FindPin(InputPin.PinName, EGPD_Input);

			if (SourceOutputPin && TargetInputPin && !SourceOutputPin->LinkedTo.Contains(TargetInputPin))
			{
				SourceOutputPin->MakeLinkTo(TargetInputPin);
			}
		}
	}

	// Run HarvestNodeConnections to ensure consistency between editor and runtime
	FlowAsset->HarvestNodeConnections();
}
