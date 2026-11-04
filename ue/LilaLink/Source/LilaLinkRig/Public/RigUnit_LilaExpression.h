#pragma once

#include "Units/RigUnit.h"

#include "RigUnit_LilaExpression.generated.h"

/**
 * A Lila expression: reads its inputs (bone channels in Lila's joint space, curves, float controls), evaluates the program
 * exactly as Lila's rig math nodes do, and writes the result to its target. LilaLink's unrealExpressionRig builds these.
 */
USTRUCT(meta = (DisplayName = "Lila Expression", Category = "Lila", Keywords = "Expression,Lila,Maya,Driven", NodeColor = "0.42 0.2 0.62"))
struct LILALINKRIG_API FRigUnit_LilaExpression : public FRigUnitMutable
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** The expression as written in Lila, for reading; Program is what runs. */
	UPROPERTY(meta = (Input))
	FString Expression;

	/** The compiled expression (lila_expr text form). */
	UPROPERTY(meta = (Input))
	FString Program;

	/** JSON: where each input comes from, the target, and the axis change between Lila and Unreal. */
	UPROPERTY(meta = (Input))
	FString Bindings;

	/** The value written to the target. */
	UPROPERTY(meta = (Output))
	double Result = 0.0;
};
