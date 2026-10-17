@{
	# Cost of the release build with a deathclaw fighting in deep snow. Run on AE (no other
	# F4SE plugins there) with -Preset perf-on -NoVsync -WorldSeconds 1500. Fixed noon, clear
	# weather, time scale 0, god mode; AI stays on, so the deathclaw keeps attacking the player,
	# who stands still, in every phase: its own animation and AI cost the same with the plugin
	# off. Each phase rewrites the INI (live reload); analysis (analyze-phases.py /
	# compare-modes.py) uses only the 10-second profiler windows that start 20 s after the
	# write. The order runs forward and back so drift (heat, the fight moving about) evens out.
	#  off      - plugin loaded, every feature off (the baseline)
	#  winter   - shipped settings with WinterMode 2 (all ground snow): deep snow, the
	#             deathclaw's heavy prints, rims and kicked-up snow; snow on objects off as shipped
	#  tops     - winter with snow on objects on, to measure that pass on its own
	#  scoped   - winter with ProfileDrawScopes, timing the tessellated terrain draws
	#  vanilla  - the same draws untessellated and timed the same way (field still updating),
	#             so scoped minus vanilla is the terrain's added GPU cost
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			# Sanctuary open ground, as the 15 October field plan. The move settles before
			# anything is placed: placing in the same batch as setpos used the previous spot.
			Name = 'deathclaw-move'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560',
				'player.setangle z 45', 'player.setangle x 5')
			Phases = @()
		}
		@{
			# EncDeathclaw02 (Fallout4.esm 001423A7), a plain deathclaw of fixed level.
			Name = 'deathclaw'
			Commands = @('player.placeatme 001423A7 1')
			Phases = @(
				@{ Name = 'off-1';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50 }
				@{ Name = 'winter-1';  Settings = @{ WinterMode = 2 }; Wait = 50 }
				@{ Name = 'tops-1';    Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'scoped-1';  Settings = @{ WinterMode = 2; ProfileDrawScopes = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'vanilla-1'; Settings = @{ WinterMode = 2; ProfileDrawScopes = 1; EnableTessellation = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'vanilla-2'; Settings = @{ WinterMode = 2; ProfileDrawScopes = 1; EnableTessellation = 0 }; Wait = 50; NoCapture = $true }
				@{ Name = 'scoped-2';  Settings = @{ WinterMode = 2; ProfileDrawScopes = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'tops-2';    Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 50; NoCapture = $true }
				@{ Name = 'winter-2';  Settings = @{ WinterMode = 2 }; Wait = 50; NoCapture = $true }
				@{ Name = 'off-2';     Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 50; NoCapture = $true }
			)
		}
	)
}
