@{
	# Where the shelter's time goes with and without the pile of 30 rifles and 10 fire
	# barrels: the 5-second "Shelter transitions" lines (LogSnowCoverage) split the first
	# pick of each frame from the other picks and the rest of the update. Run with perf-on.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 6
	Views = @(
		@{ Name = 'clean'; Commands = @('cow Commonwealth -11 15'); Phases = @(
			@{ Name = 'log'; Settings = @{ LogSnowCoverage = 1 }; Wait = 40; NoCapture = $true } ) }
		@{ Name = 'pile'; Commands = @('player.placeatme 0004F46A 30', 'player.placeatme 00048280 10'); Phases = @(
			@{ Name = 'log'; Settings = @{ LogSnowCoverage = 1 }; Wait = 50 }
			@{ Name = 'log-unbudgeted'; Settings = @{ LogSnowCoverage = 1; ShelterTimeBudget = 16 }; Wait = 40; NoCapture = $true } ) }
	)
}
