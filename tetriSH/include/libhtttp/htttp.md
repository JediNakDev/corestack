## List of protocal
*possible result is merely for note and out of scope of this implementation
#### 1. JOIN(int roomid)
Client ask to join room by roomid

roomid->0 ask for new room
roomid->positive (do not exceed 255) int ask to join that room/create new room with that id

possible result:
1. Successfully create and join room
2. Successfully join room
3. Cannot join room (room full)

#### 2. LEAVE()
Client ask to leave the current room

possible result:
1. Successfully leave room
2. Fail because client is not in any room

#### 3. START()
Client ask to initiate and start game for all clients with the same room number

possible result:
1. Successfully start game
2. Fail because client is not room owner or client is not in any room

#### 4. MOVE(int dir)
Client ask to do move() in the game
dir->0 move left
dir->1 move right


#### 5. ROTATE(int dir)
Client ask to do rotate() in the game
dir->0 rotate left
dir->1 rotate right

#### 5. DROP(int type)
Client ask to do drop() in the game
type->0 soft drop
type->1 hard drop

#### 6. HOLD()
Client ask to do hold() in the game

#### 7. UPD_GAME(GameState state)
Server ask client to update their whole game state(overwritten by a new one)

#### 8. UPD_SESSION(SessionState state)
Server ask client to update their whole session state(overwritten by a new one)

