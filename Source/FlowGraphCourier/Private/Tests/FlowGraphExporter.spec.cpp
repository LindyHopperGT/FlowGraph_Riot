// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphExporter.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Route/FlowNode_Reroute.h"
#include "Graph/FlowGraph.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "Misc/AutomationTest.h"
#include "EdGraph/EdGraphPin.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FFlowGraphExporterSpec, "FlowGraphCourier.EditorGame.Exporter", EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)
	UFlowAsset* TestFlowAsset;
END_DEFINE_SPEC(FFlowGraphExporterSpec)

void FFlowGraphExporterSpec::Define()
{
	Describe("FlowGraphExporter", [this]()
	{
		BeforeEach([this]()
		{
			TestFlowAsset = NewObject<UFlowAsset>(GetTransientPackage(), UFlowAsset::StaticClass());
			UFlowGraph::CreateGraph(TestFlowAsset);
		});

		AfterEach([this]()
		{
			if (TestFlowAsset)
			{
				TestFlowAsset->ClearFlags(RF_Standalone);
				TestFlowAsset->MarkAsGarbage();
				TestFlowAsset = nullptr;
			}
		});

		It("should export a simple flow graph with connected nodes", [this]()
		{
			UFlowGraph* FlowGraph = Cast<UFlowGraph>(TestFlowAsset->GetGraph());
			if (!FlowGraph) { return; }
			
			UFlowGraphNode* StartGraphNode = NewObject<UFlowGraphNode>(FlowGraph);
			StartGraphNode->CreateNewGuid();
			UFlowNode* StartFlowNode = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), StartGraphNode);
			StartGraphNode->SetNodeTemplate(StartFlowNode);
			StartGraphNode->AllocateDefaultPins();
			FlowGraph->AddNode(StartGraphNode, false, false);
			
			UFlowGraphNode* FinishGraphNode = NewObject<UFlowGraphNode>(FlowGraph);
			FinishGraphNode->CreateNewGuid();
			UFlowNode* FinishFlowNode = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), FinishGraphNode);
			FinishGraphNode->SetNodeTemplate(FinishFlowNode);
			FinishGraphNode->AllocateDefaultPins();
			FlowGraph->AddNode(FinishGraphNode, false, false);
			
			if (StartGraphNode->OutputPins.Num() > 0 && FinishGraphNode->InputPins.Num() > 0)
			{
				UEdGraphPin* OutputPin = StartGraphNode->OutputPins[0];
				UEdGraphPin* InputPin = FinishGraphNode->InputPins[0];
				OutputPin->MakeLinkTo(InputPin);
			}
			
			FlowGraph->NotifyGraphChanged();
			TestFlowAsset->HarvestNodeConnections();
			
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);
			
			TestTrue("Export should not be empty", !ExportedText.IsEmpty());
			TestTrue("Export should contain header", ExportedText.Contains(TEXT("FlowGraphExport v1.0")));
			TestTrue("Export should contain NODES section", ExportedText.Contains(TEXT("[NODES]")));
			TestTrue("Export should contain CONNECTIONS section", ExportedText.Contains(TEXT("[CONNECTIONS]")));
			TestTrue("Export should contain connection arrow", ExportedText.Contains(TEXT("->")));
		});

		It("should export asset metadata correctly", [this]()
		{
			TestFlowAsset->AssetGuid = FGuid::NewGuid();
			TestFlowAsset->bWorldBound = true;
			
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);
			
			TestTrue("Export should contain WorldBound", ExportedText.Contains(TEXT("WorldBound: true")));
			TestTrue("Export should contain ExpectedOwnerClass", ExportedText.Contains(TEXT("ExpectedOwnerClass:")));
		});

		It("should export node properties", [this]()
		{
			UFlowGraph* FlowGraph = Cast<UFlowGraph>(TestFlowAsset->GetGraph());
			if (!FlowGraph) { return; }
			
			UFlowGraphNode* GraphNode = NewObject<UFlowGraphNode>(FlowGraph);
			GraphNode->CreateNewGuid();
			UFlowNode* FlowNode = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), GraphNode);
			GraphNode->SetNodeTemplate(FlowNode);
			GraphNode->AllocateDefaultPins();
			FlowGraph->AddNode(GraphNode, false, false);
			FlowGraph->NotifyGraphChanged();
			
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);
			
			TestTrue("Export should contain Node section", ExportedText.Contains(TEXT("Node: ")));
			TestTrue("Export should contain Type field", ExportedText.Contains(TEXT("Type:")));
			TestTrue("Export should contain Properties section", ExportedText.Contains(TEXT("Properties:")));
			TestTrue("Export should contain InputPins section", ExportedText.Contains(TEXT("InputPins:")));
			TestTrue("Export should contain OutputPins section", ExportedText.Contains(TEXT("OutputPins:")));
		});

		It("should handle empty flow asset", [this]()
		{
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);
			
			TestTrue("Export should not be empty", !ExportedText.IsEmpty());
			TestTrue("Export should contain header", ExportedText.Contains(TEXT("FlowGraphExport v1.0")));
			TestTrue("Export should contain NODES section", ExportedText.Contains(TEXT("[NODES]")));
			TestTrue("Export should contain CONNECTIONS section", ExportedText.Contains(TEXT("[CONNECTIONS]")));
		});

		It("should handle null flow asset gracefully", [this]()
		{
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(nullptr);
			
			TestTrue("Export should be empty for null asset", ExportedText.IsEmpty());
		});

		It("should export multiple connected nodes in sequence", [this]()
		{
			UFlowGraph* FlowGraph = Cast<UFlowGraph>(TestFlowAsset->GetGraph());
			if (!FlowGraph) { return; }
			
			UFlowGraphNode* GraphNode1 = NewObject<UFlowGraphNode>(FlowGraph);
			GraphNode1->CreateNewGuid();
			UFlowNode* FlowNode1 = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), GraphNode1);
			GraphNode1->SetNodeTemplate(FlowNode1);
			GraphNode1->AllocateDefaultPins();
			FlowGraph->AddNode(GraphNode1, false, false);
			
			UFlowGraphNode* GraphNode2 = NewObject<UFlowGraphNode>(FlowGraph);
			GraphNode2->CreateNewGuid();
			UFlowNode* FlowNode2 = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), GraphNode2);
			GraphNode2->SetNodeTemplate(FlowNode2);
			GraphNode2->AllocateDefaultPins();
			FlowGraph->AddNode(GraphNode2, false, false);
			
			UFlowGraphNode* GraphNode3 = NewObject<UFlowGraphNode>(FlowGraph);
			GraphNode3->CreateNewGuid();
			UFlowNode* FlowNode3 = TestFlowAsset->CreateNode(UFlowNode_Reroute::StaticClass(), GraphNode3);
			GraphNode3->SetNodeTemplate(FlowNode3);
			GraphNode3->AllocateDefaultPins();
			FlowGraph->AddNode(GraphNode3, false, false);
			
			if (GraphNode1->OutputPins.Num() > 0 && GraphNode2->InputPins.Num() > 0)
			{
				GraphNode1->OutputPins[0]->MakeLinkTo(GraphNode2->InputPins[0]);
			}
			
			if (GraphNode2->OutputPins.Num() > 0 && GraphNode3->InputPins.Num() > 0)
			{
				GraphNode2->OutputPins[0]->MakeLinkTo(GraphNode3->InputPins[0]);
			}
			
			FlowGraph->NotifyGraphChanged();
			TestFlowAsset->HarvestNodeConnections();
			
			FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);
			
			int32 ConnectionCount = 0;
			int32 SearchPos = 0;
			while ((SearchPos = ExportedText.Find(TEXT("->"), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchPos)) != INDEX_NONE)
			{
				ConnectionCount++;
				SearchPos++;
			}
			
			TestEqual("Export should contain two connections", ConnectionCount, 2);
		});
	});
}

#endif
