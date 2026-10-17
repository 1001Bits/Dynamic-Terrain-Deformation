@{
	# Where snow on objects' 2 ms go, and the deathclaw's real feet, in a run the player watches:
	# Start-ManualSession.ps1 -Runtime AE -Preset perf-on -NoVsync, then Invoke-ManualPhases.ps1.
	# The player sets the scene first (tgm, set timescale to 0, fw 0002B52A,
	# player.placeatme 001423A7 1) and stands still while the deathclaw attacks.
	#  winter  - shipped settings with WinterMode 2 (snow on objects off)
	#  tops    - snow on objects on
	#  nopass  - its probes on but the whitening pass skipped (SnowOnObjectsPass 0)
	#  steps   - the foot trace (TraceFeet) for the deathclaw's real feet, after the timing
	Phases = @(
		@{ Name = 'winter-1'; Settings = @{ WinterMode = 2 }; Wait = 40 }
		@{ Name = 'tops-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 40 }
		@{ Name = 'nopass-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 40 }
		@{ Name = 'nopass-2'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 40 }
		@{ Name = 'tops-2'; Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 40 }
		@{ Name = 'winter-2'; Settings = @{ WinterMode = 2 }; Wait = 40 }
		@{ Name = 'steps'; Settings = @{ WinterMode = 2; TraceFeet = 1; LogStampFeet = 1 }; Wait = 60 }
	)
}
