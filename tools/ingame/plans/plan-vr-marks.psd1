@{
	# VR: the null headset sits at the tracking origin, so the mirror camera is at the player's
	# feet looking level. Place a crater, fire barrel and rifle on the verified open ground at
	# (-83300, 89500), then hover 80 units up, 300 units south, looking north at them.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tgm', 'tcl')
	SettleSeconds = 4
	Views = @(
		@{
			Name = 'before'
			Commands = @('player.setpos x -83200', 'player.setpos y 89200', 'player.setpos z 7620', 'player.setangle z 0')
			Phases = @(
				@{ Name = 'on'; Settings = @{ WinterMode = 0 }; Wait = 8 }
			)
		}
		@{
			Name = 'place-crater'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7545', 'player.placeatme 000E574F 1')
			Phases = @()
		}
		@{
			Name = 'place-barrel'
			Commands = @('player.setpos x -83080', 'player.setpos y 89520', 'player.setpos z 7545', 'player.placeatme 00048280 1')
			Phases = @()
		}
		@{
			Name = 'place-rifle'
			Commands = @('player.setpos x -83190', 'player.setpos y 89420', 'player.setpos z 7545', 'player.placeatme 0004F46A 1')
			Phases = @()
		}
		@{
			# Status only: VR's cell water height over the Sanctuary river (ESM XCLW 7250).
			Name = 'river-status'
			Commands = @('player.setpos x -84300', 'player.setpos y 90700', 'player.setpos z 7700')
			Phases = @()
		}
		@{
			Name = 'look'
			Commands = @('player.setpos x -83200', 'player.setpos y 89200', 'player.setpos z 7620', 'player.setangle z 0')
			Phases = @(
				@{ Name = 'on';      Settings = @{ WinterMode = 0 };                        Wait = 6 }
				@{ Name = 'debug';   Settings = @{ WinterMode = 0; DebugFieldColour = 1 };  Wait = 6 }
				@{ Name = 'vanilla'; Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 6 }
				@{ Name = 'winter';  Settings = @{ WinterMode = 1 };                        Wait = 20 }
				@{ Name = 'winter-debug'; Settings = @{ WinterMode = 1; DebugFieldColour = 1 }; Wait = 6 }
			)
		}
	)
}
