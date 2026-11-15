# Movement survey (step 4 of the next ten steps; plan step 17)

What Faith's movement is made of, from the generated SDK (`sdk/include/sdk/TdGame.hpp`), and what a mod can do with each
part. Generated from the SDK's property flags on 2026-11-14; the per-class table below is the data.

## What it says

- **142 move classes; 262 config tuning values (Edit, Config), 16 other editable defaults, 396 runtime state fields (moves and pawns).**
- **Tuning values are script properties.** Every value marked `(Edit, Config)` comes from `TdGame.ini` into the class's
  defaults and from there into each move object the pawn makes (`TdPawn.Moves`, one object per move). A mod changes
  one by writing the property: on the live move objects (the current pawn) and on the class default object (so a pawn
  made at the next checkpoint restart gets it too), inside a transaction, so the change log puts both back when the mod
  goes. No bytecode or native work is needed for these: they are the bulk of what "tuning" means (jump heights, every
  speed, wall-run and wall-climb limits, grab and ledge distances, slide and roll timings).
- **The pawn's own speeds** are on `TdPawn` (SneakVelocity, WalkVelocity, JogVelocity, RunVelocity, SprintVelocity,
  SpeedMax/MinBaseVelocity, ...) and the engine's `Pawn` (GroundSpeed, AirSpeed, JumpZ, AccelRate, AirControl): the same
  kind of property, on the pawn instead of a move.
- **State fields** (timers, the move's current velocities, flags) are read, not tuned: `movement_get` reports the ones
  that describe what the player is doing (the move, the previous move, velocity, grounded or falling).
- **Not covered here, and the next part of the survey:** constants that are literals in the moves' bytecode (`StartMove`
  and friends compute with numbers written in the script: they need the decompiled script, `mirage sdk` writes it to
  `build/sdk-reference`, and change by script patch or injection), and constants inside native code (physics in
  `UTdPhysicsMove`'s native functions, reached through the engine symbols of step 2). Gravity is WorldInfo's
  (`WorldGravityZ`), already a property.

## What step 4 made of it

- **Tuning is content, not a new API.** A content mod's `content/*.toml` already overrides a class's defaults and, with
  `instances = true`, the live objects, and puts both back when the mod goes off (step 7). Movement tuning is that, on
  the move classes:

  ```toml
  ["TdGame.Default__TdMove_Jump"]
  BaseJumpZ = 945.0         # The game's is 630
  instances = true          # The move objects the player has now (TdPawn.Moves) too
  ```

  Self-test "Mirage movement tuning" (`tests/mods/movement-tuning`, the plan's Scenario A): five constants on the jump,
  the wall run and the pawn; on, the live objects hold them and a standing jump goes from 124 to 279 units (the height
  goes with the upward speed squared: 124 x (945 / 630)^2 = 279, as measured); off, every value is the game's again,
  exactly, and so is the jump.
- **Code that tunes** writes the same properties with `object_set` inside a transaction (`change_begin`/`change_end`,
  Lua `mirage.transaction`), which the change log puts back.
- **What the player is doing**: `movement_get` (Lua `mirage.movement()`, C# `Mod.Movement()`, C++ `mirage::Movement`):
  the move (EMovement), the previous move, physics, velocity, horizontal speed, and the current move's object.
- **A save state restores the move objects' properties too** (the menu's save states keep every value of TdMove
  objects): loading a state saved before a tuning mod was on puts the game's values back until the mod is switched
  again.
- **Transitions**: `movement_transitions` (Lua `mirage.transitions()`) keeps the last 256 move changes with the tick,
  the level's time and the moves' names; every change, whether or not a mod listens.
- **Left for later**: the constants that are literals in bytecode or native code (the decompiled-script half of this
  survey).

## Per class


### TdMovementVolume (PhysicsVolume)
- Defaults (edit): NumSplineSegments (int)
- State: FloorNormal (Vector), WallNormal (Vector), MoveDirection (Vector), Start (Vector), End (Vector), Middle (Vector), Center (Vector), OldScale (Vector), OldLocation (Vector), OldRotation (Rotator), SplineLength

### TdMoveNode (PathNode)
- Tuning (config): SpecialMoveCost (int)

### TdMove (Object)
- Tuning (config): SpeedModifier, FrictionModifier, RedoMoveTime, StickyAngle (int), StickyAimedModifier
- Defaults (edit): RootOffset (Vector), SwanNeckEnableAtPitch (int), SwanNeckForward, SwanNeckDown
- State: AiAimPenalty, AiAimOneShotPenalty, DisableMovementTime, DisableLookTime, LastCanDoMoveTime, LastStopMoveTime, MoveActiveTime, PreciseLocationSpeed, PreciseLocation (Vector), PreciseRotationInterpolationTime, PreciseRotation (Rotator), LookAtTargetLocation (Vector), LookAtTargetAngle (Rotator), LookAtTargetInterpolationTime, LookAtTargetDuration, CancelResetCameraLookTime, ResetCameraLookTime, MinLookConstraint (Rotator), MaxLookConstraint (Rotator), CustomCollisionRadius, CustomCollisionHeight, WeaponInactivePitchAimingLimit (int), RootMotionScale (Vector), AnimBlendTime, Timer

### TdMoveNode_GrabHeave (TdMoveNode)
- State: WallDistance, MinWallHeight, MaxWallHeight

### TdMoveNode_Vault (TdMoveNode)
- Tuning (config): iHighVaultCost (int), iVaultOntoCost (int)
- Defaults (edit): ForcedSpeed
- State: VaultOverWallDistance_LowVault, VaultOntoWallDistance_LowVault, VaultOverWallDistance_HighVault, VaultOntoWallDistance_HighVault, MinWallHeight_LowVault, MaxWallHeight_LowVault, MinWallHeight_HighVault, MaxWallHeight_HighVault

### TdMoveNode_JumpIntoGrab (TdMoveNode)
- Defaults (edit): InitialSpeed

### TdMoveNode_WallRun (TdMoveNode)
- State: StartLocation (Vector), EndLocation (Vector), ForcedSpeed, WallDistance, MinWallHeight, MaxWallHeight

### TdMove_180Turn (TdMove)
- Tuning (config): TurnAnimBlendInTime, TurnAnimBlendOutTime

### TdMove_180TurnInAir (TdPhysicsMove)
- State: WantedRotation (Rotator)

### TdMove_Barge (TdPhysicsMove)
- Tuning (config): BargeMinTraceDistance, BargeTraceTime, BargeAddOnSpeed, BargeMaxSpeed, BargeKickThresholdSpeed
- State: BargeAnimTime

### TdMove_AirBarge (TdMove_Barge)
- Tuning (config): HeightBoostDuration, TotalHeightBoost
- State: HeightBoostLeft, PreCollisionVelocity (Vector)

### TdMove_AnimationPlayback (TdMove)
- State: PlayRate, BlendInTime, BlendOutTime

### TdMove_AutoStepUp (TdPhysicsMove)
- Tuning (config): StepUpDistanceLimit, StepUpSpeedLimit, StepUpLowMinHeight, StepUpMediumMinHeight, StepUpHighMinHeight, StepUpHighMaxHeight, StepUpOptimalLowHeight, StepUpOptimalMediumHeight, StepUpOptimalHighHeight
- State: EndPosition (Vector), StartDistance, SavedVelocity (Vector)

### TdMove_Balance (TdPhysicsMove)
- Tuning (config): TimeToCounter, GravityInfluence, ControlInfluence, SpeedInfluence, CameraInfluence
- State: CurrentParamOnCurve, BalanceFactor, CounterTimer, CurrentMoveDirection (Rotator), ExternalForce

### TdMove_BotMelee (TdPhysicsMove)
- State: BlendInTime, BlendOutTime, PredictedEnemyLocation (Vector), AnimationLength, EnemyStartLocation (Vector), WantedRotation (Rotator)

### TdMove_BotJump (TdPhysicsMove)
- State: ForcedSpeed, AnticipationTime

### TdMove_BotLanding (TdMove_AISpecialMove)
- State: ShortRunningDistance, MediumRunningDistance, LongRunningDistance, SoftLandingHeight, RollDistance, StartLocation (Vector)

### TdMove_BotPursuitGetDistance (TdMove)
- State: InitialPawnRotationYaw (int), DeltaPawnRotationYaw (int), InitialLegRotationYaw (int), DeltaLegRotationYaw (int)

### TdMove_BotStart (TdPhysicsMove)
- State: DeltaPawnRotationYaw (int), InitialPawnRotationYaw (int), InitialLegRotationYaw (int), DeltaLegRotationYaw (int), WantedRotation (Rotator), AnimationTurnAngle (int), BlendInTime, BlendOutTime, TimeIntoAnimation, AnimationLength, fMoveStartedTimeStamp

### TdMove_BotStartRunning (TdMove_BotStart)
- State: MoveStartedTimeStamp, MinTimeBetweenTwoStartMoves

### TdMove_BotStartWalking (TdMove_BotStart)
- State: MinTimeBetweenTwoStartMoves

### TdMove_BotStop (TdMove)
- Defaults (edit): MaxRunningStopTriggerDist, MinRunningStopTriggerDist, MaxWalkingStopTriggerDist, MinWalkingStopTriggerDist, StopMoveDistanceRunning, StopMoveDistanceWalking
- State: DeltaPawnRotationYaw (int), InitialPawnRotationYaw (int), InitialLegRotationYaw (int), DeltaLegRotationYaw (int), WantedRotation (Rotator), BlendInTime, BlendOutTime, TimeIntoRotation, RotationTime, MoveDestination (Vector), SetMoveDestinationTimeStamp

### TdMove_StumbleBase (TdPhysicsMove)
- State: DamageLocation (Vector), damageMomentum (Vector), InstigatorLocation (Vector), DirectionalBias

### TdMove_BotTurnRunning (TdPhysicsMove)
- State: WantedDeltaRotation (Rotator), InitialPawnRotation (Rotator), RotationCompensationRate, DirectionOfMovement (Rotator), PawnRotation (Rotator)

### TdMove_BotTurnStanding (TdPhysicsMove)
- State: DeltaRotationYaw (int), DeltaLegRotationYaw (int), InitialPawnRotationYaw (int), InitialLegRotationYaw (int), BlendInTime, BlendOutTime, TimeIntoRotation, RotationTime, FocusPoint (Vector), SetFocusPointTimeStamp, LegTurnAngle

### TdMove_Climb (TdPhysicsMove)
- Tuning (config): IdleBlendInTime, ClimbBlendInTime, ClimbDownBlendInTime, ClimbDownFastVelocity, ClimbFastUpPipeAnimRate, StartTurningAngle (int)

### TdMove_Coil (TdPhysicsMove)
- Tuning (config): HeightBoostDuration, TotalHeightBoost, CoilMinTriggerSpeed, CoilTime
- State: HeightBoostLeft

### TdMove_Cutscene (TdPhysicsMove)
- State: TargetLocation (Vector), TargetRotation (Rotator)

### TdMove_DodgeJump (TdPhysicsMove)
- Tuning (config): BaseJumpZ, JumpAddXY, StrafeThreshold, DodgeJumpInertiaConservation, JumpBlendInTime, JumpBlendOutTime, LookAtAIVelThreshold, LookAtAITraceDistance, LookAtAIRadiusThreshold

### TdMove_EnterCover (TdMove)
- State: PreviousLocation (Vector)

### TdMove_Falling (TdPhysicsMove)
- Tuning (config): StickyAimAfterAirTime
- State: AirTime

### TdMove_Grab (TdPhysicsMove)
- Tuning (config): GrabDesiredLedgeOffset (Vector), GrabMaxAngle, GrabMinGrabableZNormal, HangFreeZDistanceCheck, StartTurningAngle
- State: RelativeExtent, DistanceToWallFromFeet, HangFreeMinLookContraint (Rotator), HangFreeMaxLookContraint (Rotator), SlopeMinLookContraint (Rotator), SlopeMaxLookContraint (Rotator), ShimmyAroundCornerMinLookContraint (Rotator), ShimmyAroundCornerMaxLookContraint (Rotator), ShimmyAroundCornerFreeMinLookContraint (Rotator), ShimmyAroundCornerFreeMaxLookContraint (Rotator), TargetYaw (int), TargetLocation (Vector), ShimmyVelocity, ShimmyTime, LastShimmyTimeSeconds, DisableShimmyTime, StartLookingAtLedgeTime, StopLookingAtLedgeTime

### TdMove_GrabJump (TdPhysicsMove)
- Tuning (config): GrabJumpOffZHeight, GrabJumpPushAwayMaxSpeed, GrabJumpPushAwayMinSpeed, GrabAllowedJumpAngle
- State: JumpVelocity (Vector), DeltaJumpYaw (int)

### TdMove_GrabPullUp (TdPhysicsMove)
- Tuning (config): GrabAllowedPullUpAngle (int)
- State: FloorOverLedgeLocation (Vector)

### TdMove_GrabPullUpBot (TdMove_AISpecialMove)
- State: PawnLocation (Vector), PawnRotation (Vector), VerticalTranslation, HorizontalTranslation, MoveStartLocation (Vector), MoveIntermediateLocation (Vector), MoveEndLocation (Vector)

### TdMove_GrabTransfer (TdPhysicsMove)
- Tuning (config): Allowed2DTransferDistance, AllowedZTransferDistance
- State: TransferLocation (Vector), TransferNormal (Vector), TransferLookAtLocation (Vector), TransferLedgeNormal (Vector), TransferSpeed, TransferDistance

### TdMove_Interact (TdPhysicsMove)
- Tuning (config): DistanceToAButton, DistanceToAValve
- State: AnimationInteractionDelay, EventDelay, MoveLocation (Vector), NormalFromTrigger2D (Vector), DistanceToInteractableObject

### TdMove_IntoClimb (TdPhysicsMove)
- State: FirstStepZDistance

### TdMove_IntoGrab (TdPhysicsMove)
- Tuning (config): IntoGrabMaxAngle, IntoGrabAlignSpeed, IntoGrabMinInitialAlignSpeed, GrabMinGrabableZNormal, GrabDesiredLedgeOffset (Vector), MinGrabLedgeAdjustDistance, IntoGrabMaxDistance, IntoGrabZVelocityThreshold, HangFoldedDownwardSpeedLimit, HangFoldedIntoGrabZSpeedThreshold, HangFoldedIntoGrabSpeed2DThreshold, HangFoldedUpperDeltaDistance, HangFoldedLowerDeltaDistance, HangFoldedMaxDistance, HangImpactMinZSpeed, HangHardImpactMinZSpeed
- State: IntoGrabSpeed

### TdMove_IntoZipLine (TdPhysicsMove)
- Tuning (config): ZVelocityFallLimit, IntoZiplineBlendInTime, IntoZiplineBlendOutTime
- Defaults (edit): HangOffset (Vector)
- State: EnterZipLineParam, SavedInitial2DVelocity (Vector), SameZipLineRedoMoveTime

### TdMove_Jump (TdPhysicsMove)
- Tuning (config): BaseJumpZ, BaseJumpZHeavy, JumpAddXY, LongJumpSlowThreshold, LongJumpNormalThreshold, LongJumpFastThreshold, JumpBlendInTime, JumpBlendOutTime, JumpStillBlendOutTime
- State: PreJumpMomentum, WantedJumpVelocity (Vector), CanDoMoveTaserLimit

### TdMove_JumpBot_Base (TdMove_AISpecialMove)
- State: ForcedSpeed, AnticipationTime

### TdMove_JumpIntoGrabBot (TdMove_AISpecialMove)
- State: MoveStartLocation (Vector), MoveEndLocation (Vector)

### TdMove_Landing (TdMove)
- Tuning (config): HardLandingDamage, LandingSpeedReduction, HardLandingHeight, SkillRollLandingHeight, SoftLandingHeight

### TdMove_LayOnGroundBot (TdMove)
- State: LayDownIdleTime

### TdMove_LedgeWalk (TdPhysicsMove)
- State: CurrentParamOnCurve, StrafeFactor, StrafeSpeed

### TdMove_MeleeBase (TdPhysicsMove)
- Tuning (config): MeleeDamage, MaxMeleeDistance, MaxMeleeAngle
- State: TargetingRotationSpeed, TargetingMaxDistance, HitDetectionStart (Vector), HitDetectionLastStart (Vector), TraceOffset (Vector), TraceExtent (Vector), CanDoMoveTaserLimit

### TdMove_Melee (TdMove_MeleeBase)
- Tuning (config): SoccerKickDamage, BlendInMissed, BlendOutMissed
- State: ComboCounter (int), ComboTimer, ComboQueuedActions (int), PreventTime

### TdMove_MeleeAir (TdMove_MeleeBase)
- Tuning (config): MeleeAirAboveMinAngle, MeleeAirAboveMinSeparation, MeleeAirAboveMaxSeparation
- State: ImpactMomentum (Vector), LookAtAngle (Rotator)

### TdMove_MeleeAirAbove (TdMove_MeleeBase)
- State: TargetRotation (Rotator), TargetLocation (Vector)

### TdMove_MeleeCrouch (TdMove_MeleeBase)
- State: BargeTraceDistance

### TdMove_MeleeSlide (TdMove_MeleeBase)
- State: BargeTraceDistance

### TdMove_MeleeVault (TdMove_MeleeBase)
- State: ImpactMomentum (Vector), LookAtAngle (Rotator), VaultTimeDown, VaultEndPosition (Vector), SavedVelocity (Vector)

### TdMove_MeleeWallrun (TdMove_MeleeBase)
- State: WallrunNormal (Vector)

### TdMove_RumpSlide (TdPhysicsMove)
- Tuning (config): MaxSlideSpeed, SideControl, GravityModifier, InitialSpeedLoss, MinSlideFloorZ
- State: TimeFalling, OldFloorZ

### TdMove_Slide (TdMove)
- Tuning (config): SlideAbortSpeed, SlideAbortTime, MaxFloorInclineZ
- State: SlideAngleTarget (int)

### TdMove_SlideBot (TdMove_AISpecialMove)
- Tuning (config): SlideAbortSpeed
- State: EndTarget (Vector), startPos (Vector), DistanceToTravel

### TdMove_SpeedVault (TdPhysicsMove)
- Tuning (config): VaultClearObjectHeight, MaxTimeToLedge
- State: ActiveVaultType (int), VaultEndPosition (Vector), OverEndLocation (Vector), OntoEndLocation (Vector), SavedVelocity (Vector), StartToHandplant (Vector), VaultState (int), MoveDirection (Vector), HandLocation (Vector), TargetLocation (Vector), VaultSpeed, HandplantHeight, CameraCollisionDirection (Vector)

### TdMove_VaultBot (TdMove_AISpecialMove)
- State: StartLocation (Vector), EndLocation (Vector), HandLocation (Vector)

### TdMove_SpringBoard (TdPhysicsMove)
- Tuning (config): SpringBoardMaxHeight, SpringBoardMinHeight, SpringBoardJumpZ, SpringBoardJumpXYAdd, SpringBoardJumpXYMin, IntermediateFootPlantHeight, IntermediateFootPlantDistance, CheckDistanceTime
- State: IntermediateFootPlantLedgeLocation (Vector), SpringBoardFootPlantLedgeLocation (Vector), StepTime1, StepTime2, SpringBoardState (int), SavedInitialSpeed (int)

### TdMove_StandGrabHeaveBot (TdMove_AISpecialMove)
- State: GrabLocation (Vector), StartLocation (Vector), EndLocation (Vector), ForcedSpeed, WallDistance, GrabZOffset, PawnLocation (Vector), PawnRotation (Vector), VerticalTranslation, HorizontalTranslation, StartJumpLocation (Vector), StartTime, EndTime, JumpStartTime, JumpEndTime, HeaveStartTime

### TdMove_StepUp (TdPhysicsMove)
- Tuning (config): StepUpDistanceLimit, StepUpSpeedLimit, StepUpLowMinHeight, StepUpMediumMinHeight, StepUpHighMinHeight, StepUpHighMaxHeight, StepUpOptimalLowHeight, StepUpOptimalMediumHeight, StepUpOptimalHighHeight
- State: EndPosition (Vector), StartDistance, SavedVelocity (Vector)

### TdMove_Swing (TdPhysicsMove)
- Tuning (config): ExitVelocityModifier, SwingPendulumLength, SwingAngleTimingOffset, SwingExitGravityModifier, SwingExitGravityModifierTime
- State: SwingVelocity, MaxSwingVelocity, SwingAngle, SwingDirection (Vector), BarDirection (Vector), SwingLocation (Vector), ShimmyVelocity, ShimmyTime

### TdMove_SwingJump (TdPhysicsMove)
- Tuning (config): GravityModifier, GravityModifierTimer, TargetVolumeOffset (Vector)

### TdMove_Vertigo (TdPhysicsMove)
- Tuning (config): ZoomFOV, ZoomRate, ZoomOutTime
- State: LastVertigoEdgePosition (Vector), LastActualVertigoEdgePosition (Vector)

### TdMove_Walking (TdPhysicsMove)
- Tuning (config): TriggerIdleAnimMinTime, TriggerIdleAnimMaxTime

### TdMove_WallClimb (TdPhysicsMove)
- Tuning (config): WallClimbingVelocityStartLimit, WallClimbingVerticalStartAngle, WallClimbingVerticalFriction, WallClimbingMaxDistance2D, AddOnSpeed2DHeight, AddOnSpeed2DMaxLimit, AddOnSpeedZHeight, AddOnSpeedZMaxLimit, WallClimbingGravity, MinLegdeZNormal, MinWallHeight, MinUpwardsVelocityToDoubleJump, MaxIntoWallClimbVelocityToDoubleJump
- State: LookAtEdgeAngle (Rotator), PossibleEdgeDestination (Vector), IntoWallClimbSpeed, GroundZLoc

### TdMove_WallClimb180TurnJump (TdPhysicsMove)
- Tuning (config): JumpOffZHeight, JumpPushAwaySpeed, JumpTimeWindow
- State: WantedJumpDir (Vector)

### TdMove_WallClimbDodgeJump (TdPhysicsMove)
- Tuning (config): BaseJumpZ, JumpAddXY, DodgeJumpInertiaConservation, JumpBlendInTime, JumpBlendOutTime

### TdMove_WallKick (TdPhysicsMove)
- Tuning (config): WallKickMaxDistance, WallKickCheckHeight, WallKickExtentWidth, WallKickExtentHeight, WallKickVelocity2D, WallKickVelocityZ

### TdMove_WallRun (TdPhysicsMove)
- Tuning (config): WallRunningForwardCheckDistance, WallRunningStrafeCheckDistance, WallRunningVerticalCheckDistance, WallRunningMinWallHeight, WallRunningMinSpeed, WallRunningVelocityStartLimit, WallRunningVelocityStopLimit, WallRunningForwardMinStartAngle, WallRunningForwardMaxStartAngle, WallRunningStrafeStartAngle, WallRunningHorisontalFriction, WallRunningHorisontalInitialZHeight, WallRunningHorisontalAcceleration, WallRunningHorisontalDeceleration, WallRunningHorisontalAlignSpeed, WallRunningIntoWallrunBlendInTime, WallRunningIntoWallrunBlendOutTime, WallRunningDelayPawnRotationTime, WallRunningDistanceForIntoWall, WallRunningRotatePawnAlongWallTime, WallRunningMoveToIntoPositionDegreeThreshold, MinimumVelocityIntoWall, MaximumVelocityIntoWall, WallrunStartUpperBodyAnimPlayRate, LookAlongWallInterpolationTime
- State: WallRunningBeginSpeed, WallNormal (Vector), PredictedWallHitLocation (Vector), IntoWallrunLocationTarget (Vector), ConsequtiveWallruns (int), MinContraintWorld (int), MaxContraintWorld (int)

### TdMove_WallRunBot (TdMove_AISpecialMove)
- State: PawnLocation (Vector), PawnRotation (Vector), VerticalTranslation, HorizontalTranslation, MoveStartLocation (Vector), MoveEndLocation (Vector), StartTime, EndTime, ClimbNotifierStartTime, ClimbNotifierEndTime

### TdMove_WallrunDodgeJump (TdPhysicsMove)
- Tuning (config): BaseJumpZ, JumpAddXY, DodgeJumpInertiaConservation, JumpBlendInTime, JumpBlendOutTime

### TdMove_WallrunJump (TdPhysicsMove)
- Tuning (config): WallRunningPushAwaySpeedNoob, WallRunningPushAwaySpeedProAdd, WallRunningPushForwardSpeedMin, WallRunningJumpOffZHeightForward, WallRunningJumpOffZHeightMaxAddTurned
- State: WallRunningJumpOffZSpeed, MinContraintWorld (int), MaxContraintWorld (int)

### TdMove_ZipLine (TdPhysicsMove)
- Tuning (config): MinZipVelocity, MinZipAcceleration, ZipFadeInTime, ZipFadeOutTime
- Defaults (edit): HangOffset (Vector)
- State: CurrentParamOnCurve, CurrentLookAtPoint (Vector)

### TdPawn (GamePawn)
- Tuning (config): LegRotationSpeed, GoBackLegAngleLimitMin (int), GoBackLegAngleLimitMax (int), LegAngleLimitFudge (int), SneakVelocity, WalkVelocity, JogVelocity, RunVelocity, SprintVelocity, ASFilterTime, SpeedMaxBaseVelocity, SpeedMinBaseVelocity, SpeedStrafeVelocityAccelerationFactor, SpeedWalkVelocityAccelerationFactor, SpeedSprintVelocityAccelerationFactor, SpeedEnergyDecelerationTime, SpeedEnergyDecelerationExponent, SpeedTurnDecelerationFactor, UpwardWalkFrictionScale, DownwardWalkFrictionScale, MinWalkFrictionModify, MaxWalkFrictionModify, UpwardSlideFrictionScale, DownwardSlideFrictionScale, BrakingFrictionStrength, FallingUncontrolledHeight, PhysicsHitReactionBlendInTime, PhysicsHitReactionBlendOutTime, FootstepTraceLength, FootstepTraceWidth, RegenerateDelay, RegenerateHealthPerSecond, UnrealEngineFallDamageScale, RegenerateFromTaserPerSecond, TaserRegenerateDelay, RegenerateFromStunPerSecond
- Defaults (edit): MaxHealth (int)
- State: VelocityMagnitude2D, VelocityMagnitude, VelocityDir2D (Vector), VelocityDir (Vector), FaceRotationTimeLeft, BecameReadyTime, AmountTilUnarmed, GravityModifier, GravityModifierTimer, AnimLockRefCount (byte), RootMotionRefCount (byte), LadderType (byte), ReplicateCustomAnimCount (byte), ReloadCount (byte), NoOfBreathingSounds (byte), AgainstWallLeftHand (Vector), AgainstWallRightHand (Vector), AgainstWallNormal (Vector), MinLookConstraint (Rotator), MaxLookConstraint (Rotator), LegRotationSlowTimer, LegRotation (int), AverageSpeed, ASPollInterval, ASPollTimer, ASPollSlots (int), ASSlotPointer (int), ASDistanceAccum, NewFloorSmooth, SmoothOffset, FootPlacementStoredRotation (Rotator), TargetMeshTranslationZ, SlideStoppedTimeStamp, MoveLocation (Vector), MoveNormal (Vector), MaxWallStepHeight, MoveLedgeLocation (Vector), MoveLedgeNormal (Vector), MoveLedgeResult (int), LedgeFindExtent (Vector), LedgeFindDistance, LedgeFindDepth, IllegalLedgeNormal (Vector), bIllegalLedgeTimer, ActiveMoveTimer (int), RemoteViewYaw (int), EvadeTimer, SpeedSprintEnergy, SoftLockStrength, RollTriggerTime, UncontrolledSlideNormal (Vector), EnterFallingHeight, SlideEffectUpdateTimer, CustomSoundInput, OverrideSynchPosOffset, StreakEffectOverride, StreakEffectDirection, PatchOne (int), PatchTwo (int), PatchThree (int), PhysicsHitReactionBlendTimer, PhysicsHitReactionBlendOut, PhysicsHitReactionScale, LastDamageTaken (int), ArmorBulletsHead, ArmorBulletsBody, ArmorBulletsLegs, ArmorMeleeHead, ArmorMeleeBody, ArmorMeleeLegs, LastFlybyStamp, TimeSinceLastDamage, HealthFrac, TaserDamageLevel, TimeSinceLastTaserDamage, StunDamageLevel, MinTimeBeforeRemovingDeadBody, MaxTimeBeforeRemovingDeadBody, MyPassengerSeatIndex (int), SlideFactor, LastDamage, LastDamageTime, NextDebugPlotTime, LastPlotLocation (Vector), LastJumpLocation (Vector)

### TdPlayerPawn (TdPawn)
- Tuning (config): VertigoEdgeProbingHeight, VertigoEdgeProbingDistance, VertigoEffectThreshold, EdgeCheckMaxSpeed, EdgeCheckDistance, EdgeStopMinHeight, ReverbVolumePollTime, OcclusionDuckLevel, OcclusionDuckFadeTime, MovementStringAllowedGap, PlayerBulletDamageMultiplier, FocusLocationInterpolationSpeed
- State: LastEnemyHitTimeOut, ReverbVolumeTimer, IndoorSoundGroupIndex (int), IndoorMixGroupIndex (int), OutdoorMixGroupIndex (int), MovementStringGapTimer, FocusLocation (Vector), PlayerCameraLocation (Vector), PlayerCameraRotation (Rotator), DebugPlayerGraph (int), LastDebugGraphValue, SimulatedBadFPS (int), LastResetTimeStamp
