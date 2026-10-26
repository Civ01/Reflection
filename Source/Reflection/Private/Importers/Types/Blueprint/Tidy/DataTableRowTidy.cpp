/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/GraphTidy.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/DataTable.h"
#include "K2Node_CallFunction.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/DataTableFunctionLibrary.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionTidyDataTable, All, All);

/* A row looked up in a data table and branched on whether it was there.
 *
 * Get Data Table Row is one node: a table, a row name, the row it found, and Row Found or Row Not
 * Found to run on from. It compiles to a call that hands the row out through a pin that takes any
 * struct, and a branch on what the call answered. Read back as those two nodes, the row pin has
 * nothing telling it what it is unless something reads it, and a row nobody read leaves the
 * compiler stopping on a type it cannot work out. The node knows the type from the table it was
 * given, so put back it compiles whether the row is read or not. */
struct FDataTableRowTidy final : FGraphTidy {
	virtual const TCHAR* GetName() const override { return TEXT("Data Table Row"); }

	virtual int32 Apply(UEdGraph* Graph) const override {
		if (Graph == nullptr) return 0;

		int32 PutBack = 0;

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();

		/* Found by name: the node's functions are exported from its module, but the class itself
		 * is not, so asking it for its class directly does not link */
		UClass* LookupClass = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetDataTableRow"));

		if (LookupClass == nullptr) return 0;

		/* Over a copy, since the graph is being taken apart as it is read */
		TArray<UEdGraphNode*> Nodes = Graph->Nodes;

		for (UEdGraphNode* Node : Nodes) {
			UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);

			if (Call == nullptr || Call->FunctionReference.GetMemberName() != TEXT("GetDataTableRowFromName")) continue;
			if (Call->FunctionReference.GetMemberParentClass() != UDataTableFunctionLibrary::StaticClass()) continue;

			UEdGraphPin* Table = Call->FindPin(TEXT("Table"), EGPD_Input);
			UEdGraphPin* RowName = Call->FindPin(TEXT("RowName"), EGPD_Input);
			UEdGraphPin* OutRow = Call->FindPin(TEXT("OutRow"), EGPD_Output);
			UEdGraphPin* Answer = Call->GetReturnValuePin();
			UEdGraphPin* Execute = Call->GetExecPin();
			UEdGraphPin* Then = Call->GetThenPin();

			if (Table == nullptr || RowName == nullptr || OutRow == nullptr || Answer == nullptr || Execute == nullptr || Then == nullptr) continue;

			/* Answered to one branch and nothing else, and run straight into that branch */
			if (Answer->LinkedTo.Num() != 1 || Then->LinkedTo.Num() != 1) continue;

			UK2Node_IfThenElse* Branch = Cast<UK2Node_IfThenElse>(Answer->LinkedTo[0]->GetOwningNode());

			if (Branch == nullptr || Answer->LinkedTo[0] != Branch->GetConditionPin()) continue;
			if (Then->LinkedTo[0] != Branch->GetExecPin()) continue;

			/* Only entered from the call, or something else reaching the branch would lose it */
			if (Branch->GetExecPin() == nullptr || Branch->GetExecPin()->LinkedTo.Num() != 1) continue;

			/* What the table is: said on the pin, or wired in */
			UDataTable* Said = Cast<UDataTable>(Table->DefaultObject);

			if (Said == nullptr && Table->LinkedTo.Num() == 0) continue;

			/* Everything wired to the two, taken before either is pulled apart */
			const TArray<UEdGraphPin*> Enters = Execute->LinkedTo;
			const TArray<UEdGraphPin*> Tables = Table->LinkedTo;
			const TArray<UEdGraphPin*> Names = RowName->LinkedTo;
			const FString NameSaid = RowName->DefaultValue;
			const TArray<UEdGraphPin*> Readers = OutRow->LinkedTo;
			const FEdGraphPinType RowType = OutRow->PinType;
			const TArray<UEdGraphPin*> WhenFound = Branch->GetThenPin() != nullptr ? Branch->GetThenPin()->LinkedTo : TArray<UEdGraphPin*>();
			const TArray<UEdGraphPin*> WhenNot = Branch->GetElsePin() != nullptr ? Branch->GetElsePin()->LinkedTo : TArray<UEdGraphPin*>();

			UK2Node_GetDataTableRow* Lookup = static_cast<UK2Node_GetDataTableRow*>(NewObject<UK2Node>(Graph, LookupClass));

			Graph->AddNode(Lookup, false, false);

			Lookup->CreateNewGuid();
			Lookup->PostPlacedNewNode();
			Lookup->AllocateDefaultPins();

			/* Where the call stood, since that is where the run reads it */
			Lookup->NodePosX = Call->NodePosX;
			Lookup->NodePosY = Call->NodePosY;

			/* The table first, since that is what tells the row pin what it is */
			if (UEdGraphPin* Into = Lookup->GetDataTablePin()) {
				if (Said != nullptr) {
					Into->DefaultObject = Said;

					Lookup->PinDefaultValueChanged(Into);
				}

				for (UEdGraphPin* From : Tables) Schema->TryCreateConnection(From, Into);
			}

			if (UEdGraphPin* Keyed = Lookup->GetRowNamePin()) {
				if (Names.Num() > 0) {
					for (UEdGraphPin* From : Names) Schema->TryCreateConnection(From, Keyed);
				} else {
					Keyed->DefaultValue = NameSaid;
				}
			}

			UEdGraphPin* Row = Lookup->GetResultPin();

			/* A table wired in rather than said, whose kind of row only shows once it runs: the call
			 * knew it from the local it wrote into, so the node is told the same */
			if (Row != nullptr && Row->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard && RowType.PinCategory != UEdGraphSchema_K2::PC_Wildcard) {
				Row->PinType = RowType;
			}

			if (Row != nullptr) {
				for (UEdGraphPin* Reader : Readers) Schema->TryCreateConnection(Row, Reader);
			}

			if (UEdGraphPin* In = Lookup->GetExecPin()) {
				for (UEdGraphPin* Enter : Enters) Schema->TryCreateConnection(Enter, In);
			}

			if (UEdGraphPin* Found = Lookup->GetThenPin()) {
				for (UEdGraphPin* Next : WhenFound) Schema->TryCreateConnection(Found, Next);
			}

			if (UEdGraphPin* NotFound = Lookup->GetRowNotFoundPin()) {
				for (UEdGraphPin* Next : WhenNot) Schema->TryCreateConnection(NotFound, Next);
			}

			Graph->RemoveNode(Branch);
			Graph->RemoveNode(Call);

			PutBack++;
		}

		if (PutBack > 0) {
			UE_LOG(LogReflectionTidyDataTable, Display, TEXT("\"%s\" had %d data table row(s) looked up and branched on at once"), *Graph->GetName(), PutBack);
		}

		return PutBack;
	}
};

REGISTER_TIDY(FDataTableRowTidy)
