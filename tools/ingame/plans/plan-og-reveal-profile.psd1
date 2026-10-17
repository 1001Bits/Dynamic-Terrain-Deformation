@{
	# Reveal under snow, made visible: a probe surface profile types CommonwealthDefault01_d,
	# the dominant ground at both craters, as snow. A crater then blends toward a different
	# layer (ForestFloor/RubbleRock), so BlendStrength 0 and 1 differ inside the craters.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm', 'tcl')
	SettleSeconds = 4
	Views = @(
		@{ Name = 'place-crater-1'; Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7545', 'player.placeatme 000E574F 1'); Phases = @() }
		@{ Name = 'place-crater-2'; Commands = @('player.setpos x -83030', 'player.setpos y 89380', 'player.setpos z 7592', 'player.placeatme 000E574F 1'); Phases = @() }
		@{
			Name = 'look'
			Commands = @('player.setpos x -83200', 'player.setpos y 89200', 'player.setpos z 7681', 'player.setangle z 45', 'player.setangle x 25')
			Phases = @(
				@{ Name = 'base';           Settings = @{ WinterMode = 0 }; Wait = 8 }
				@{ Name = 'profile';        Settings = @{ WinterMode = 0 }; Profile = "[ProbeSnow]`r`nTextures = commonwealthdefault01_d`r`nType = snow"; Wait = 15 }
				@{ Name = 'profile-blend0'; Settings = @{ BlendStrength = 0 }; Profile = "[ProbeSnow]`r`nTextures = commonwealthdefault01_d`r`nType = snow"; Wait = 8 }
				@{ Name = 'profile-blend1'; Settings = @{ BlendStrength = 1 }; Profile = "[ProbeSnow]`r`nTextures = commonwealthdefault01_d`r`nType = snow"; Wait = 8 }
				@{ Name = 'profile-debug';  Settings = @{ DebugFieldColour = 1 }; Profile = "[ProbeSnow]`r`nTextures = commonwealthdefault01_d`r`nType = snow"; Wait = 8 }
				@{ Name = 'base-again';     Settings = @{ WinterMode = 0 }; Wait = 12 }
			)
		}
	)
}
