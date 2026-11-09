#pragma once

#include "Units/RigUnit.h"

#include "RigUnit_LilaStrings.generated.h"

/**
 * Lila Strings in Control Rig: pose a bone the way Lila's Strings modes do, through the character's rule web (its
 * contacts, limbs, lines of action and stiffness, from Lila's stringsRuleWeb). The pose comes from the moment: nothing is
 * kept between evaluations but the bones' transforms. LilaLink's unrealStringsRig builds rigs with these.
 */
USTRUCT(meta = (Abstract, Category = "Lila|Strings", NodeColor = "0.42 0.2 0.62"))
struct LILALINKRIG_API FRigUnit_LilaStringsBase : public FRigUnitMutable
{
	GENERATED_BODY()

	/** The rule web as JSON, as stringsRuleWeb writes it. */
	UPROPERTY(meta = (Input))
	FString RuleWeb;

	/** The bone being posed. */
	UPROPERTY(meta = (Input))
	FName Bone;

	/** A control whose global transform is where the bone goes; none uses Target. */
	UPROPERTY(meta = (Input))
	FName Control;

	/** Where the bone goes, global, when no Control is given. */
	UPROPERTY(meta = (Input))
	FTransform Target = FTransform::Identity;

	/** The rule web's contacts hold where they were (feet stay planted). */
	UPROPERTY(meta = (Input))
	bool bHoldContacts = true;

	/** 0 leaves the pose, 1 poses all the way. */
	UPROPERTY(meta = (Input, ClampMin = "0.0", ClampMax = "1.0"))
	float Alpha = 1.0f;

	/** Largest distance left between a held contact and where it was, cm. */
	UPROPERTY(meta = (Output))
	float Error = 0.0f;
};

/** Forward: the bone turns to the target's rotation and its children follow. */
USTRUCT(meta = (DisplayName = "Lila Strings Forward", Keywords = "Strings,FK,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsForward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;
};

/** Backward: the bone turns to the target's rotation, the body swinging around it while the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings Backward", Keywords = "Strings,Reverse,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsBackward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;
};

/** IK: the bone's limb bends to put it at the target; on the hips, the body moves and the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings IK", Keywords = "Strings,IK,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsIK : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** Bones above this one that bend; 0 its limb from the rule web, or up to the next branch. */
	UPROPERTY(meta = (Input, ClampMin = "0"))
	int32 ChainLength = 0;

	/** The bone also takes the target's rotation. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;
};

/** Full Body: the body moves to put the bone at the target when its limb can't reach, stiff bones resisting, the contacts holding. */
USTRUCT(meta = (DisplayName = "Lila Strings Full Body", Keywords = "Strings,FBIK,Full Body,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsFullBody : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** The bone also takes the target's rotation. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;
};
