/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

class UBlueprint;

/* Compiling a blueprint that was read out of a game, without taking the editor with it */
REFLECTION_API bool CompileBlueprintGuarded(UBlueprint* Blueprint,
	EBlueprintCompileOptions Options = EBlueprintCompileOptions::None);

/* Making a blueprint, which compiles it before it hands it back and so can go the same way. */
REFLECTION_API UBlueprint* CreateBlueprintGuarded(UClass* ParentClass, UObject* Outer, FName Name,
	EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType);

/* Blueprints that import one another build against each other half finished.
 *
 * A character that names its weapon brings the weapon in while the character is still being made,
 * and the weapon compiles against a character with none of its variables or dispatchers yet. Each one
 * compiled during a run is remembered, and once the run is over the ones left with errors or warnings have their
 * nodes refreshed and are compiled again against everything that has finished since. */
/* A blueprint whose import is under way, for as long as this is alive.
 *
 * Compiling a blueprint lays out again every node in every blueprint that depends on it, and has
 * them compiled too. A blueprint that brings another in while it is half built a character
 * reaching its widgets, a GUI reaching the widgets it spawns is one of those, and the import
 * laying it out is holding on to its nodes and pins the whole time. Laid out again underneath it,
 * every one of those is gone, and the import goes on with them and stops the editor.
 *
 * So while one is under construction, compiling anything else leaves it out of the dependents. It
 * is compiled when its own import gets there, and again when the run settles. */
struct REFLECTION_API FBlueprintUnderConstruction {
	explicit FBlueprintUnderConstruction(UBlueprint* Blueprint);
	~FBlueprintUnderConstruction();

	FBlueprintUnderConstruction(const FBlueprintUnderConstruction&) = delete;
	FBlueprintUnderConstruction& operator=(const FBlueprintUnderConstruction&) = delete;

private:
	TWeakObjectPtr<UBlueprint> Held;
};

struct REFLECTION_API FBlueprintSettle {
	/* Forgets what an earlier run left behind */
	static void Begin();

	/* Compiles again whatever was left with errors or warnings, as long as each pass fixes something. Answers how many came good. */
	static int32 Settle();

	/* Remembered by every guarded compile, so nothing has to say it */
	static void Track(UBlueprint* Blueprint);

	/* Asks for a blueprint to be read again once the run is over, because something it calls was not
	 * there yet when its graphs were laid out */
	static void Reread(UBlueprint* Blueprint);

	/* Whether a blueprint being imported now should only declare what it is, and have its graphs
	 * laid out once the run is over. True for one brought in by another while a run is going. */
	static bool ShouldDefer();

	/* Its graphs are laid out at the end of the run, after everything it was brought in with */
	static void Defer(UBlueprint* Blueprint);

	/* Laid out in full after all, so there is nothing left to do for it at the end */
	static void Undefer(UBlueprint* Blueprint);
};
