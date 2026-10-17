@{
	# Where snow on objects' frame time goes, on AE (no ENB, no other plugins): a still scene (AI
	# off, the player standing) so only the setting changes. Run with Invoke-SmokeTest.ps1
	# -Runtime AE -TestDriver -Preset perf-on -NoVsync. In the order winter, tops, nopass, winter,
	# nopass, tops, winter, so drift evens out.
	#  winter  - WinterMode 2, snow on objects off (shipped)
	#  tops    - snow on objects on
	#  nopass  - its probes and grid upload on, the whitening pass skipped (SnowOnObjectsPass 0)
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm')
	SettleSeconds = 8
	Views = @(
		@{
			# Sanctuary open ground looking across at the houses, as the 15 October field plan.
			Name = 'tops'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7560',
				'player.setangle z 45', 'player.setangle x 5')
			Phases = @(
				@{ Name = 'winter-1'; Settings = @{ WinterMode = 2 }; Wait = 35 }
				@{ Name = 'tops-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 35 }
				@{ Name = 'nopass-1'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 35; NoCapture = $true }
				@{ Name = 'winter-2'; Settings = @{ WinterMode = 2 }; Wait = 35; NoCapture = $true }
				@{ Name = 'nopass-2'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; SnowOnObjectsPass = 0 }; Wait = 35; NoCapture = $true }
				@{ Name = 'tops-2'; Settings = @{ WinterMode = 2; SnowOnObjects = 1 }; Wait = 35; NoCapture = $true }
				@{ Name = 'winter-3'; Settings = @{ WinterMode = 2 }; Wait = 35; NoCapture = $true }
			)
		}
	)
}
