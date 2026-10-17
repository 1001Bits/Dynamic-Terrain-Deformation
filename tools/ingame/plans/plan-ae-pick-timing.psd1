@{
	# How long one engine pick (TESObjectCELL::Pick, as the shelter casts it) takes, timed by
	# the test driver's @status at the player's position: before the pile of 30 rifles and
	# 10 fire barrels, beside it, 1500 and 3000 units away, and back. Run with -Preset perf-on.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 6
	Views = @(
		@{ Name = 'clean-near'; Commands = @('cow Commonwealth -11 15'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 20; NoCapture = $true } ) }
		@{ Name = 'clean-far'; Commands = @('player.setpos x -42706'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 15; NoCapture = $true } ) }
		@{ Name = 'pile-near'; Commands = @('player.setpos x -44206', 'player.placeatme 0004F46A 30', 'player.placeatme 00048280 10'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 20 } ) }
		@{ Name = 'pile-far'; Commands = @('player.setpos x -42706'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 15; NoCapture = $true } ) }
		@{ Name = 'pile-far2'; Commands = @('player.setpos x -41206'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 15; NoCapture = $true } ) }
		@{ Name = 'pile-near-again'; Commands = @('player.setpos x -44206'); Phases = @(
			@{ Name = 'wait'; Settings = @{ WinterMode = 0 }; Wait = 15; NoCapture = $true } ) }
		@{ Name = 'pile-near-noshelter'; Commands = @('set gamehour to 12'); Phases = @(
			@{ Name = 'wait'; Settings = @{ EnableShelter = 0 }; Wait = 15; NoCapture = $true } ) }
	)
}
