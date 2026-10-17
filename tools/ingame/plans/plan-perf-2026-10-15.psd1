@{
	# Performance of the build with the 2026-10-14/15 features. Run with -Preset perf-on
	# -NoVsync. Fixed noon, clear weather, time scale 0, AI off, god mode; the player stands
	# still. Each phase rewrites the INI (live reload), waits, and records when it was
	# written and when measurement ended: analysis (analyze-phases.py / compare-modes.py)
	# uses only the 10-second profiler windows that start 20 s after the write.
	#  off      - plugin loaded, every feature off (the baseline)
	#  on       - shipped settings, no winter
	#  winter   - WinterMode 2 with everything new: buried meshes, snow on objects, ground
	#             decals fitted
	#  no-tops  - winter without the snow-on-objects pass, to isolate its cost
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			# Sanctuary open ground looking across at the houses.
			Name = 'field'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560',
				'player.setangle z 45', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'off-1';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50 }
				@{ Name = 'on-1';      Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-1';  Settings = @{ WinterMode = 2 }; Wait = 50 }
				@{ Name = 'no-tops-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-2';  Settings = @{ WinterMode = 2 }; Wait = 50; NoCapture = $true }
				@{ Name = 'on-2';      Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50; NoCapture = $true }
			)
		}
		@{
			# Sanctuary street, looking east along the road at roofs and wrecks.
			Name = 'street'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7760',
				'player.setangle z 60', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'off-1';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50 }
				@{ Name = 'on-1';      Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-1';  Settings = @{ WinterMode = 2 }; Wait = 50 }
				@{ Name = 'no-tops-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-2';  Settings = @{ WinterMode = 2 }; Wait = 50; NoCapture = $true }
				@{ Name = 'on-2';      Settings = @{ WinterMode = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50; NoCapture = $true }
			)
		}
	)
}
