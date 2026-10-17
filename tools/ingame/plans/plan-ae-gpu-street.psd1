@{
	# Run with -Preset perf-on -NoVsync. GPU time of the near-terrain G-buffer draws, vanilla
	# against tessellated: with ProfileDrawScopes on and only EnableTessellation=0 the same
	# draws are selected and timed untouched (the field, window and coverage keep running).
	# Vanilla terrain's depth pre-pass draws are not timed, so the difference is an upper
	# bound on the added GPU cost. Phases run in mirrored order to cancel slow drift.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			# Sanctuary street, looking east along the road.
			Name = 'street'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7760', 'player.setangle z 60', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'on-1'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 0 }; Wait = 42 }
				@{ Name = 'vanilla-1'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 0; EnableTessellation = 0 }; Wait = 42 }
				@{ Name = 'winter-1'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 1 }; Wait = 42 }
				@{ Name = 'winter-vanilla-1'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 1; EnableTessellation = 0 }; Wait = 42 }
				@{ Name = 'winter-vanilla-2'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 1; EnableTessellation = 0 }; Wait = 42; NoCapture = $true }
				@{ Name = 'winter-2'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 1 }; Wait = 42; NoCapture = $true }
				@{ Name = 'vanilla-2'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 0; EnableTessellation = 0 }; Wait = 42; NoCapture = $true }
				@{ Name = 'on-2'; Settings = @{ ProfileDrawScopes = 1; WinterMode = 0 }; Wait = 42; NoCapture = $true }
			)
		}
	)
}
