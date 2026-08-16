#ifndef TETRISU_GAME_SCREEN_H
#define TETRISU_GAME_SCREEN_H

/*
 * game_screen.h - the live gameplay screen for tetrisu.
 *
 * The lobby calls screen_game() once a room's game has started. It renders the
 * board with ncurses and runs the real-time loop - sending the player's inputs
 * and applying the server's pushes - until the round ends or the player quits,
 * then returns to the caller.
 *
 * Takes a Client rather than a raw session_t, for the same reason every other
 * screen does: the screen never touches a socket, so the receive path stays in
 * one place (client_service) instead of being reimplemented here.
 *
 * Assumes ncurses is already initialised (initscr / keypad / noecho) by the
 * entry point's tetrisui_init.
 *
 * Author: Phatsakorn Ukanchanakitti
 */

#include "tetrisu/screens.h" /* ScreenResult, Client */

/*
 * Run the gameplay screen until the round ends, the link dies, or the player
 * presses q. Returns:
 *
 *   SCR_OK            - the round finished; c->last_winner names the winner
 *                       (-1 if the server never said)
 *   SCR_BACK          - the player left the game screen
 *   SCR_DISCONNECTED  - the transport died
 *
 * Never returns while the room is still playing, so the caller can go straight
 * back to the lobby.
 */
ScreenResult screen_game(Client *c);

#endif /* TETRISU_GAME_SCREEN_H */
