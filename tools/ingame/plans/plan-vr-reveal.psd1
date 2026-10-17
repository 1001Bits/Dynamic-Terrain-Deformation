@{
	# VR layer classification through the recorded texture sets. Two craters on open ground
	# near (-83300, 89500); the headset faces yaw 45 from (-83200, 89200), so the second one
	# is placed ahead of it. In winter the revealed layer's strength is SnowRevealStrength:
	# its 0/1 phases differ only if VR layers are classified.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm', 'tcl')
	SettleSeconds = 4
	Views = @(
		@{
			Name = 'place-crater-1'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7545', 'player.placeatme 000E574F 1')
			Phases = @()
		}
		@{
			Name = 'place-crater-2'
			Commands = @('player.setpos x -83030', 'player.setpos y 89380', 'player.setpos z 7575', 'player.placeatme 000E574F 1')
			Phases = @()
		}
		@{
			Name = 'look'
			Commands = @('player.setpos x -83200', 'player.setpos y 89200', 'player.setpos z 7620')
			Phases = @(
				@{ Name = 'normal';          Settings = @{ WinterMode = 0 }; Wait = 8 }
				@{ Name = 'winter';          Settings = @{ WinterMode = 1 }; Wait = 20 }
				@{ Name = 'winter-reveal0';  Settings = @{ WinterMode = 1; SnowRevealStrength = 0 }; Wait = 8 }
				@{ Name = 'winter-reveal1';  Settings = @{ WinterMode = 1; SnowRevealStrength = 1 }; Wait = 8 }
				@{ Name = 'winter-nomat';    Settings = @{ WinterMode = 1; EnableSurfaceMaterial = 0 }; Wait = 8 }
				@{ Name = 'normal-nomat';    Settings = @{ WinterMode = 0; EnableSurfaceMaterial = 0 }; Wait = 8 }
				@{ Name = 'normal-again';    Settings = @{ WinterMode = 0 }; Wait = 8 }
			)
		}
	)
}
