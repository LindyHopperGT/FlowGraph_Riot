// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowGraphImporter.h"
#include "FlowGraphExporter.h"
#include "FlowGraphRegrapher.h"
#include "FlowAsset.h"
#include "Nodes/FlowNode.h"
#include "AddOns/FlowNodeAddOn.h"
#include "AddOns/FlowNodeAddOn_SwitchCase.h"
#include "Graph/FlowGraph.h"
#include "Graph/Nodes/FlowGraphNode.h"
#include "Misc/AutomationTest.h"
#include "Editor.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FFlowGraphImporterSpec, "FlowGraphCourier.EditorGame.Importer", EAutomationTestFlags::ProductFilter | EAutomationTestFlags::EditorContext)
	UFlowAsset* TestFlowAsset;
	FString TestImportText;
END_DEFINE_SPEC(FFlowGraphImporterSpec)

void FFlowGraphImporterSpec::Define()
{
	Describe("FlowGraphImporter", [this]()
	{
		BeforeEach([this]()
		{
			TestFlowAsset = nullptr;
			TestImportText.Empty();
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

		Describe("Basic Import", [this]()
		{
			It("should import a simple flow graph with Start and Finish nodes", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: true\n"
					"ExpectedOwnerClass: None\n"
					"---\n"
					"\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000002\n"
					"  Type: FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"00000000-0000-0000-0000-000000000001.Out -> 00000000-0000-0000-0000-000000000002.In\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportBasic"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				TestEqual("Asset should have 2 nodes", TestFlowAsset->GetNodes().Num(), 2);
				TestTrue("Asset should be world bound", TestFlowAsset->bWorldBound);

				FGuid StartGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), StartGuid);
				UFlowNode* StartNode = TestFlowAsset->GetNode(StartGuid);
				TestNotNull("Start node should exist", StartNode);

				FGuid FinishGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000002"), FinishGuid);
				UFlowNode* FinishNode = TestFlowAsset->GetNode(FinishGuid);
				TestNotNull("Finish node should exist", FinishNode);

				if (StartNode)
				{
					FConnectedPin Connection = StartNode->GetConnection(FName("Out"));
					TestTrue("Start node should have connection", Connection.NodeGuid.IsValid());
					TestEqual("Connection should point to Finish node", Connection.NodeGuid, FinishGuid);
					TestEqual("Connection should point to In pin", Connection.PinName, FName("In"));
				}
			});

			It("should handle empty import text gracefully", [this]()
			{
				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportEmpty"),
					TEXT(""),
					ErrorMessage
				);

				TestNull("Import should fail for empty text", TestFlowAsset);
				TestFalse("Error message should not be empty", ErrorMessage.IsEmpty());
			});

			It("should parse asset metadata correctly", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n"
					"\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportMetadata"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (TestFlowAsset)
				{
					TestFalse("Asset should not be world bound", TestFlowAsset->bWorldBound);
					TestTrue("Asset GUID should be valid", TestFlowAsset->AssetGuid.IsValid());
				}
			});
		});

		Describe("Property Import", [this]()
		{
			It("should import array properties with nested structs correctly", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_DefineProperties\n"
					"  Properties:\n"
					"    NamedProperties: ((Name=\"TestProp\",DataPinValue=/Script/Flow.FlowDataPinValue_Text(Values=(\"Test Value\"))))\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - TestProp [Text]\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportArrayProperties"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				FGuid DefinePropsGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), DefinePropsGuid);
				UFlowNode* DefinePropsNode = TestFlowAsset->GetNode(DefinePropsGuid);
				TestNotNull("DefineProperties node should exist", DefinePropsNode);

				if (DefinePropsNode)
				{
					// Check that NamedProperties array was imported correctly
					FProperty* NamedPropsProperty = DefinePropsNode->GetClass()->FindPropertyByName(TEXT("NamedProperties"));
					TestNotNull("NamedProperties property should exist", NamedPropsProperty);
					
					if (NamedPropsProperty)
					{
						FArrayProperty* ArrayProperty = CastField<FArrayProperty>(NamedPropsProperty);
						TestNotNull("NamedProperties should be an array property", ArrayProperty);
						
						if (ArrayProperty)
						{
							void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(DefinePropsNode);
							FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayPtr);
							
							TestEqual("NamedProperties should have 1 element", ArrayHelper.Num(), 1);
							
							if (ArrayHelper.Num() > 0)
							{
								// Verify the struct has the correct PropertyName
								FStructProperty* InnerStructProp = CastField<FStructProperty>(ArrayProperty->Inner);
								TestNotNull("Inner property should be a struct", InnerStructProp);
								
								if (InnerStructProp)
								{
									void* StructPtr = ArrayHelper.GetRawPtr(0);
									
									// Check Name field
									FProperty* NameFieldProp = InnerStructProp->Struct->FindPropertyByName(TEXT("Name"));
									TestNotNull("Name field should exist", NameFieldProp);
									
									if (NameFieldProp)
									{
										FNameProperty* NameProp = CastField<FNameProperty>(NameFieldProp);
										TestNotNull("Name should be a Name property", NameProp);
										
										if (NameProp)
										{
											FName NameValue = NameProp->GetPropertyValue_InContainer(StructPtr);
											TestEqual("Name should be 'TestProp'", NameValue.ToString(), FString(TEXT("TestProp")));
											TestFalse("Name should not be empty", NameValue.IsNone());
										}
									}
									
									// Check DataPinValue field
									FProperty* DataPinValueProp = InnerStructProp->Struct->FindPropertyByName(TEXT("DataPinValue"));
									TestNotNull("DataPinValue field should exist", DataPinValueProp);
									
									if (DataPinValueProp)
									{
										FStructProperty* DataPinValueStructProp = CastField<FStructProperty>(DataPinValueProp);
										TestNotNull("DataPinValue should be a struct property", DataPinValueStructProp);
										
										if (DataPinValueStructProp)
										{
											// Get the TInstancedStruct value
											void* DataPinValuePtr = DataPinValueStructProp->ContainerPtrToValuePtr<void>(StructPtr);
											
											// Export to text to see what we got
											FString DataPinValueExport;
											DataPinValueStructProp->ExportText_Direct(DataPinValueExport, DataPinValuePtr, nullptr, nullptr, PPF_None);
											UE_LOG(LogFlow, Log, TEXT("DataPinValue exported as: %s"), *DataPinValueExport);
											
											// Check if it's not None
											TestNotEqual("DataPinValue should not be None", DataPinValueExport, FString(TEXT("None")));
										}
									}
									
									// Log the struct for debugging
									FString StructDebugString;
									InnerStructProp->Struct->ExportText(StructDebugString, StructPtr, nullptr, nullptr, PPF_None, nullptr);
									UE_LOG(LogFlow, Log, TEXT("NamedProperties[0] exported as: %s"), *StructDebugString);
								}
							}
						}
					}
				}
			});

			It("should import float properties correctly", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Timer\n"
					"  Properties:\n"
					"    CompletionTime: 5.0\n"
					"    StepTime: 1.0\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"    - Completed [Exec]\n"
					"    - Step [Exec]\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportProperties"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					return;
				}

				FGuid TimerGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), TimerGuid);
				UFlowNode* TimerNode = TestFlowAsset->GetNode(TimerGuid);
				TestNotNull("Timer node should exist", TimerNode);

				if (TimerNode)
				{
					FProperty* CompletionTimeProperty = TimerNode->GetClass()->FindPropertyByName(TEXT("CompletionTime"));
					if (CompletionTimeProperty)
					{
						FFloatProperty* FloatProperty = CastField<FFloatProperty>(CompletionTimeProperty);
						if (FloatProperty)
						{
							float Value = FloatProperty->GetPropertyValue_InContainer(TimerNode);
							TestEqual("CompletionTime should be 5.0", Value, 5.0f);
						}
					}

					FProperty* StepTimeProperty = TimerNode->GetClass()->FindPropertyByName(TEXT("StepTime"));
					if (StepTimeProperty)
					{
						FFloatProperty* FloatProperty = CastField<FFloatProperty>(StepTimeProperty);
						if (FloatProperty)
						{
							float Value = FloatProperty->GetPropertyValue_InContainer(TimerNode);
							TestEqual("StepTime should be 1.0", Value, 1.0f);
						}
					}
				}
			});

			It("should import FlowDataPinValue_Text struct properties correctly", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /LbExpeditions/Flow/FN_UpdatePhaseInstructions.FN_UpdatePhaseInstructions_C\n"
					"  Properties:\n"
					"    PhaseInstructions: (Values=(\"The Avarosans need your aid! Seek out the ancient Frostguard watchtower.\"))\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportTextProperties"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					return;
				}

				FGuid NodeGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), NodeGuid);
				UFlowNode* Node = TestFlowAsset->GetNode(NodeGuid);
				TestNotNull("Node should exist", Node);

				if (Node)
				{
					FProperty* PhaseInstructionsProperty = Node->GetClass()->FindPropertyByName(TEXT("PhaseInstructions"));
					if (!PhaseInstructionsProperty)
					{
						PhaseInstructionsProperty = FindFProperty<FProperty>(Node->GetClass(), TEXT("PhaseInstructions"));
					}
					
					if (PhaseInstructionsProperty)
					{
						FStructProperty* StructProperty = CastField<FStructProperty>(PhaseInstructionsProperty);
						if (StructProperty && StructProperty->Struct->GetName() == TEXT("FlowDataPinValue_Text"))
						{
							// Access the Values array within the struct
							void* StructPtr = StructProperty->ContainerPtrToValuePtr<void>(Node);
							FProperty* ValuesProperty = StructProperty->Struct->FindPropertyByName(TEXT("Values"));
							if (ValuesProperty)
							{
								FArrayProperty* ArrayProperty = CastField<FArrayProperty>(ValuesProperty);
								if (ArrayProperty)
								{
									void* ArrayPtr = ValuesProperty->ContainerPtrToValuePtr<void>(StructPtr);
									FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayPtr);
									
									TestTrue("Values array should have at least one element", ArrayHelper.Num() > 0);
									if (ArrayHelper.Num() > 0)
									{
										FTextProperty* TextProperty = CastField<FTextProperty>(ArrayProperty->Inner);
										if (TextProperty)
										{
											FText Value = TextProperty->GetPropertyValue(ArrayHelper.GetRawPtr(0));
											FString ValueString = Value.ToString();
											TestEqual("PhaseInstructions should match", ValueString, FString(TEXT("The Avarosans need your aid! Seek out the ancient Frostguard watchtower.")));
										}
									}
								}
							}
						}
						else
						{
							AddError(FString::Printf(TEXT("PhaseInstructions property is not a FlowDataPinValue_Text struct, it's: %s"), 
								StructProperty ? *StructProperty->Struct->GetName() : TEXT("not a struct")));
						}
					}
					else
					{
						AddError(TEXT("PhaseInstructions property not found on node"));
					}
				}
			});
		});

		Describe("Export-Import Roundtrip", [this]()
		{
			It("should successfully roundtrip a test FlowAsset", [this]()
			{
				UFlowAsset* OriginalAsset = LoadObject<UFlowAsset>(nullptr, TEXT("/LbExpeditions/EncounterDefinitions/Tests/FA_ZTest_FlowGraph.FA_ZTest_FlowGraph"));
				if (!OriginalAsset)
				{
					AddWarning(TEXT("Could not load test asset - skipping roundtrip test"));
					return;
				}

				FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(OriginalAsset);
				TestFalse("Exported text should not be empty", ExportedText.IsEmpty());

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportRoundtrip"),
					ExportedText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				TestEqual("Node count should match", TestFlowAsset->GetNodes().Num(), OriginalAsset->GetNodes().Num());

				for (const TPair<FGuid, UFlowNode*>& OriginalNodePair : OriginalAsset->GetNodes())
				{
					UFlowNode* OriginalNode = OriginalNodePair.Value;
					UFlowNode* ImportedNode = TestFlowAsset->GetNode(OriginalNodePair.Key);

					TestNotNull(FString::Printf(TEXT("Imported node %s should exist"), *OriginalNodePair.Key.ToString()), ImportedNode);
					if (ImportedNode)
					{
						TestEqual(FString::Printf(TEXT("Node %s type should match"), *OriginalNodePair.Key.ToString()),
							ImportedNode->GetClass(), OriginalNode->GetClass());
					}
				}

				// Verify connections match by checking each original connection exists in imported asset
				for (const TPair<FGuid, UFlowNode*>& OriginalNodePair : OriginalAsset->GetNodes())
				{
					UFlowNode* OriginalNode = OriginalNodePair.Value;
					UFlowNode* ImportedNode = TestFlowAsset->GetNode(OriginalNodePair.Key);

					if (!ImportedNode)
					{
						continue;
					}

					// Check all output connections (exec and data)
					for (const FFlowPin& OutputPin : OriginalNode->GetOutputPins())
					{
						FConnectedPin OriginalConnection = OriginalNode->GetConnection(OutputPin.PinName);
						if (OriginalConnection.NodeGuid.IsValid())
						{
							FConnectedPin ImportedConnection = ImportedNode->GetConnection(OutputPin.PinName);
							TestTrue(FString::Printf(TEXT("Node %s output pin %s should have connection"), 
								*OriginalNodePair.Key.ToString(), *OutputPin.PinName.ToString()),
								ImportedConnection.NodeGuid.IsValid());

							if (ImportedConnection.NodeGuid.IsValid())
							{
								TestEqual(FString::Printf(TEXT("Node %s output pin %s connection target should match"),
									*OriginalNodePair.Key.ToString(), *OutputPin.PinName.ToString()),
									ImportedConnection.NodeGuid, OriginalConnection.NodeGuid);

								TestEqual(FString::Printf(TEXT("Node %s output pin %s connection pin should match"),
									*OriginalNodePair.Key.ToString(), *OutputPin.PinName.ToString()),
									ImportedConnection.PinName, OriginalConnection.PinName);
							}
						}
					}

					// Check all data input connections
					for (const FFlowPin& InputPin : OriginalNode->GetInputPins())
					{
						if (!InputPin.IsExecPin())
						{
							FConnectedPin OriginalConnection = OriginalNode->GetConnection(InputPin.PinName);
							if (OriginalConnection.NodeGuid.IsValid())
							{
								FConnectedPin ImportedConnection = ImportedNode->GetConnection(InputPin.PinName);
								TestTrue(FString::Printf(TEXT("Node %s input pin %s should have connection"), 
									*OriginalNodePair.Key.ToString(), *InputPin.PinName.ToString()),
									ImportedConnection.NodeGuid.IsValid());

								if (ImportedConnection.NodeGuid.IsValid())
								{
									TestEqual(FString::Printf(TEXT("Node %s input pin %s connection source should match"),
										*OriginalNodePair.Key.ToString(), *InputPin.PinName.ToString()),
										ImportedConnection.NodeGuid, OriginalConnection.NodeGuid);

									TestEqual(FString::Printf(TEXT("Node %s input pin %s connection pin should match"),
										*OriginalNodePair.Key.ToString(), *InputPin.PinName.ToString()),
										ImportedConnection.PinName, OriginalConnection.PinName);
								}
							}
						}
					}
				}
			});

			It("should create editor graph nodes after import using Regrapher", [this]()
			{
				// Log the editor state for debugging
				UE_LOG(LogFlow, Log, TEXT("Regrapher test: GEditor=%p, IsRunningCommandlet=%d, IsUnattended=%d"),
					(void*)GEditor, IsRunningCommandlet(), FApp::IsUnattended());
				
				if (GEditor)
				{
					UE_LOG(LogFlow, Log, TEXT("Regrapher test: GEditor->PlayWorld=%p"), (void*)GEditor->PlayWorld.Get());
				}

				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: true\n"
					"ExpectedOwnerClass: None\n"
					"---\n"
					"\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000002\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"00000000-0000-0000-0000-000000000001.Out -> 00000000-0000-0000-0000-000000000002.In\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestImportWithRegrapher"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					return;
				}

				bool bRegraphSuccess = UFlowGraphRegrapher::RegraphFlowAsset(TestFlowAsset);
				TestTrue("Regrapher should succeed", bRegraphSuccess);

				UFlowGraph* FlowGraph = Cast<UFlowGraph>(TestFlowAsset->GetGraph());
				TestNotNull("FlowGraph should exist after regraphing", FlowGraph);

				if (FlowGraph)
				{
					FGuid StartGuid;
					FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), StartGuid);
					UFlowNode* StartNode = TestFlowAsset->GetNode(StartGuid);
					
					if (StartNode)
					{
						UEdGraphNode* GraphNode = StartNode->GetGraphNode();
						TestNotNull("Start node should have a graph node after regraphing", GraphNode);
					}
				}
			});

			It("should import and regraph in one call using ImportAndRegraphFromText", [this]()
			{
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: true\n"
					"ExpectedOwnerClass: None\n"
					"---\n"
					"\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000002\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"00000000-0000-0000-0000-000000000001.Out -> 00000000-0000-0000-0000-000000000002.In\n"
					"---\n"
				);

				UFlowAsset* ImportedAsset = UFlowGraphRegrapher::ImportAndRegraphFromText(
					TestImportText,
					TEXT("/Game/Test/TestImportAndRegraph")
				);

				TestNotNull("Imported and regraphed asset should not be null", ImportedAsset);
				
				if (ImportedAsset)
				{
					// Verify the asset has a graph
					UFlowGraph* FlowGraph = Cast<UFlowGraph>(ImportedAsset->GetGraph());
					TestNotNull("FlowGraph should exist", FlowGraph);

					// Verify nodes have graph nodes
					FGuid StartGuid;
					FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), StartGuid);
					UFlowNode* StartNode = ImportedAsset->GetNode(StartGuid);
					
					if (StartNode)
					{
						UEdGraphNode* GraphNode = StartNode->GetGraphNode();
						TestNotNull("Start node should have a graph node", GraphNode);
						
						if (GraphNode)
						{
							// Verify the graph node has pins
							TestTrue("Graph node should have pins", GraphNode->Pins.Num() > 0);
						}
					}

					FGuid FinishGuid;
					FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000002"), FinishGuid);
					UFlowNode* FinishNode = ImportedAsset->GetNode(FinishGuid);
					
					if (FinishNode)
					{
						UEdGraphNode* GraphNode = FinishNode->GetGraphNode();
						TestNotNull("Finish node should have a graph node", GraphNode);
					}
				}
			});

			It("should properly connect data pins in EdGraphNodes after import and regraph", [this]()
			{
				// Create a flow with data pin connections: FN_Spawn.Location -> FN_UpdateMapMarker.MapMarkerLocation
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: true\n"
					"ExpectedOwnerClass: None\n"
					"---\n"
					"\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000002\n"
					"  Type: /LbExpeditions/Flow/FN_Spawn.FN_Spawn_C\n"
					"  Properties:\n"
					"    ActorType: /LbExpeditions/Flow/EA_Flow_GoalLocation.EA_Flow_GoalLocation_C\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"    - OnSpawned [Exec]\n"
					"    - ActorID [InstancedStruct]\n"
					"    - Location [Vector]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000003\n"
					"  Type: /LbExpeditions/Flow/FN_UpdateMapMarker.FN_UpdateMapMarker_C\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"    - MapMarkerLocation [Vector]\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"\n"
					"Node: 00000000-0000-0000-0000-000000000004\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"\n"
					"---\n"
					"\n"
					"[CONNECTIONS]\n"
					"; Exec connections\n"
					"00000000-0000-0000-0000-000000000001.Out -> 00000000-0000-0000-0000-000000000002.In\n"
					"00000000-0000-0000-0000-000000000002.OnSpawned -> 00000000-0000-0000-0000-000000000003.In\n"
					"00000000-0000-0000-0000-000000000003.Out -> 00000000-0000-0000-0000-000000000004.In\n"
					"; Data connection - this is what we're testing\n"
					"00000000-0000-0000-0000-000000000002.Location -> 00000000-0000-0000-0000-000000000003.MapMarkerLocation\n"
					"---\n"
				);

				UFlowAsset* ImportedAsset = UFlowGraphRegrapher::ImportAndRegraphFromText(
					TestImportText,
					TEXT("/Game/Test/TestDataPinImportAndRegraph")
				);

				TestNotNull("Imported and regraphed asset should not be null", ImportedAsset);
				
				if (ImportedAsset)
				{
					// Verify the asset has a graph
					UFlowGraph* FlowGraph = Cast<UFlowGraph>(ImportedAsset->GetGraph());
					TestNotNull("FlowGraph should exist", FlowGraph);

					// Get the nodes
					FGuid SpawnGuid;
					FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000002"), SpawnGuid);
					UFlowNode* SpawnNode = ImportedAsset->GetNode(SpawnGuid);
					
					FGuid MapMarkerGuid;
					FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000003"), MapMarkerGuid);
					UFlowNode* MapMarkerNode = ImportedAsset->GetNode(MapMarkerGuid);

					TestNotNull("FN_Spawn node should exist", SpawnNode);
					TestNotNull("FN_UpdateMapMarker node should exist", MapMarkerNode);

					if (SpawnNode && MapMarkerNode)
					{
						// Get the EdGraphNodes
						UEdGraphNode* SpawnGraphNode = SpawnNode->GetGraphNode();
						UEdGraphNode* MapMarkerGraphNode = MapMarkerNode->GetGraphNode();

						TestNotNull("FN_Spawn should have a graph node", SpawnGraphNode);
						TestNotNull("FN_UpdateMapMarker should have a graph node", MapMarkerGraphNode);

						if (SpawnGraphNode && MapMarkerGraphNode)
						{
							// Find the Location output pin on FN_Spawn
							UEdGraphPin* LocationOutputPin = nullptr;
							for (UEdGraphPin* Pin : SpawnGraphNode->Pins)
							{
								if (Pin && Pin->PinName == TEXT("Location") && Pin->Direction == EGPD_Output)
								{
									LocationOutputPin = Pin;
									break;
								}
							}

							// Find the MapMarkerLocation input pin on FN_UpdateMapMarker
							UEdGraphPin* MapMarkerLocationInputPin = nullptr;
							for (UEdGraphPin* Pin : MapMarkerGraphNode->Pins)
							{
								if (Pin && Pin->PinName == TEXT("MapMarkerLocation") && Pin->Direction == EGPD_Input)
								{
									MapMarkerLocationInputPin = Pin;
									break;
								}
							}

							TestNotNull("FN_Spawn should have Location output pin", LocationOutputPin);
							TestNotNull("FN_UpdateMapMarker should have MapMarkerLocation input pin", MapMarkerLocationInputPin);

							if (LocationOutputPin && MapMarkerLocationInputPin)
							{
								// Verify the data pin connection exists in the EdGraph
								bool bFoundConnection = false;
								for (UEdGraphPin* LinkedPin : LocationOutputPin->LinkedTo)
								{
									if (LinkedPin == MapMarkerLocationInputPin)
									{
										bFoundConnection = true;
										break;
									}
								}

								TestTrue("Location output pin should be linked to MapMarkerLocation input pin", bFoundConnection);

								// Also verify the reverse connection
								bool bFoundReverseConnection = false;
								for (UEdGraphPin* LinkedPin : MapMarkerLocationInputPin->LinkedTo)
								{
									if (LinkedPin == LocationOutputPin)
									{
										bFoundReverseConnection = true;
										break;
									}
								}

								TestTrue("MapMarkerLocation input pin should be linked to Location output pin", bFoundReverseConnection);
							}
						}
					}
				}

			});
		});
		Describe("AddOn Import Export", [this]()
		{
			// Helpers ----------------------------------------------------------
			// Build a minimal single-node flow graph text with one SwitchCase addon.
			// UFlowNodeAddOn_SwitchCase is a concrete, native, non-abstract addon in
			// /Script/Flow - no Blueprint asset or asset registry required.
			auto MakeSwitchCaseAddonText = [](const FString& CaseName) -> FString
			{
				return FString::Printf(TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"  AddOn: /Script/Flow.FlowNodeAddOn_SwitchCase\n"
					"    CaseName: %s\n"
					"\n"
					"---\n\n"
					"[CONNECTIONS]\n"
					"---\n"
				), *CaseName);
			};

			It("should import a node with a single AddOn attached", [this, MakeSwitchCaseAddonText]()
			{
				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnImportSingle"),
					MakeSwitchCaseAddonText(TEXT("CaseA")),
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				FGuid NodeGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), NodeGuid);
				UFlowNode* StartNode = TestFlowAsset->GetNode(NodeGuid);
				TestNotNull("Start node should exist", StartNode);
				if (!StartNode)
				{
					return;
				}

				const TArray<UFlowNodeAddOn*>& AddOns = StartNode->GetFlowNodeAddOnChildren();
				TestEqual("Node should have exactly one AddOn", AddOns.Num(), 1);
				if (AddOns.Num() > 0)
				{
					TestNotNull("AddOn should not be null", AddOns[0]);
					if (AddOns[0])
					{
						TestTrue("AddOn should be a UFlowNodeAddOn_SwitchCase",
							AddOns[0]->IsA(UFlowNodeAddOn_SwitchCase::StaticClass()));
					}
				}
			});

			It("should preserve AddOn property value through import", [this, MakeSwitchCaseAddonText]()
			{
				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnImportProperty"),
					MakeSwitchCaseAddonText(TEXT("MyCaseName")),
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				FGuid NodeGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), NodeGuid);
				UFlowNode* StartNode = TestFlowAsset->GetNode(NodeGuid);
				if (!StartNode)
				{
					return;
				}

				const TArray<UFlowNodeAddOn*>& AddOns = StartNode->GetFlowNodeAddOnChildren();
				if (AddOns.Num() == 0 || !AddOns[0])
				{
					return;
				}

				UFlowNodeAddOn_SwitchCase* SwitchCase = Cast<UFlowNodeAddOn_SwitchCase>(AddOns[0]);
				TestNotNull("AddOn should cast to UFlowNodeAddOn_SwitchCase", SwitchCase);
				if (SwitchCase)
				{
					TestEqual("CaseName should be preserved", SwitchCase->CaseName, FName(TEXT("MyCaseName")));
				}
			});

			It("should export a node with an AddOn as an AddOn: block", [this]()
			{
				// Build the asset via importer so we have a known addon attached.
				const FString ImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"  AddOn: /Script/Flow.FlowNodeAddOn_SwitchCase\n"
					"    CaseName: ExportedCase\n"
					"\n"
					"---\n\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnExport"),
					ImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				const FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(TestFlowAsset);

				TestFalse("Exported text should not be empty", ExportedText.IsEmpty());
				TestTrue("Exported text should contain AddOn: line",
					ExportedText.Contains(TEXT("AddOn:")));
				TestTrue("Exported text should contain the SwitchCase class path",
					ExportedText.Contains(TEXT("FlowNodeAddOn_SwitchCase")));
			});

			It("should roundtrip a node with a single AddOn preserving type and property", [this, MakeSwitchCaseAddonText]()
			{
				FString ErrorMessage;
				UFlowAsset* OriginalAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnRoundtripOrig"),
					MakeSwitchCaseAddonText(TEXT("RoundtripCase")),
					ErrorMessage
				);

				TestNotNull("Original import should succeed", OriginalAsset);
				if (!OriginalAsset)
				{
					AddError(FString::Printf(TEXT("First import failed: %s"), *ErrorMessage));
					return;
				}

				const FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(OriginalAsset);
				TestFalse("Exported text should not be empty", ExportedText.IsEmpty());

				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnRoundtripReimport"),
					ExportedText,
					ErrorMessage
				);

				// Clean up original (not tracked by TestFlowAsset)
				OriginalAsset->ClearFlags(RF_Standalone);
				OriginalAsset->MarkAsGarbage();

				TestNotNull("Re-imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Re-import failed: %s"), *ErrorMessage));
					return;
				}

				FGuid NodeGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), NodeGuid);
				UFlowNode* StartNode = TestFlowAsset->GetNode(NodeGuid);
				TestNotNull("Start node should survive roundtrip", StartNode);
				if (!StartNode)
				{
					return;
				}

				const TArray<UFlowNodeAddOn*>& AddOns = StartNode->GetFlowNodeAddOnChildren();
				TestEqual("Addon count should be 1 after roundtrip", AddOns.Num(), 1);
				if (AddOns.Num() > 0 && AddOns[0])
				{
					UFlowNodeAddOn_SwitchCase* SwitchCase = Cast<UFlowNodeAddOn_SwitchCase>(AddOns[0]);
					TestNotNull("Re-imported AddOn should cast to UFlowNodeAddOn_SwitchCase", SwitchCase);
					if (SwitchCase)
					{
						TestEqual("CaseName should survive roundtrip", SwitchCase->CaseName, FName(TEXT("RoundtripCase")));
					}
				}
			});

			It("should roundtrip multiple AddOns on the same node preserving order", [this]()
			{
				const FString ImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"  AddOn: /Script/Flow.FlowNodeAddOn_SwitchCase\n"
					"    CaseName: Alpha\n"
					"  AddOn: /Script/Flow.FlowNodeAddOn_SwitchCase\n"
					"    CaseName: Beta\n"
					"\n"
					"---\n\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				UFlowAsset* OriginalAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestMultiAddOnOrig"),
					ImportText,
					ErrorMessage
				);

				TestNotNull("Original import should succeed", OriginalAsset);
				if (!OriginalAsset)
				{
					AddError(FString::Printf(TEXT("First import failed: %s"), *ErrorMessage));
					return;
				}

				// Verify two addons were attached before roundtrip
				FGuid NodeGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000001"), NodeGuid);
				UFlowNode* StartNode = OriginalAsset->GetNode(NodeGuid);
				TestNotNull("Start node should exist after first import", StartNode);

				if (StartNode)
				{
					TestEqual("First import should have 2 AddOns", StartNode->GetFlowNodeAddOnChildren().Num(), 2);
				}

				const FString ExportedText = UFlowGraphExporter::ExportFlowGraphToString(OriginalAsset);

				OriginalAsset->ClearFlags(RF_Standalone);
				OriginalAsset->MarkAsGarbage();

				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestMultiAddOnReimport"),
					ExportedText,
					ErrorMessage
				);

				TestNotNull("Re-imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Re-import failed: %s"), *ErrorMessage));
					return;
				}

				StartNode = TestFlowAsset->GetNode(NodeGuid);
				TestNotNull("Start node should survive roundtrip", StartNode);
				if (!StartNode)
				{
					return;
				}

				const TArray<UFlowNodeAddOn*>& AddOns = StartNode->GetFlowNodeAddOnChildren();
				TestEqual("Both AddOns should survive roundtrip", AddOns.Num(), 2);

				if (AddOns.Num() >= 2)
				{
					UFlowNodeAddOn_SwitchCase* Case0 = Cast<UFlowNodeAddOn_SwitchCase>(AddOns[0]);
					UFlowNodeAddOn_SwitchCase* Case1 = Cast<UFlowNodeAddOn_SwitchCase>(AddOns[1]);

					TestNotNull("First re-imported AddOn should cast to SwitchCase", Case0);
					TestNotNull("Second re-imported AddOn should cast to SwitchCase", Case1);

					if (Case0)
					{
						TestEqual("First AddOn CaseName should be Alpha", Case0->CaseName, FName(TEXT("Alpha")));
					}
					if (Case1)
					{
						TestEqual("Second AddOn CaseName should be Beta", Case1->CaseName, FName(TEXT("Beta")));
					}
				}
			});

			It("should fail import gracefully when AddOn class path is unknown", [this]()
			{
				const FString ImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: false\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n"
					"  AddOn: /Script/Flow.FlowNodeAddOn_DoesNotExist\n"
					"\n"
					"---\n\n"
					"[CONNECTIONS]\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestAddOnUnknownClass"),
					ImportText,
					ErrorMessage
				);

				TestNull("Import should fail when AddOn class is unknown", TestFlowAsset);
				TestFalse("Error message should not be empty on unknown AddOn class", ErrorMessage.IsEmpty());
			});
		});

		Describe("Dynamic Pin Resolution", [this]()
		{
			It("should wire declared [Exec] output pins beyond CDO defaults as exec connections", [this]()
			{
				// Regression: ExecutionSequence pin 2+ was absent from GetOutputPins() on the CDO
				// (only 0 and 1 exist by default) and fell back to wildcard/data, causing the
				// connection to be stored on the wrong node and silently dropped on export.
				TestImportText = TEXT(
					"FlowGraphExport v1.0\n"
					"WorldBound: true\n"
					"ExpectedOwnerClass: None\n"
					"---\n\n"
					"[NODES]\n"
					"Node: 00000000-0000-0000-0000-000000000001\n"
					"  Type: /Script/Flow.FlowNode_Start\n"
					"  Properties:\n"
					"  InputPins:\n"
					"  OutputPins:\n"
					"    - Out [Exec]\n\n"
					"Node: 00000000-0000-0000-0000-000000000002\n"
					"  Type: /Script/Flow.FlowNode_ExecutionSequence\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n"
					"    - 0 [Exec]\n"
					"    - 1 [Exec]\n"
					"    - 2 [Exec]\n\n"
					"Node: 00000000-0000-0000-0000-000000000003\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n\n"
					"Node: 00000000-0000-0000-0000-000000000004\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n\n"
					"Node: 00000000-0000-0000-0000-000000000005\n"
					"  Type: /Script/Flow.FlowNode_Finish\n"
					"  Properties:\n"
					"  InputPins:\n"
					"    - In [Exec]\n"
					"  OutputPins:\n\n"
					"---\n\n"
					"[CONNECTIONS]\n"
					"00000000-0000-0000-0000-000000000001.Out -> 00000000-0000-0000-0000-000000000002.In\n"
					"00000000-0000-0000-0000-000000000002.0 -> 00000000-0000-0000-0000-000000000003.In\n"
					"00000000-0000-0000-0000-000000000002.1 -> 00000000-0000-0000-0000-000000000004.In\n"
					"00000000-0000-0000-0000-000000000002.2 -> 00000000-0000-0000-0000-000000000005.In\n"
					"---\n"
				);

				FString ErrorMessage;
				TestFlowAsset = UFlowGraphImporter::ImportFlowGraphFromText(
					TEXT("/Game/Test/TestDynamicExecPin"),
					TestImportText,
					ErrorMessage
				);

				TestNotNull("Imported asset should not be null", TestFlowAsset);
				if (!TestFlowAsset)
				{
					AddError(FString::Printf(TEXT("Import failed: %s"), *ErrorMessage));
					return;
				}

				FGuid SeqGuid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000002"), SeqGuid);
				UFlowNode* SeqNode = TestFlowAsset->GetNode(SeqGuid);
				TestNotNull("ExecutionSequence node should exist", SeqNode);
				if (!SeqNode)
				{
					return;
				}

				FGuid Finish3Guid;
				FGuid::Parse(TEXT("00000000-0000-0000-0000-000000000005"), Finish3Guid);

				// Pin 2 connection must be stored on the source (SeqNode), not the target,
				// which is only the case when the importer correctly treats it as an exec pin.
				FConnectedPin Pin2Connection = SeqNode->GetConnection(FName("2"));
				TestTrue("ExecutionSequence pin 2 should have a valid connection", Pin2Connection.NodeGuid.IsValid());
				if (Pin2Connection.NodeGuid.IsValid())
				{
					TestEqual("ExecutionSequence pin 2 should connect to Finish node 5", Pin2Connection.NodeGuid, Finish3Guid);
					TestEqual("ExecutionSequence pin 2 connection target pin should be 'In'", Pin2Connection.PinName, FName("In"));
				}
			});
		});
	});
}

#endif
