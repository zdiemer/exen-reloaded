# SMS: the In-Fusio SMS protocol and the mock server

44 of the 47 games in the corpus (all but Crash Bandicoot, Panko and Spyro) talk to In-Fusio's
SMS server: they submit scores to a world ranking, ask for their ranking, vote in "Your
opinion", pay per use (continue, extra content), unlock or download content and restore what
they bought. This page documents the protocol, from the simulator (`gensimu.exe`) and from the
games' bytecode, and what the port's mock server answers. The natives themselves (exen.Sms
89-100, Gamelet.sendSms 78) are in [NATIVES.md](NATIVES.md) §2.2/§2.4; the code is
`src/core/sms.c` (natives) and `src/core/smsserver.c` (codec and mock server).

Confidence: **H** traced in the disassembly and checked by running games in the port, **M**
traced but not exercised, **L** inferred.

## 1. What the simulator does [H]

The simulator does not answer SMS itself. It forwards them to a separate In-Fusio PC tool (not
shipped) and injects that tool's answers. `simulator.ini`:

```
[SmsServerConfig]
Port=5555            ; the PC tool's port (default in the code 5555)
IP=127.0.0.1         ; the PC tool (default in the code 192.168.0.46)
Port_bind=5582       ; where the simulator listens for answers (default 5556)
nb_phone=+33655555555 ; this handset's number (default +33689291150)
Activate=1           ; 0 = no server: every send is reported as successful
DefaultFileID=128
```

### 1.1 Sending (Gamelet.sendSms, native 78, 0x4250c7)

1. The price index (`Sms.m_type`, set with `Sms.setPrice`, clamped to 0..4) goes into the
   buffer's last word (`+0x8c`). If the price-table entry has no number, or the SIM's network
   is not the home network, the VM stops (exit state 3); see NATIVES.md.
2. `0x4218d3(buf, 1)` finishes a **copy** of the 0x94-byte native buffer:
   - the 11-bit length of the container block `0x60` (bit 136) = bit position - 0x93;
   - `[0x456cec]` = `0x42279a` appends the **platform statistics block `0x41`**: for each
     installed gamelet not yet reported (record flag `+0x1d4` bit 1), while the position is below
     0x3fc: 16-bit gamelet id, 16-bit play count (`+0x1d6`), 16-bit play time (`+0x1c0`/1000),
     32-bit date of the last report, 32-bit date now (84 bits each); then it marks them
     reported. Nothing is added if the message is already past bit 1001.
   - an end byte `0xff` (`0x421982`);
   - a checksum (`0x421aa0`): payload byte 0x87 = sum of payload bytes 0..0x86 (mod 256).
3. `0x406514` calls `EXManufSmsSend(&cfg[0x1c], number, buf)` (0x434efe). `number` is the
   price-table entry's short number (`cfg+0x190+0x1c*m_type`), logged as the TPOA.
4. `0x434f76`: if `Activate=0` it returns 1 (sent) without any network. Otherwise it opens a TCP
   connection to `IP:Port` and sends:
   - 17 bytes: `nb_phone` (13 bytes, NUL padded) followed by `Port_bind` (4 ASCII digits), so
     the tool knows where to answer;
   - it receives 1 byte: 0 = accepted, anything else = refused;
   - 140 bytes: the whole buffer from its start, i.e. `"EXEN"` and the 136-byte payload.
   Any socket error or a refusal gives 2.
5. The result is posted as ExEn event `0x101` with wParam 1 (sent) or 2 (failed). The core's
   handler (`0x402fbf`) maps 1 to Bootstrap.smsSent(Gamelet.SMS_SENT_SUCCESSFULLY = 1) and 2 to
   SMS_SENT_UNSUCCESSFULLY (= 0) (`Gamelet.<clinit>`, static slots 1 and 2).

### 1.2 Receiving

- At start-up (`0x4351f8`) the simulator binds `Port_bind` and starts a thread (`0x435396`)
  that blocks in `accept`, reads 0x8c (140) bytes per connection, closes it and queues the
  message (`0x43544d`: a ring of 10 entries of 0x8c bytes + a due time) with a **500 ms** delay
  (`[0x458090]` = 500).
- The idle hook `0x438840` dequeues a due message (`0x434eaf`/`0x434e50`) and posts ExEn event
  `0x100`, handled by `0x41509c` ("onSmsReceived" in the log):
  1. `0x42186f` copies the 4-byte magic and 0x88 payload bytes; the position starts at 0 (an MT
     message has **no header**).
  2. `0x421aba` checks the checksum (payload byte 0x87 = sum of bytes 0..0x86); a mismatch drops
     the message.
  3. It walks the top-level blocks (`id:8, len:11, data`) until the id `0xff` or bit 0x440. Each
     block's data is unpacked into a 136-byte buffer filled with `0xff`: in 8-bit units, or 7-bit
     units for ids 7, 8 and 9; the remaining bits go left-aligned into a last byte.
  4. The id's top 3 bits select a table, the low 5 bits the handler: class 0 (ids 0-0x19,
     `0x456c7c`) are platform commands (0 delete a gamelet, 4 set the registration bytes at
     `cfg+0x22`, 0x18 GPRS parameters, ...), class 1 (`0x20`, `0x21`) and class 3 (`0x60`,
     `0x61`).
  5. **`0x60` is the gamelet message** (`0x422640`): if the running gamelet matches, its
     onSmsReceived callback gets the unpacked buffer, i.e. the **contents of the 0x60 block**;
     otherwise the buffer is stored in the gamelet's record (`+0x14c`, 0x88 bytes) for later.
  6. The EEPROM is written back (`0x402d7f`).
- The VM side is `Bootstrap.smsReceived([B)V` (hash 0x6f6c0565), which calls
  `Gamelet.onSmsReceived(byte[])`. Games wrap the bytes with `new Sms(byte[])`
  (`Sms.createSms([B)`, native 91: copies 0x88 bytes, clears the magic, position 0) and read the
  blocks with getIdBlock / getLengthBlock / readBits.

So the "EXEN" magic only travels on the wire; the game never sees it in a received message.

## 2. Message layout [H]

The payload is bit-addressed, MSB first (`0x413071` / `0x412ea0`). Sms.createSms() writes:

| bits | width | field |
|---|---|---|
| 0 | 8 | `cfg+5` (version, 0x85 in the shipped EEPROM) |
| 8 | 8 | `cfg+7` (0x80) |
| 16 | 16 | gamelet record id (0 when the game has none) |
| 32 | 4 x 8 | registration bytes `cfg+0x1e..0x21` |
| 64 | 12 | MCC (nibble-swapped) |
| 76 | 8 | MNC (nibble-swapped) |
| 84 | 8 | language id (English 1, French 3, ...) |
| 92 | 16 | `cfg+0x164` |
| 108 | 20 | reserved (zero) |
| 128 | 8 | `0x60`: the container block for the game's blocks |
| 136 | 11 | its length in bits (filled in at send time) |
| 147 | | the game's blocks: `id:8, len:11, data:len` each |

`Sms.createBlock(id)` writes the id and skips the length; `endBlock()` writes the length;
`writeShortBlock` / `writeIntBlock` write 16 / 32 bits as 8-bit pieces; names are 7-bit
characters. A received message (MT) is just a list of blocks from bit 0, followed by `0xff`.

## 3. Block ids

The framework's `exen.Sms` has the constants (`<clinit>`); the games' library adds its own.

| id | name | direction | answer | confidence |
|---|---|---|---|---|
| 0 | MT_ERROR | MT | | games ignore it (they keep waiting) — H |
| 1 | MO_UNLOCK_ITEM | MO | 2 | H |
| 2 | MT_UNLOCK_ITEM | MT | | H |
| 3 | MO_SCORE | MO | 4 | H |
| 4 | MT_RANKING | MT | | H |
| 5 | MO_RANKING | MO | 4 | H |
| 6 | MO_DOWNLOAD_ITEM | MO | 7 | M |
| 7 | MT_REQUESTED_ITEM | MT | | M |
| 8 | MO_LOAD | MO | 9 | H |
| 9 | MT_LOAD | MT | | H |
| 10-13 | MO_SAVE, MT_SAVE, MO_RESTORE, MT_RESTORE | | | no game uses them |
| -1, -2 | MT_ERROR_DATA_CAN_BE_FOUND, MT_ERROR_INTERNAL_SERVER_ERROR | MT | | no game handles them |
| 14 | nickname (IFRacing2) | MO | none | M |
| 23 | vote ("Your opinion") | MO | 24 | H |
| 24 | votes result | MT | | H |
| 25 | pay per use (continue, ...) | MO | none | H |
| 27 | time trial upload (IFRacing2 "Top chrono") | MO | ? | L |
| 116 / 117 | IFRacing2 challenge / ghost | MO / MT | ? | L |

## 4. The games' SMS library [H]

The games share one In-Fusio library, compiled into each game (class indices and field order
differ, method bodies are the same modulo the request-type numbering). In GhostHunter it is
class #62 (the SMS dialog/state machine) and class #61 (the score table and ranking viewer,
also used for the local "My scores"). The flow:

1. A prompt ("Do you want to continue? This will be done via SMS.", with Yes/No).
2. Yes: `new Sms()`, `setPrice(p)`, `createBlock(id)`, the fields, `endBlock()`,
   `Gamelet.sendSms(sms)`, `deleteSms()`; a "Waiting" screen.
3. `onSmsSent(status)` (`305a4c5b`): remembers sent / success.
4. Success: requests without an answer (pay per use, block 25) end here and the game goes on
   (GhostHunter: the continue is granted). Requests with an answer wait for onSmsReceived
   ("Waiting for a reply...", clear cancels). Failure: "Sorry, there has been a problem when
   sending the SMS. Try again?" (French games: "Problème dans l'envoi du SMS. Recommencer?").
5. `onSmsReceived(byte[])` (`6f6c70d4`): `new Sms(bytes)`, `getIdBlock()`, `getLengthBlock()`,
   then a switch on the id; the matching handler parses the data and sets "received", and the
   result screen follows ("We've received your ranking.").

Prices: `setPrice(1)` for scores and rankings, 2 for continues and votes, 3-5 for unlocks
(game-specific), 0 for restores.

### 4.1 Score and ranking: MO_SCORE (3), MO_RANKING (5) -> MT_RANKING (4)

- MO_SCORE: score (32), 4 bits 0, level (16), nickname (`Gamelet.getNickName`, up to 15 x 7
  bits, no terminator; IFRacing2 pads with 0).
- MO_RANKING ("Update" in the Scores menu): 4 bits, all ones.
- MT_RANKING: the game copies **124 bytes and 1 bit** (993 bits) verbatim into its saved context
  (Gamelet.saveCtx) at a game-specific offset (18 in most games, 140 in Tomb Raider 1-3, 33 in
  Age of Empires, 160 in Malibu Ride 2) and the score-table class displays it, as three pages
  (left/right): the ranking date, the player's rank, the list. Layout, from the reads of the
  display code (offsets relative to the record; the games read them at `base*8 + N`):

| bit | width | field |
|---|---|---|
| 0 | 4 | unused (L) |
| 4 | 1 | 1 = ranked |
| 5 | 32 | ranked: the player's rank; else the number of best scores kept (0: "You are not ranked yet.", n: "You're not among the n best scores.") |
| 37 | 14 | ranked: level |
| 51 | 32 | ranked: score |
| 83 | 105 | not read by any game; the mock puts the nickname here (L) |
| 188 | 8 | the player's position, for the list's last two rows |
| 196 | 4+5+12 | ranking date: month, day, year. **Month 0 = no ranking stored** ("We don't have a ranking for you...") |
| 217 | 4+5+12 | contest end date; month 0 shows `********` |
| 238 | 5 x 151 | the list: nickname (15 x 7 bits, 127 = space), level (14), score (32). Rows 1-3 show ranks 1-3; rows 4-5 show ranks p-1, p where p = the position at 188 if it is above 5 and the contest has an end date, else 4 and 5 |

### 4.2 "Your opinion": vote (23) -> votes (24)

- Vote: mark (3 bits, 1-5), nickname 15 x 7 bits padded with `'\n'`, then a `'\n'`.
- Answer: **125 bytes** that the game keeps and shows as "Thanks for your input. The average
  score is [stars] with N votes. And the favourite games are: -1- ... -5-": average mark (3),
  votes (32), then 5 x {mark (3), game name (27 x 7 bits; the game shows the first 15, `'\n'`
  shows as a space)}.

### 4.3 Pay per use (25)

An empty block. The game acts on onSmsSent(SUCCESSFULLY) alone: GhostHunter's "Do you want to
continue?" restarts the level with new lives. BombJack, Pikubi, Sexy Breaker Bikini, Sexy Manga,
The Terminator, Worms, ShadoFighter, SouthPark and Eagle Squadron send it too (seen in a corpus
run with a generic key script; what each one sells was not checked).

### 4.4 Unlocks: MO_UNLOCK_ITEM (1) -> MT_UNLOCK_ITEM (2)

- Request: a **400-bit map** of the items asked for. Football Fans asks for a team's whole group
  ("download this team and 4 other teams?", bit i = team i of 32); Mutant Alert asks for `count`
  levels from `offset`; Pikubi2 sends its own bit array.
- Answer: a 400-bit map of the items granted. Football Fans sets team i for each bit i < 32;
  Mutant Alert skips the first 10 + (levels already bought) bits and counts the ones after them
  into its level count (context byte 148). Then "The extension has been received" ("L'extension
  a été reçue. A vous de jouer!").

### 4.5 Restore: MO_LOAD (8) -> MT_LOAD (9)

"Do you want to send an SMS to recover your extensions?" ("Recover" / "Récupérer"). An empty
request (price 0). The answer: nickname (15 x 7 bits, skipped), the 400-bit map of everything
bought (parsed like MT_UNLOCK_ITEM), then three {level (16), score (32)} entries that **replace
the local score table** (the game clears context bytes 0-17 first). With no bit set the game
says "No extensions bought for this game" ("Pas d'extensions acquises pour ce jeu"). Worms and
MotoGp send MO_LOAD but have no MT_LOAD handler (M).

### 4.6 Downloads: MO_DOWNLOAD_ITEM (6) -> MT_REQUESTED_ITEM (7)

The request is a 32-bit item id (007 Ice Racer's Q-lab items, IFRacing2's tracks, Pikubi2,
IF Ping Pong, the IF sports games). The games that handle MT_REQUESTED_ITEM (IFPingPong,
IFSki*, IFSudoku, MonkeyBusiness, Pikubi2) only set their "received" state; the item itself
presumably came as an over-the-air download through the platform (`catalog`), which the port
does not have. Several games (007, FighterPilot, SphereMadness, TheTerminator) do not handle 7
at all and go on after onSmsSent.

### 4.7 IFRacing2 [L]

IFRacing2 has its own code on top of the library: block 14 (nickname + 16-bit 0, sent in front
of other blocks), 27 ("Top chrono": 32-bit lap time, 8-bit fields and an 80-byte racing line),
116 (a challenge: lap data and the 80-byte line) and an MT block 117 handled directly in its
Gamelet.onSmsReceived (a nickname, a time and a line: another player's ghost). What the server
answered to 27 / 116 cannot be inferred without the server; the mock does not answer them.

### 4.8 Per game

MO = blocks the game sends, MT = blocks its onSmsReceived handles.

| game | MO | MT |
|---|---|---|
| 007 Ice Racer | 1 3 5 6 8 25 | 0 2 4 9 |
| Age Of Empires Mobile | 3 5 23 | 4 24 |
| Arthur & les pirates | 3 5 23 25 | 4 24 |
| Banjo Kazooie | 3 5 23 25 | 4 |
| Bomb Jack | 1 3 5 25 | 0 2 4 9 |
| Bomb Squad | 1 3 5 8 23 25 | 0 4 24 |
| Charmed | 1 3 5 8 23 | 0 2 4 9 24 |
| Crazy Cobra 2 | 3 5 23 25 | 4 24 |
| Eagle Squadron | 3 5 23 25 | 0 4 24 |
| Fighter Pilot Evolved | 3 5 6 23 25 | 0 4 24 |
| Flynn | 3 5 23 25 | 4 24 |
| Football Fans | 1 3 5 8 | 2 4 9 |
| Ghost Hunter | 3 5 23 25 | 0 4 24 |
| IF Ping Pong | 1 3 5 6 8 23 25 | 0 2 4 7 9 24 |
| IF Racing | 1 3 5 8 | 0 2 4 9 |
| IF Racing 2 | 1 3 5 6 14 23 25 27 116 | 0 2 4 9 24 117 |
| IF Ski Extreme, IF Ski Jumping, IF Sudoku, Monkey Business, Pikubi 2 | 1 3 5 6 23 25 | 0 2 4 7 9 24 |
| Jungle Run, Star Wars 3 | 3 5 23 25 | 0 4 24 |
| Malibu Ride 2 | 1 3 5 23 | 2 4 9 24 |
| Midtown Madness 3 | 1 3 23 | 0 2 4 9 24 |
| Moto GP, Worms | 3 5 8 23 25 | 0 4 24 |
| Mutant Alert | 1 3 5 8 | 0 2 4 9 |
| Pikubi, Xmas Tales | 3 5 23 25 | 4 24 |
| Reversi | 1 3 5 23 25 | 0 4 24 |
| Sexy Breaker Bikini, Sexy Manga, South Park | 1 3 5 23 25 | 0 2 4 9 24 |
| Sexy Video Poker | 1 3 5 8 23 25 | 0 2 4 9 24 |
| Shado Fighter | 3 5 25 | 4 |
| Sphere Madness 1, 2 | 3 5 6 23 25 | 4 24 |
| The Terminator | 1 3 5 6 23 25 | 0 2 4 9 24 |
| Tomb Raider 1-3 | 3 5 | 4 |
| Tour De France | 1 3 5 8 23 | 0 2 4 9 24 |
| Zoo Tycoon 2 | 3 5 23 | 4 24 |

(Found by listing each game's `createBlock` ids and the switch after `getIdBlock`, with
tools/disasm.py.)

## 5. The mock server (src/core/smsserver.c)

The port replaces the PC tool and the receive path. A host setting (exen.h,
`exen_sms_set_mode`), not saved in states:

| mode | sendSms reports | answers |
|---|---|---|
| `EXEN_SMS_SERVER` (default) | sent | yes, 2 s later (VM clock) |
| `EXEN_SMS_DELIVERED` | sent | never (the simulator with `Activate=0`) |
| `EXEN_SMS_OFFLINE` | not sent | never (the simulator with no server running) |

In server mode, `sendSms` decodes the message (header and blocks; malformed messages are
logged and get no answer), updates the server's records and, for a request that has one, builds
the answer: the contents of the `0x60` block exactly as the platform would hand them to the
game, bytes after the blocks set to `0xff`. The answer is delivered by the core after a host
callback (tick or key) once the VM clock has passed the due time: `Bootstrap.smsReceived(byte[136])`,
after the smsSent report, never from inside a native. One answer at a time; a new request
replaces an answer still in flight.

The answers are the most generous valid ones, and contain only what this player sent:

| request | answer |
|---|---|
| MO_SCORE | the score is recorded (best five kept, best first, a tie ranks the newer first); MT_RANKING ranks the submitted score among the player's own scores, today's date (the host's date where the frontend gives one, else 2004-01-01), no contest end, the list of the player's scores |
| MO_RANKING | MT_RANKING with the player's best score, or "not ranked yet" |
| vote | MT_VOTES: the average of the player's marks, the number of votes, and one favourite game, this one (its title from the game database), with that average |
| pay per use (25) | none: reported as sent, which is what grants it |
| MO_UNLOCK_ITEM | MT_UNLOCK_ITEM with the same map: everything asked for is granted (and remembered) |
| MO_DOWNLOAD_ITEM | MT_REQUESTED_ITEM with the same item id |
| MO_LOAD | MT_LOAD: the last nickname received, every unlock granted so far, the three best scores received (zeros when there are none: like the real service, a restore on an empty account clears the local score table) |
| anything else (14, 27, 116) | none |

The server's records (scores, nickname, votes, unlocks) and an answer in flight live in
`ExSmsServer` (plain data) and are saved in the state chunk `SMSV`; they start empty with each
VM. Each SMS is logged in one line (`sms: sent MO_SCORE(3) score=975 level=1 name="Xcell" ->
MT_RANKING, rank 1 of 1 in 2.0 s`, `sms: received MT_RANKING(4)`); `exen_sms_set_log(vm, 1)`
(exen-run `--sms-log`) adds a hex dump of every message.

Not reproduced: the platform statistics block 0x41, the end byte and the checksum (they only
matter on the wire), and messages stored for a gamelet that is not running.

## 6. Open questions

- IFRacing2's challenge / time-trial blocks (27, 116 -> 117): the answer would be another
  player's ghost; nothing in the game tells what the server sent back for 27 and 116.
- Bits 0-3 and 83-187 of the ranking record are never read; their meaning is unknown.
- MT_REQUESTED_ITEM probably carried, or announced, an over-the-air download; the games only
  acknowledge it, and the port has no catalog to put an item into.
- Games that send MO_LOAD without handling MT_LOAD (Worms, MotoGp) presumably got their content
  re-sent another way.
- Answers are delivered after host callbacks only, so a game that stopped its timer would get
  its answer at the next key press. Every game observed keeps its timer running while it
  waits.
