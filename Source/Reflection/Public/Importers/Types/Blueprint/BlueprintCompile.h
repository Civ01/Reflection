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
 * compiled during a run is remembered, and once the run is over the ones still in error have their
 * nodes refreshed and are compiled again against everything that has finished since. */
struct REFLECTION_API FBlueprintSettle {
	/* Forgets what an earlier run left behind */
	static void Begin();

	/* Compiles again whatever was left in error, as long as each pass fixes something. Answers how many came good. */
	static int32 Settle();

	/* Remembered by every guarded compile, so nothing has to say it */
	static void Track(UBlueprint* Blueprint);
};
