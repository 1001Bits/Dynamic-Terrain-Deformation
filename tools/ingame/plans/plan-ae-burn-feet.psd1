@{
	# Codsworth's jet burn and planted footprints: an overhead view of the yard of his house,
	# where he hovers, with a melee raider placed on the ground beside him to fight, so he
	# flies about and the raider runs. The camera stays within the plugin's actor range
	# (576 units, measured in 3D from the player), so about 330 units up. tgm keeps the
	# player alive; tcl holds the camera in the air. AI stays on.
	Setup = @('set gamehour to 12', 'fw 0002B52A', 'tgm', 'tcl')
	SettleSeconds = 6
	Views = @(
		@{
			Name = 'yard'
			Commands = @('player.setpos x -80270', 'player.setpos y 90450', 'player.setpos z 8150',
				'player.setangle z 0', 'player.setangle x 85')
			Phases = @(
				@{ Name = 'calm'; Settings = @{ LogThrusters = 1; LogStampFeet = 1 }; Wait = 20 }
			)
		}
		@{
			# Down on open ground south of the house, to place the raider there.
			Name = 'spawn'
			Commands = @('player.setpos x -80200', 'player.setpos y 90150', 'player.setpos z 7860',
				'player.placeatme 00019562 1')
			Phases = @(
				@{ Name = 'placed'; Settings = @{ LogThrusters = 1 }; Wait = 3; NoCapture = $true }
			)
		}
		@{
			Name = 'fight'
			Commands = @('player.setpos x -80270', 'player.setpos y 90350', 'player.setpos z 8150',
				'player.setangle z 0', 'player.setangle x 85')
			Phases = @(
				@{ Name = 'early'; Settings = @{ LogThrusters = 1; LogStampFeet = 1 }; Wait = 25 }
				@{ Name = 'late'; Settings = @{ LogThrusters = 1; LogStampFeet = 1 }; Wait = 30 }
				@{ Name = 'noscorch'; Settings = @{ EnableScorch = 0 }; Wait = 10 }
			)
		}
	)
}
