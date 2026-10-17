@{
	# Functional check of the features added on 2026-10-14/15, at Sanctuary in winter
	# (WinterMode 2, every ground snow). Run beside a -TestDriver smoke run with the smoke
	# preset; each phase rewrites the INI (live reload), waits, then captures, so every
	# feature is seen on and off from the same spot:
	#  - street:  static meshes (roads, kerbs, slabs) buried by the blanket vs the old cap;
	#  - crater:  a raider and a frag explosion at open ground - craters in the snow and the
	#             ground decals (blood, scorch) fitted to the deformed surface vs vanilla;
	#  - codsworth: his jet melting snow along his path (no scorch on mud or sand).
	# Later features add views here (snow on tall statics, interior shoe prints, corpse
	# mounds, grass).
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tgm')
	SettleSeconds = 6
	Views = @(
		@{
			# Sanctuary street, looking east along the road at the houses.
			Name = 'street'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7760',
				'player.setangle z 60', 'player.setangle x 12')
			Phases = @(
				@{ Name = 'buried'; Settings = @{ WinterMode = 2; SnowCoverLowMeshes = 1; LogSnowCoverage = 1 }; Wait = 35 }
				@{ Name = 'capped'; Settings = @{ WinterMode = 2; SnowCoverLowMeshes = 0; LogSnowCoverage = 1 }; Wait = 25 }
				@{ Name = 'no-winter'; Settings = @{ WinterMode = 0 }; Wait = 20 }
			)
		}
		@{
			# Verified open ground south of the houses: a melee raider beside the player, then
			# a frag explosion that kills it, so the field holds a crater and blood decals.
			# The move settles first: placing in the same batch as setpos put the raider and the
			# blast at the previous view's spot (run 20261015-052649).
			Name = 'crater-move'
			Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7545',
				'player.setangle x 35')
			Phases = @()
		}
		@{
			Name = 'crater'
			Commands = @('player.placeatme 00019562 1', 'player.placeatme 000E574F 1')
			Phases = @(
				@{ Name = 'decals-fitted'; Settings = @{ WinterMode = 2; EnableBloodDecals = 1; LogBloodDecals = 1; LogImpacts = 1; LogCorpses = 1 }; Wait = 25 }
				@{ Name = 'decals-vanilla'; Settings = @{ WinterMode = 2; EnableBloodDecals = 0; LogCorpses = 1 }; Wait = 15 }
			)
		}
		@{
			# Same spot: the raider's body lies in a hollow; rain under WinterMode is snowfall,
			# and a 30-second burial shows the hollow refilling and the mound growing.
			Name = 'burial'
			Commands = @('fw 001CA7E4')
			Phases = @(
				@{ Name = 'snowing'; Settings = @{ WinterMode = 2; EnableBloodDecals = 1; CorpseBurialSeconds = 30; LogCorpses = 1 }; Wait = 45 }
			)
		}
		@{
			Name = 'clear-weather'
			Commands = @('fw 0002B52A')
			Phases = @()
		}
		@{
			# Up the street at the house roofs, porch roofs and car wrecks: snow on the tops of
			# tall statics (the G-buffer pass) on and off. Walls, porch floors under their
			# roofs and the player's weapon must stay bare.
			Name = 'rooftops'
			Commands = @('player.setpos x -81700', 'player.setpos y 89000', 'player.setpos z 7760',
				'player.setangle z 60', 'player.setangle x -6')
			Phases = @(
				@{ Name = 'objects-snow'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; LogSnowCoverage = 1 }; Wait = 30 }
				@{ Name = 'objects-debug'; Settings = @{ WinterMode = 2; SnowOnObjects = 1; SnowOnObjectsDebug = 1; LogSnowCoverage = 1 }; Wait = 20 }
				@{ Name = 'objects-bare'; Settings = @{ WinterMode = 2; SnowOnObjects = 0 }; Wait = 15 }
			)
		}
		@{
			# An interior (the QA test hall): the floor-print pass must find the G-buffer and
			# the camera indoors ('Floor prints: ... ready', no 'stays off'); the driver cannot
			# walk, so prints themselves are left to the play-test.
			Name = 'interior'
			Commands = @('coc QASmoke')
			Phases = @(
				@{ Name = 'floor-pass'; Settings = @{ IndoorPrints = 1; LogStampFeet = 1 }; Wait = 20 }
			)
		}
		@{
			# Back outside for the remaining views.
			Name = 'return'
			Commands = @('coc SanctuaryExt')
			Phases = @()
		}
		@{
			# Codsworth's house yard: the jet melts a channel through the snow.
			Name = 'codsworth'
			Commands = @('player.setpos x -80270', 'player.setpos y 90569', 'player.setpos z 7821',
				'player.setangle x 30')
			Phases = @(
				@{ Name = 'jet'; Settings = @{ WinterMode = 2; LogThrusters = 1 }; Wait = 30 }
			)
		}
	)
}
