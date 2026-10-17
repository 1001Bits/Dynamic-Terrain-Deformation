@{
	# The soak's expensive shelter cost grew with every visit while fire barrels and rifles
	# piled up at the same spots. Reproduce that pile at grid (-11,15) and compare the shelter
	# time budget with no budget. Run with -Preset perf-on -NoVsync.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			Name = 'clean'
			Commands = @('cow Commonwealth -11 15')
			Phases = @(
				@{ Name = 'budget';     Settings = @{ ShelterTimeBudget = 0.5 }; Wait = 45 }
				@{ Name = 'unbudgeted'; Settings = @{ ShelterTimeBudget = 16 }; Wait = 45; NoCapture = $true }
			)
		}
		@{
			Name = 'pile'
			Commands = @('player.placeatme 0004F46A 30', 'player.placeatme 00048280 10')
			Phases = @(
				@{ Name = 'budget';       Settings = @{ ShelterTimeBudget = 0.5 }; Wait = 50 }
				@{ Name = 'unbudgeted';   Settings = @{ ShelterTimeBudget = 16 }; Wait = 50; NoCapture = $true }
				@{ Name = 'no-shelter';   Settings = @{ EnableShelter = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'budget-2';     Settings = @{ ShelterTimeBudget = 0.5 }; Wait = 50; NoCapture = $true }
				@{ Name = 'unbudgeted-2'; Settings = @{ ShelterTimeBudget = 16 }; Wait = 50; NoCapture = $true }
			)
		}
	)
}
