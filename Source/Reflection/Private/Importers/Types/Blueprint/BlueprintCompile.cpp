/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/BlueprintCompile.h"

#include "Engine/EngineUtilities.h"
#include "Importers/Constructor/ImportIssues.h"

#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FeedbackContext.h"
#include "Misc/SlowTask.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Package.h"
#include "Settings/SettingsAccess.h"
#include "Engine/Log.h"
#include "BlueprintCompilationManager.h"
#include "K2Node_BaseMCDelegate.h"
#include "EdGraphSchema_K2.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/UObjectIterator.h"

namespace {
	/* Where the slow tasks were before, and what to drop back to afterwards.
	 *
	 * A slow task puts itself on a stack the feedback context holds and takes itself off again when
	 * it goes out of scope. Coming back out through the guard is not going out of scope, so the
	 * compiler's own are left on the stack pointing at frames that no longer exist, and the next
	 * slow task to end reads one of them by name. Anything pushed past where the compile started is
	 * dropped rather than left there. */
	int32 SlowTasks() {
		return GWarn != nullptr ? GWarn->GetScopeStack().Num() : 0;
	}

	void DropSlowTasksTo(const int32 Depth) {
		if (GWarn == nullptr) return;

		FSlowTaskStack& Stack = const_cast<FSlowTaskStack&>(GWarn->GetScopeStack());

		if (Stack.Num() > Depth) {
			Stack.SetNum(Depth);
		}
	}

	/* Nothing of its own to unwind, which is what lets the guard sit around it. Everything the
	 * compiler makes is made further in, where the compiler is the one that owns it. */
	bool Attempt(UBlueprint* Blueprint, const EBlueprintCompileOptions Options) {
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
		__try {
			FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#else
		FKismetEditorUtilities::CompileBlueprint(Blueprint, Options);

		return true;
#endif
	}

	/* Same shape, and for the same reason: making one runs the compiler over it before it comes
	 * back, so the making is no safer than the compiling. */
	UBlueprint* AttemptCreate(UClass* ParentClass, UObject* Outer, const FName Name,
		const EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType) {
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
		__try {
			return FKismetEditorUtilities::CreateBlueprint(ParentClass, Outer, Name, BlueprintType,
				BlueprintClassType, BlueprintGeneratedClassType);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
#else
		return FKismetEditorUtilities::CreateBlueprint(ParentClass, Outer, Name, BlueprintType,
			BlueprintClassType, BlueprintGeneratedClassType);
#endif
	}
}

bool CompileBlueprintGuarded(UBlueprint* Blueprint, const EBlueprintCompileOptions Options) {
	if (Blueprint == nullptr) return false;

	const FString Named = Blueprint->GetName();

	const int32 Depth = SlowTasks();

	if (Attempt(Blueprint, Options)) {
		FBlueprintSettle::Track(Blueprint);

		return true;
	}

	DropSlowTasksTo(Depth);

	/* Kept off disk, because what is here cannot be read back. */
	if (UPackage* Held = Blueprint->GetOutermost()) {
		Held->SetDirtyFlag(false);
		Held->SetFlags(RF_Transient);
	}

	UE_LOG(LogReflection, Error, TEXT("\"%s\" could not be compiled and nothing was written for it"), *Named);

	FImportIssues::Report(
		EImportIssue::Data,
		FString::Printf(TEXT("\"%s\" could not be compiled"), *Named),
		FString::Printf(
			TEXT("The blueprint was built from the export and the compiler could not read it back. That is the asset being wrong in a way nothing here checked for, most often a class it says it comes from or a type it names being something this engine hasn't got. What a compile leaves half built cannot be read back either, so nothing was written for '%s' rather than leaving a package that stops the editor whenever anything opens it. Everything importing alongside it carried on."),
			*Named)
	);

	return false;
}

UBlueprint* CreateBlueprintGuarded(UClass* ParentClass, UObject* Outer, const FName Name,
	const EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType) {
	const int32 Depth = SlowTasks();

	if (UBlueprint* Made = AttemptCreate(ParentClass, Outer, Name, BlueprintType, BlueprintClassType, BlueprintGeneratedClassType)) {
		return Made;
	}

	DropSlowTasksTo(Depth);

	UE_LOG(LogReflection, Error, TEXT("\"%s\" could not be made and nothing was imported for it"), *Name.ToString());

	FImportIssues::Report(
		EImportIssue::Data,
		FString::Printf(TEXT("\"%s\" could not be made"), *Name.ToString()),
		FString::Printf(
			TEXT("Making a blueprint compiles it, and the compiler could not read what it was given. Usually that is the class it says it comes from being something this engine hasn't got, or having come across broken itself. Nothing was written for '%s' and the rest of the import carried on."),
			*Name.ToString())
	);

	return nullptr;
}

namespace {
	TArray<TWeakObjectPtr<UBlueprint>> GSettleTracked;

	/* Settling compiles through the same guard, which would only put each one back on the list */
	bool bSettleRunning = false;

	/* Compiles everything queued in one go, with the same guard a single compile has */
	bool AttemptFlush() {
#if PLATFORM_WINDOWS && !PLATFORM_SEH_EXCEPTIONS_DISABLED
		__try {
			FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();

			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
#else
		FBlueprintCompilationManager::FlushCompilationQueueAndReinstance();

		return true;
#endif
	}

	/* Whether a class is the one that stands for its blueprint now, rather than a copy the compiler
	 * keeps on the side or one an earlier compile left behind */
	bool IsCurrentClass(const UClass* Class) {
		if (Class == nullptr || Class->HasAnyClassFlags(CLASS_NewerVersionExists)) return false;
		if (Class->GetOutermost() == GetTransientPackage()) return false;

		const FString Named = Class->GetName();

		return !Named.StartsWith(TEXT("SKEL_")) && !Named.StartsWith(TEXT("REINST_")) && !Named.StartsWith(TEXT("TRASHCLASS_"));
	}

	/* Points a bind, unbind or call at the class that really declares the dispatcher it names.
	 *
	 * A dispatcher on another blueprint is often not there yet when the node is laid out, because
	 * that blueprint is still being made further up the import: the node is left asking for it on
	 * its own blueprint, which has no dispatcher of that name. By the end of the run the other one
	 * has it. What the node is asked against says which class that is, and where it is wired to
	 * nothing, a dispatcher only one blueprint declares can only be that one. */
	int32 RepointDelegates(UBlueprint* Blueprint) {
		TArray<UK2Node_BaseMCDelegate*> Nodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Nodes);

		int32 Repointed = 0;

		for (UK2Node_BaseMCDelegate* Node : Nodes) {
			if (Node == nullptr || Node->GetProperty() != nullptr) continue;

			const FName Called = Node->GetPropertyName();

			if (Called.IsNone()) continue;

			UClass* Owner = nullptr;

			if (const UEdGraphPin* Target = Node->FindPin(UEdGraphSchema_K2::PN_Self); Target != nullptr && Target->LinkedTo.Num() > 0 && Target->LinkedTo[0] != nullptr) {
				UClass* Wired = Cast<UClass>(Target->LinkedTo[0]->PinType.PinSubCategoryObject.Get());

				if (Wired != nullptr && FindFProperty<FMulticastDelegateProperty>(Wired, Called) != nullptr) Owner = Wired;
			}

			if (Owner == nullptr) {
				TSet<UClass*> Declaring;

				for (TObjectIterator<UBlueprintGeneratedClass> It; It; ++It) {
					if (!IsCurrentClass(*It)) continue;

					/* Declared there rather than inherited, or every child of the owner would count too */
					if (const FMulticastDelegateProperty* Found = FindFProperty<FMulticastDelegateProperty>(*It, Called); Found != nullptr && Found->GetOwnerClass() == *It) {
						Declaring.Add(*It);
					}
				}

				if (Declaring.Num() == 1) Owner = *Declaring.CreateConstIterator();
			}

			if (Owner == nullptr) continue;

			/* One the blueprint has itself is asked of itself */
			if (Blueprint->GeneratedClass != nullptr && Blueprint->GeneratedClass->IsChildOf(Owner)) continue;

			Node->Modify();
			Node->DelegateReference.SetExternalMember(Called, Owner);

			Repointed++;
		}

		return Repointed;
	}
}

void FBlueprintSettle::Begin() {
	GSettleTracked.Reset();
}

void FBlueprintSettle::Track(UBlueprint* Blueprint) {
	if (Blueprint == nullptr || bSettleRunning) return;

	GSettleTracked.AddUnique(Blueprint);
}

int32 FBlueprintSettle::Settle() {
	if (bSettleRunning) return 0;

	TArray<TWeakObjectPtr<UBlueprint>> Tracked = MoveTemp(GSettleTracked);
	GSettleTracked.Reset();

	TGuardValue<bool> Guard(bSettleRunning, true);

	TSet<UBlueprint*> Recompiled;
	int32 Settled = 0;

	/* A blueprint coming good can be what the next one was waiting on, so it goes round again while
	 * anything changes. Bounded, since one that will never compile would otherwise keep it going. */
	for (int32 Pass = 0; Pass < 4; ++Pass) {
		TArray<UBlueprint*> Failing;

		for (const TWeakObjectPtr<UBlueprint>& Weak : Tracked) {
			UBlueprint* Blueprint = Weak.Get();

			/* One whose compile threw was kept off disk on purpose, and is left that way */
			if (Blueprint == nullptr || Blueprint->GetOutermost()->HasAnyFlags(RF_Transient)) continue;
			/* Warnings too: a member asked for on a class that was not finished yet is only a warning to the
			 * compiler, and the node it is on does nothing until it is found */
			if (Blueprint->Status != BS_Error && Blueprint->Status != BS_UpToDateWithWarnings) continue;

			Failing.Add(Blueprint);
		}

		if (Failing.Num() == 0) break;

		TMap<UBlueprint*, EBlueprintStatus> Was;
		int32 Repointed = 0;

		for (UBlueprint* Blueprint : Failing) {
			Was.Add(Blueprint, Blueprint->Status);

			Repointed += RepointDelegates(Blueprint);

			/* Reconstructed the way loading one does, so a node pointing at something that was not
			 * there when it was laid out picks up what is there now */
			FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
		}

		/* All of them together, the way loading compiles what it brought in. One at a time, each compile
		 * makes its class over again, and the next blueprint still holds members of the class it replaced:
		 * the compiler reads one of those as a property whose owner is gone, and stops the editor. */
		for (UBlueprint* Blueprint : Failing) {
			FBlueprintCompilationManager::QueueForCompilation(Blueprint);
		}

		if (!AttemptFlush()) {
			UE_LOG(LogReflection, Error, TEXT("Settling pass %d: compiling %d blueprint(s) together failed, and nothing more was tried"), Pass + 1, Failing.Num());

			break;
		}

		int32 Came = 0;

		for (UBlueprint* Blueprint : Failing) {
			Recompiled.Add(Blueprint);

			/* Better counts, errors down to warnings included, since that can be what the next one waits on */
			const EBlueprintStatus Before = Was.FindRef(Blueprint);

			if (Blueprint->Status == BS_UpToDate || (Before == BS_Error && Blueprint->Status == BS_UpToDateWithWarnings)) Came++;
		}

		UE_LOG(LogReflection, Display, TEXT("Settling pass %d: %d dispatcher reference(s) repointed, %d of %d blueprint(s) left with errors or warnings improved"),
			Pass + 1, Repointed, Came, Failing.Num());

		Settled += Came;

		if (Came == 0) break;
	}

	/* Written again where the run writes what it makes, otherwise left for whoever saves */
	for (UBlueprint* Blueprint : Recompiled) {
		if (GetSettings()->AssetSettings.SaveAssets) {
			SavePackage(Blueprint->GetOutermost());
		} else {
			Blueprint->MarkPackageDirty();
		}
	}

	return Settled;
}
