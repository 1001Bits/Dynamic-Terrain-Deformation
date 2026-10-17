@{
	# Every step of a deathclaw fighting the god-mode player on open snow, for the foot trace
	# (TraceFeet: NMN_DeformableTerrain_feet.csv and _actors.csv beside the log). Run after
	# plan-perf-2026-10-18-deathclaw.psd1 in the same run (the deathclaw is already there), so
	# the trace's own cost never falls in a measured phase.
	Setup = @('tgm')
	SettleSeconds = 4
	Views = @(
		@{
			Name = 'deathclaw-steps'
			Commands = @('player.setangle x 5')
			Phases = @(
				@{ Name = 'traced'; Settings = @{ WinterMode = 2; TraceFeet = 1; LogStampFeet = 1 }; Wait = 60 }
			)
		}
	)
}
