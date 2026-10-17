@{
	# Vertical picks across the walkway south of the house at (-81750, 89480), and open ground.
	Setup = @('set timescale to 0', 'set gamehour to 12', 'fw 0002B52A', 'tai', 'tcl')
	SettleSeconds = 2
	Views = @(
		@{ Name = 'p-81850_89350'; Commands = @('player.setpos x -81850', 'player.setpos y 89350', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81850_89250'; Commands = @('player.setpos x -81850', 'player.setpos y 89250', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81850_89150'; Commands = @('player.setpos x -81850', 'player.setpos y 89150', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81850_89050'; Commands = @('player.setpos x -81850', 'player.setpos y 89050', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81850_88950'; Commands = @('player.setpos x -81850', 'player.setpos y 88950', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81750_89350'; Commands = @('player.setpos x -81750', 'player.setpos y 89350', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81750_89250'; Commands = @('player.setpos x -81750', 'player.setpos y 89250', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81750_89150'; Commands = @('player.setpos x -81750', 'player.setpos y 89150', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81750_89050'; Commands = @('player.setpos x -81750', 'player.setpos y 89050', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81750_88950'; Commands = @('player.setpos x -81750', 'player.setpos y 88950', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81650_89350'; Commands = @('player.setpos x -81650', 'player.setpos y 89350', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81650_89250'; Commands = @('player.setpos x -81650', 'player.setpos y 89250', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81650_89150'; Commands = @('player.setpos x -81650', 'player.setpos y 89150', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81650_89050'; Commands = @('player.setpos x -81650', 'player.setpos y 89050', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81650_88950'; Commands = @('player.setpos x -81650', 'player.setpos y 88950', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81550_89350'; Commands = @('player.setpos x -81550', 'player.setpos y 89350', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81550_89250'; Commands = @('player.setpos x -81550', 'player.setpos y 89250', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81550_89150'; Commands = @('player.setpos x -81550', 'player.setpos y 89150', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81550_89050'; Commands = @('player.setpos x -81550', 'player.setpos y 89050', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'p-81550_88950'; Commands = @('player.setpos x -81550', 'player.setpos y 88950', 'player.setpos z 7950'); Phases = @() }
		@{ Name = 'open-83300_89500'; Commands = @('player.setpos x -83300', 'player.setpos y 89500', 'player.setpos z 7700'); Phases = @() }
		@{ Name = 'open-83000_89300'; Commands = @('player.setpos x -83000', 'player.setpos y 89300', 'player.setpos z 7700'); Phases = @() }
		@{ Name = 'open-82500_89800'; Commands = @('player.setpos x -82500', 'player.setpos y 89800', 'player.setpos z 7700'); Phases = @() }
	)
}
