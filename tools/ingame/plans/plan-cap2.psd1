@{
	# Static-only cap ray, 3-unit clearance, erosion before smoothing, tolerance 1 by default.
	# Each winter phase follows a non-winter one, so no earlier blanket carries over.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tcl')
	SettleSeconds = 6
	Views = @(
		@{
			Name = 'overhead'
			Commands = @('player.setpos x -81750', 'player.setpos y 89480', 'player.setpos z 8250',
				'player.setangle z 90', 'player.setangle x 85')
			Phases = @(
				@{ Name = 'vanilla';      Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 12 }
				@{ Name = 'winter';       Settings = @{ WinterMode = 1 }; Wait = 25 }
				@{ Name = 'normal';       Settings = @{ WinterMode = 0 }; Wait = 12 }
				@{ Name = 'winter-nocap'; Settings = @{ WinterMode = 1; ShelterMeshCap = 0 }; Wait = 25 }
				@{ Name = 'winter-again'; Settings = @{ WinterMode = 1 }; Wait = 25 }
			)
		}
		@{
			# Standing on the concrete, looking east along it at eye height.
			Name = 'walk'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7840',
				'player.setangle z 60', 'player.setangle x 25')
			Phases = @(
				@{ Name = 'vanilla';      Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 12 }
				@{ Name = 'winter';       Settings = @{ WinterMode = 1 }; Wait = 25 }
				@{ Name = 'winter-nocap'; Settings = @{ WinterMode = 1; ShelterMeshCap = 0 }; Wait = 25 }
			)
		}
		@{
			# Open ground with rocks and debris, to see what the cap does around small statics.
			Name = 'open'
			Commands = @('player.setpos x -83100', 'player.setpos y 89400', 'player.setpos z 8000',
				'player.setangle z 0', 'player.setangle x 85')
			Phases = @(
				@{ Name = 'vanilla';      Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 12 }
				@{ Name = 'winter';       Settings = @{ WinterMode = 1 }; Wait = 25 }
				@{ Name = 'normal';       Settings = @{ WinterMode = 0 }; Wait = 12 }
				@{ Name = 'winter-nocap'; Settings = @{ WinterMode = 1; ShelterMeshCap = 0 }; Wait = 25 }
			)
		}
	)
}
