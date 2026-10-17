@{
	# Short regression on the final build: winter at Sanctuary with shelter logging, so the
	# game-thread probe batches and the blanket can be checked on each runtime.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai')
	SettleSeconds = 6
	Views = @(
		@{ Name = 'field'; Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560'); Phases = @(
			@{ Name = 'winter'; Settings = @{ WinterMode = 1; LogSnowCoverage = 1 }; Wait = 35 }
			@{ Name = 'normal'; Settings = @{ WinterMode = 0; LogSnowCoverage = 1 }; Wait = 15 } ) }
	)
}
