@{
	# OG with Winter Redone (white snow, detected by WinterMode -1) and ENB, a deathclaw attacking
	# the god-mode player: where snow on objects' 2 ms go, and the deathclaw's real feet. Run with
	# Invoke-SmokeTest.ps1 -Runtime OG -TestDriver -Preset perf-on -NoVsync -Mods WinterRedone.
	#  off     - plugin loaded, every feature off (the baseline)
	#  on      - shipped settings (snow on objects off)
	#  tops    - snow on objects on
	#  nopass  - its probes on but the whitening pass skipped (SnowOnObjectsPass 0)
	#  steps   - the foot trace, after the timing so its cost falls in no measured phase
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			Name = 'og-move'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560',
				'player.setangle z 45', 'player.setangle x 5')
			Phases = @()
		}
		@{
			# EncDeathclaw02 (Fallout4.esm 001423A7).
			Name = 'og'
			Commands = @('player.placeatme 001423A7 1')
			Phases = @(
				@{ Name = 'off-1';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 40 }
				@{ Name = 'on-1';     Settings = @{ SnowOnObjects = 0 }; Wait = 40 }
				@{ Name = 'tops-1';   Settings = @{ SnowOnObjects = 1 }; Wait = 40; NoCapture = $true }
				@{ Name = 'nopass-1'; Settings = @{ SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 40; NoCapture = $true }
				@{ Name = 'nopass-2'; Settings = @{ SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 40; NoCapture = $true }
				@{ Name = 'tops-2';   Settings = @{ SnowOnObjects = 1 }; Wait = 40; NoCapture = $true }
				@{ Name = 'on-2';     Settings = @{ SnowOnObjects = 0 }; Wait = 40; NoCapture = $true }
				@{ Name = 'off-2';    Settings = @{ EnableTessellation = 0; UseClipmap = 0; EnableSnowRaise = 0; EnableShelter = 0; EnableWeather = 0; SnowSparkle = 0; SnowOnObjects = 0; EnableBloodDecals = 0; IndoorPrints = 0; CorpseMarks = 0; CorpseBurial = 0; EnableExplosionImpacts = 0; EnableProjectileImpacts = 0 }; Wait = 40; NoCapture = $true }
				@{ Name = 'steps';    Settings = @{ TraceFeet = 1; LogStampFeet = 1 }; Wait = 60 }
			)
		}
	)
}
