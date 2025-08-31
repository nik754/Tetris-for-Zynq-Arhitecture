# Info
-The hardware design was done by building the block diagram using the IP Integrator in Vivado. Then the software implementation was done using the Vitis platform and the C programming language.

-The game is fully functional including all the controls and basics of the base game, meaning the tetrominoes can be manipulated in the playfield and the game will clear any full lines the player completes. Also after every cleared line the score of the player with the speed of the game will be increased.

-Controlling the game is done using the keyboard of the pc connected to the board more exactly using the keys W (rotate), A (move left), S (fast descend), D(move right).

-The graphics of the game are output using a VGA interface connected to an external monitor.

# Notes
-The hardware design can be found in the .xpr format for edit through the Vivado platform and also in the .xsa format ready to be opened in Vitis for software implementation.

-The project was done specifically for the Zybo-Z7 board but is compatible with any board based on the Zynq arhitecture that has the same componentes used from this specific board.
