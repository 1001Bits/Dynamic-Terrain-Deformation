@{
	# The OG look-back view of 20261013-080952-OG-smoke/winter-movement-look-back.png:
	# standing at the end of the winter walk, looking back east along it.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai')
	SettleSeconds = 6
	Views = @(
		@{
			Name = 'edge'
			Commands = @('player.setpos x -81978.97', 'player.setpos y 89494.11', 'player.setpos z 7790',
				'player.setangle z 90', 'player.setangle x 55')
			Phases = @(
				@{ Name = 'winter';     Settings = @{ WinterMode = 1 };                        Wait = 15 }
				@{ Name = 'debug';      Settings = @{ WinterMode = 1; DebugFieldColour = 1 };  Wait = 10 }
				@{ Name = 'no-raise';   Settings = @{ WinterMode = 1; EnableSnowRaise = 0 };   Wait = 15 }
				@{ Name = 'no-normals'; Settings = @{ WinterMode = 1; RecomputeNormals = 0 };  Wait = 15 }
				@{ Name = 'no-cap';     Settings = @{ WinterMode = 1; ShelterMeshCap = 0 };    Wait = 15 }
				@{ Name = 'no-culling'; Settings = @{ WinterMode = 1; EnablePatchCulling = 0 }; Wait = 12 }
				@{ Name = 'normal';     Settings = @{ WinterMode = 0 };                        Wait = 15 }
				@{ Name = 'vanilla';    Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 10 }
			)
		}
		@{
			# The same ground from above, so terrain and static meshes can be told apart.
			Name = 'overhead'
			Commands = @('tcl', 'player.setpos x -81750', 'player.setpos y 89480', 'player.setpos z 8250',
				'player.setangle z 90', 'player.setangle x 85')
			Phases = @(
				@{ Name = 'winter';  Settings = @{ WinterMode = 1 };                        Wait = 15 }
				@{ Name = 'debug';   Settings = @{ WinterMode = 1; DebugFieldColour = 1 };  Wait = 10 }
				@{ Name = 'normal';  Settings = @{ WinterMode = 0 };                        Wait = 15 }
				@{ Name = 'vanilla'; Settings = @{ WinterMode = 0; EnableTessellation = 0 }; Wait = 10 }
			)
		}
	)
}
