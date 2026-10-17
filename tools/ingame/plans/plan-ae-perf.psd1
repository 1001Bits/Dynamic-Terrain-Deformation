@{
	# Run with -Preset perf-on -NoVsync. Fixed noon, clear weather, time scale 0, AI off,
	# god mode; the player stands still. Each phase rewrites the INI (live reload), waits,
	# and records when it was written and when measurement ended: analysis uses only the
	# 10-second profiler windows that start 20 s after the write and end before the capture.
	# 'off' is the perf-off preset (plugin loaded, every feature off).
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			# Grid (-11,15): engine ray picks cost ~0.2 ms each here in the soak.
			Name = 'shelter'
			Commands = @('cow Commonwealth -11 15')
			Phases = @(
				@{ Name = 'budget';      Settings = @{ ShelterTimeBudget = 0.5 }; Wait = 55 }
				@{ Name = 'unbudgeted';  Settings = @{ ShelterTimeBudget = 16 }; Wait = 55; NoCapture = $true }
				@{ Name = 'no-shelter';  Settings = @{ EnableShelter = 0 }; Wait = 55; NoCapture = $true }
				@{ Name = 'budget-2';    Settings = @{ ShelterTimeBudget = 0.5 }; Wait = 55; NoCapture = $true }
			)
		}
		@{
			Name = 'urban'
			Commands = @('player.setangle x 5')
			Phases = @(
				@{ Name = 'off-1';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50 }
				@{ Name = 'on-1';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-1'; Settings = @{ WinterMode = 1 }; Wait = 50 }
				@{ Name = 'winter-2'; Settings = @{ WinterMode = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'on-2';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50; NoCapture = $true }
			)
		}
		@{
			# Sanctuary open ground looking across at the houses.
			Name = 'field'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560',
				'player.setangle z 45', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'off-1';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50 }
				@{ Name = 'on-1';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-1'; Settings = @{ WinterMode = 1 }; Wait = 50 }
				@{ Name = 'winter-2'; Settings = @{ WinterMode = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'on-2';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50; NoCapture = $true }
			)
		}
		@{
			# Sanctuary street, looking east along the road.
			Name = 'street'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7760',
				'player.setangle z 60', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'off-1';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50 }
				@{ Name = 'on-1';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-1'; Settings = @{ WinterMode = 1 }; Wait = 50 }
				@{ Name = 'winter-2'; Settings = @{ WinterMode = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'on-2';     Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0 }; Wait = 50; NoCapture = $true }
			)
		}
	)
}
