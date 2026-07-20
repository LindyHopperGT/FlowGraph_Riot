// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphGlossaryExporter.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FFlowGraphGlossaryExporterSpec, "FlowGraphCourier.EditorGame.GlossaryExporter", EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)
END_DEFINE_SPEC(FFlowGraphGlossaryExporterSpec)

void FFlowGraphGlossaryExporterSpec::Define()
{
	Describe("FlowGraphGlossaryExporter", [this]()
	{
		It("should export the nodes we know about", [this]()
		{
			FString GlossaryText = UFlowGraphGlossaryExporter::ExportGlossaryToString();
			
			TestTrue("Glossary should not be empty", !GlossaryText.IsEmpty());
			TestTrue("Glossary should contain header", GlossaryText.Contains(TEXT("# FlowGraph Node Glossary")));
			TestTrue("Glossary should contain total nodes count", GlossaryText.Contains(TEXT("**Total Nodes:**")));
			
			TestTrue("Glossary should contain Finish node", GlossaryText.Contains(TEXT("## Finish")));
			TestTrue("Glossary should contain FlowNode_Finish class", GlossaryText.Contains(TEXT("FlowNode_Finish")));
			
			TestTrue("Glossary should contain Start node", GlossaryText.Contains(TEXT("## Start")));
			TestTrue("Glossary should contain FlowNode_Start class", GlossaryText.Contains(TEXT("FlowNode_Start")));
			
			TestTrue("Glossary should contain ExecutionSequence node", GlossaryText.Contains(TEXT("## ExecutionSequence")));
			TestTrue("Glossary should contain FlowNode_ExecutionSequence class", GlossaryText.Contains(TEXT("FlowNode_ExecutionSequence")));
			TestTrue("ExecutionSequence should indicate dynamic output pins", GlossaryText.Contains(TEXT("Supports user-added output pins")));
			
			TestTrue("Glossary should contain Reroute node", GlossaryText.Contains(TEXT("## Reroute")));
			TestTrue("Glossary should contain FlowNode_Reroute class", GlossaryText.Contains(TEXT("FlowNode_Reroute")));
			
			TestTrue("Glossary should contain Properties header", GlossaryText.Contains(TEXT("### Properties")));
			
			TestTrue("Glossary should contain Input Pins header", GlossaryText.Contains(TEXT("### Input Pins")));
			TestTrue("Glossary should contain Output Pins header", GlossaryText.Contains(TEXT("### Output Pins")));
			TestTrue("Glossary should contain pin table header", GlossaryText.Contains(TEXT("| Pin Name | Type |")));
			
			TestTrue("Glossary should contain Timer node", GlossaryText.Contains(TEXT("## Timer")));
			TestTrue("Glossary should contain FlowNode_Timer class", GlossaryText.Contains(TEXT("FlowNode_Timer")));
			
			TestTrue("Glossary should contain LogicalAND node", GlossaryText.Contains(TEXT("## LogicalAND")));
			TestTrue("Glossary should contain FlowNode_LogicalAND class", GlossaryText.Contains(TEXT("FlowNode_LogicalAND")));
			TestTrue("LogicalAND should indicate dynamic input pins", GlossaryText.Contains(TEXT("Supports user-added input pins")));
		});

		It("should contain a keyword index section in the exported glossary", [this]()
		{
			FString GlossaryText = UFlowGraphGlossaryExporter::ExportGlossaryToString();

			TestTrue("Glossary should contain Keyword Index header", GlossaryText.Contains(TEXT("## Keyword Index")));
		});

		It("GatherAllKeywords should produce a non-empty map when keywords are present", [this]()
		{
			TArray<UClass*> NodeClasses;
			UFlowGraphGlossaryExporter::GatherAllNodeClasses(NodeClasses);

			TArray<UClass*> AddOnClasses;
			UFlowGraphGlossaryExporter::GatherAllAddOnClasses(AddOnClasses);

			TMap<FString, TArray<FString>> KeywordMap;
			UFlowGraphGlossaryExporter::GatherAllKeywords(NodeClasses, AddOnClasses, KeywordMap);

			// At least the FlowGraph upstream nodes decorated in Pass 6 contribute keywords
			TestTrue("Keyword map should not be empty", KeywordMap.Num() > 0);

			// All keywords must be lower-case (normalization contract)
			for (const auto& Pair : KeywordMap)
			{
				TestEqual(FString::Printf(TEXT("Keyword '%s' should be lower-case"), *Pair.Key),
					Pair.Key, Pair.Key.ToLower());
			}

			// Every entry must have at least one display name
			for (const auto& Pair : KeywordMap)
			{
				TestTrue(FString::Printf(TEXT("Keyword '%s' should have at least one node name"), *Pair.Key),
					Pair.Value.Num() > 0);
			}
		});
	});
}

#endif
