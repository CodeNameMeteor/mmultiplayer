# Code Review: mmultiplayer

This document records findings from a read-only, in-depth review of the repository at the `claude/affectionate-gauss-yxvhat` branch (identical to `main` at review time). **No source code was changed.** This file is the only addition.

Scope:
- `Server/` (Go relay server)
- `Launcher/`, `Client/` first-party code (core, `addons/trainer`, `addons/chaos`, `addons/client`, `addons/dolly`, `addons/misc`)
- Build, packaging, installer and CI (`build.sh`, `package.sh`, `resources/installer/`, `.github/workflows/build.yaml`)

Out of scope: vendored code (`Client/imgui/`, `Client/json.h`), the generated SDK dumps (`Client/sdk/`), and the C++ client's network-message handler beyond its interface with the server.

## How to read the confidence column

- **verified**: the mechanism was re-checked against the source during the review.
- **reported**: found by a reviewer after tracing the code path. The mechanism is credible, but the exact lines or reachability were not re-checked, or runtime behaviour was not tested. Treat these as leads to confirm.

Line numbers for the server findings were re-checked against the files. Some line numbers in the reviewers' raw output were wrong, and those are corrected here.

## Summary

| Severity | Count | Themes |
|---|---|---|
| Critical | 2 | Unauthenticated remote crash of the whole server; unbounded memory from one connection |
| High | 15 | Server deadlocks and lock-order inversions; UDP spoofing and amplification; client render-thread races and use-after-free; remotely triggered client overflows; teleport and aimbot crashes |
| Medium | 29 | Stale cached game objects; uninitialized reads; incomplete reverts; settings overwritten each launch; CI and build mismatches |
| Low | 16 | Resource leaks, busy loops, misleading success reporting, minor config issues |

The most important issues are the server findings: any client on the network can crash the relay for every room, and the same server accepts unauthenticated state changes. The client-side findings matter most for players who join rooms with untrusted members, because a room member can crash other players' clients.

---

## Server (Go)

### S1. Critical: concurrent map iteration and write crashes the server (verified)
- **Where:** `Server/system.go:23-35` (`GetClientById` iterates each `r.Clients` while holding only `system.RLock`), `Server/room.go:33` (`AddPlayer` writes `room.Clients` under `room.rwMu`), `Server/room.go:133` (delete under `room.rwMu`).
- **Why:** The system-level lock does not protect the per-room maps. Every 676-byte UDP packet calls `GetClientById`, which iterates those maps. A concurrent connect or disconnect writes them.
- **Impact:** Go aborts the process with `fatal error: concurrent map iteration and map write`. That cannot be recovered, so one unauthenticated sender can take down every room. The reviewer reproduced it in a scratch copy, and the race detector flags it.
- **Fix direction:** Take the room read lock when iterating, or keep a single `map[uint32]*Client` on `System` guarded by one lock.

### S2. Critical: one TCP connection can exhaust memory (reported)
- **Where:** `Server/client.go:270-285`. `json.Decoder` decodes into `map[string]interface{}` with no size cap. The 30-second deadline limits time, not bytes.
- **Impact:** A single client sending a multi-gigabyte `chat` body can get the process OOM-killed, which drops every room.
- **Fix direction:** Wrap the connection in `io.LimitReader` or cap message size before decoding.

### S3. High: deadlock between the ping loop and last-player teardown (verified lock order)
- **Where:** `Server/main.go:36-42` holds `system.RLock` and calls `r.SendMessage`, which takes the room lock. `Server/room.go:133-137` holds the room write lock and calls `system.RemoveRoom`, which takes `system.Lock` (`Server/system.go:14-20`).
- **Impact:** If the last player leaves while the ping loop is mid-iteration, teardown waits for the system writer lock and the ping loop waits for the room lock. Every subsequent connect and every UDP `GetClientById` then blocks behind the pending writer, freezing the server. A client can repeat connect and disconnect to hit this window.
- **Fix direction:** Snapshot rooms under the system lock, release it, then message rooms. Never call a system-level method while holding a room lock.

### S4. High: room lock and client lock are taken in opposite orders (reported)
- **Where:** `Server/room.go:262-291` (`SendLastPackets`) holds `room.rwMu.RLock` and calls `GetLevelAndPosition`, which takes `client.rwMu.RLock`. `Server/client.go:95-170` (name, chat and level handlers) holds `client.rwMu` and then takes the room lock.
- **Impact:** A deadlock cycle, which needs a pending room writer at the same moment.
- **Fix direction:** Copy the needed client fields out under the room lock, then release it before taking client locks.

### S5. High: UDP identity is not bound to the sender (verified by reading `main.go`)
- **Where:** `Server/main.go:115-125` picks the client from the first four bytes of the datagram. The source address is never checked against the client. `Server/client.go:236-246` trusts the submitted position.
- **Impact:** Anyone who joins a room learns other members' IDs from `connect` messages. They can then spoof a victim's ID, overwrite the victim's position, and, through `Server/room.go:285-291`, make the victim "it". `dead` (`client.go:203`) also lets any client make itself "it".
- **Fix direction:** Bind the UDP peer address to the client at connect time, and validate positions against plausible bounds.

### S6. High: UDP reflection amplification (reported)
- **Where:** `Server/main.go:125` with `Server/room.go:267-270`.
- **Impact:** A spoofed-source 676-byte packet makes the server send up to `(N-1) × 676` bytes to the spoofed address. Each packet also starts a goroutine (`main.go:115`), with no rate limit.
- **Fix direction:** Rate-limit per source address, and require the peer binding from S5 before replying.

### S7. Medium: data races on client and room state (verified with the race detector)
- **Where:** `Server/client.go:43-78` reads `room.gameMode`, `room.taggedPlayerId` and `room.canTag` without the room lock. `Server/room.go:45-53` reads other clients' name, character and level without their locks. `Server/room.go:267-270` reads `c.lastPacket` and `c.level` unlocked. `Server/client.go:270-281` reads `client.room` unlocked. The race detector reports `client.go:240` against `room.go:268`.
- **Impact:** Torn reads of Go string and slice headers can produce bad pointers.

### S8. Medium: second `connect` leaves a ghost member in the old room (reported)
- **Where:** `Server/client.go:75` overwrites `client.room` without removing the client from its previous room. Cleanup on disconnect (`client.go:278`) only touches the current room.
- **Impact:** The old room never empties, and its remaining members keep sending to a dead connection.
- **Fix direction:** Remove the client from the old room before assigning the new one.

### S9. Medium: no write deadline, and one goroutine per message per recipient (reported)
- **Where:** `Server/client.go:220-227` (no `SetWriteDeadline`). `Server/room.go:241-260` starts `go c.SendMessage` per recipient per message.
- **Impact:** A client that stops reading pins its goroutines forever. Combined with S2 (no chat size limit), one client can hold an unbounded number of goroutines and bytes.

### S10. Medium: message ordering is not preserved (reported)
- **Where:** `Server/room.go:241-260` and `room.go:45-55, 64-69`, since each send is its own goroutine.
- **Impact:** `gameMode` and `tagged` from `StartTagGameMode` can arrive reversed, and `connect` and `disconnect` for the same ID can reorder. Client-side impact was not verified.
- **Fix direction:** Use one sender goroutine per client with an ordered queue.

### S11. Medium: any room member can change game state (reported)
- **Where:** `Server/client.go:143-153` with `Server/room.go:103-108` and `191-193`. `announce` (`client.go:128-141`), `startTagGameMode` and `endGameMode` (`client.go:189-201`) have no authorization.
- **Impact:** Any member can set the tag cooldown. Values of about 9.2e9 seconds or more overflow the float-to-`Duration` conversion, so a huge value locks tagging for centuries and a negative value makes the cooldown zero.
- **Fix direction:** Authorize game-mode and cooldown changes to a room host, and validate the cooldown range.

### S12. Medium: Dockerfile cannot build (reported)
- **Where:** `Server/Dockerfile:2-5`. It copies only `main.go` and runs `go build main.go`. `main.go` depends on `client.go`, `room.go` and `system.go`, and there is no `go.mod` in the image. The base image `FROM golang` is an unpinned tag.
- **Fix direction:** Copy the whole `Server/` directory and the module files, build with `go build .`, and pin the base image.

### S13. Low: busy loops on listener errors (reported)
- **Where:** `Server/main.go:78-82` and `104-109` `continue` on `Accept` or `ReadFrom` errors with no backoff. `EMFILE` produces a 100% CPU spin.

### S14. Low: a stale room can be removed by name (reported, narrow window)
- **Where:** `Server/client.go:65-75` releases the system lock before `AddPlayer`. `RemoveRoom(name)` (`Server/system.go:14-20`) can then delete a live room with the same name.

### Server areas checked and clean
- Protocol framing and the UDP 676-byte length check.
- Type assertions with `ok` checks; no panics from malformed JSON types in the reviewed handlers.
- `go.mod` and `go.sum` are consistent.

---

## Launcher, installer and build

### B1. Medium: build.sh passes a platform that the project does not define (verified)
- **Where:** `build.sh` passes `-property:Platform=x86`. Both `Client/Client.vcxproj` and `Launcher/Launcher.vcxproj` define only `Win32` configurations.
- **Impact:** The build may fail or skip configuration. The `.sln` maps x86 to Win32, but `build.sh` bypasses the solution.
- **Fix direction:** Use `Win32`, or use the solution file.

### B2. Medium: package.sh expects output paths the build does not produce (reported)
- **Where:** `package.sh:15-16, 104-105` read `Release/Launcher.exe` and `Release/mmultiplayer.dll` from the repo root. No `OutDir` is set, so the output lands under `Client/` or `Launcher/`.

### B3. Medium: CI grants write permission to every event and downloads an unverified SDK (reported)
- **Where:** `.github/workflows/build.yaml:11` grants `contents: write`. Line 31 downloads the DirectX SDK with no checksum. Lines 26, 41, 60 and 66 pin actions to tags rather than SHAs. `actions/upload-release-asset@v1` is archived. Line 74 uses `application/x-tar` as the content type of an `.exe`.
- **Fix direction:** Scope `contents: write` to the release job, verify the SDK checksum, pin actions to SHAs, and replace the archived upload action.

### B4. Medium: installer adds a Defender exclusion and XOR-obfuscates payloads (reported, policy decision)
- **Where:** `resources/installer/squibbles/main.go:107-115` adds a Windows Defender path exclusion for `{app}\bin` on every install. `launcher.buh` and `dll.buh` are XOR-obfuscated (`resources/installer/xor/main.go`, key 4) specifically to avoid detection. The exclusion persists unless uninstall succeeds.
- **Impact:** Users' antivirus protection is weakened, and this pattern is what antivirus products flag. Whether to ship it is a project decision, but it should be a stated, opt-in choice.

### L1. Low: launcher reports success when the DLL did not load (reported)
- **Where:** `Launcher/main.cpp:65-69` waits for the remote thread and then sets `status = true` without checking the `LoadLibraryW` result from `GetExitCodeThread`. Also `main.cpp:122` leaks a buffer, `main.cpp:136` ignores the `_wsplitpath_s` return (long install paths produce a garbage DLL path), and `main.cpp:19` and `34` use `TerminateThread` on a thread that may be inside `MessageBox`.

### L2. Low: squibbles writes without truncation (reported)
- **Where:** `resources/installer/squibbles/main.go:117, 123`. Harmless today because `installer.iss:55` deletes `bin` first, but a stale larger file would keep trailing bytes.

---

## Client: core (`Client/main.cpp`, `engine`, `hook`, `settings`, `menu`, `addons/client`, `addons/dolly`, `addons/misc`)

### C1. High: render thread reads players without the lock (reported)
- **Where:** `Client/addons/client.cpp:1127, 1163, 1224`. `OnRenderGames` iterates `Players.List` and calls `GetPlayerById` with no lock. `ShowTagCooldownOverlay` defaults to true (`client.cpp:37`), so this runs every frame in a level. The network thread's disconnect branch (`client.cpp:781-798`) erases and deletes players under `Players.Mutex`.
- **Impact:** A remote player leaving during a level can make the render thread read freed memory.

### C2. High: queued spawns hold references into freed players (reported)
- **Where:** `Client/engine.cpp:771-776` stores an `Actor*&` into `Player::Actor` in `spawns.Queue`. `client.cpp:649, 716, 756, 1674, 1708` pass it in. `TickHook` (`engine.cpp:524-549`) writes through it. `LevelLoadHook` clears the queue before loading (`engine.cpp:275-277`), not afterwards, so entries survive for many frames.
- **Impact:** If the disconnect branch deletes the player first (`client.cpp:796`), `TickHook` writes a pointer into freed heap and spawns an actor that is never despawned.
- **Fix direction:** Store the player ID in the queue, not a reference, and look the player up when the spawn runs.

### C3. High: cached game-object pointers can be stale after a level change (reported)
- **Where:** `Client/engine.cpp:625-707`. `GetWorld`, `GetPlayerController` and `GetPlayerPawn` cache static pointers that nothing invalidates. When a fresh scan fails they return the old pointer (`engine.cpp:655, 688, 706`). `GetPlayerController` returns null without refreshing when `PlayerCamera` is null (`engine.cpp:674-677`). Callers dereference the result (`client.cpp:904-913`, `dolly.cpp:580-596`, `menu.cpp:135`). `projectionTick.Matrix` (`engine.cpp:366`) is never cleared and is read by `WorldToScreen` (`engine.cpp:879`).
- **Impact:** Use-after-free across level transitions. Whether the main menu world lacks a player controller, which decides how often this path runs, was not verified.

### C4. High: a remote player's name can overflow a fixed buffer and crash the client (verified overflow, crash mechanism reported)
- **Where:** `Client/addons/client.cpp:867` writes `"[Tag] %s tagged %s"` into `char buffer[0xFF]` with `sprintf_s`. Player names are stored without a length check (`client.cpp:463-467`), and the server does not check length either (`Server/client.go:39-70`). The same pattern appears at `client.cpp:859, 957, 1515, 1530, 1547`.
- **Impact:** A room member with a name of roughly 230 or more characters who tags someone can crash the victim's client. MSVC's secure CRT fails fast on overflow in release builds.
- **Fix direction:** Cap name length at the server and at receipt, and use `snprintf` or `std::string`.

### C5. High: uninitialized stack buffer can be sent to the server (reported)
- **Where:** `Client/addons/client.cpp:855-874`. The buffer is written only when the previously tagged player is still in the list. If that player has left, the uninitialized `char[255]` goes out as an announce body, which leaks stack contents to the room. If those bytes are not valid UTF-8, `msg.dump()` throws inside `ClientListener` (`SendJsonMessage`, `client.cpp:180`), and the process terminates.
- **Fix direction:** Initialize the buffer, and handle the missing-player case explicitly.

### C6. High: loaded dolly recordings hold uninitialized actor pointers (reported)
- **Where:** `Client/addons/dolly.cpp:557` declares `Dolly::Recording recording;`. `LoadRecordings` never assigns `Actor`, and `dolly.h:39` has no initializer. The pointer is dereferenced at `dolly.cpp:213-214` (Camera View toggle), `433-434` (delete) and `882-885` (`OnBonesTick`, which runs every skinned-mesh tick).
- **Impact:** A non-null garbage value crashes on the next bones tick.
- **Fix direction:** Initialize to `nullptr`, and check before every dereference.

### C7. High: default keybinds can dereference a null controller (reported)
- **Where:** `Client/addons/dolly.cpp:755-757, 764-766, 771-776, 778-783` call `GetPlayerController()` without a null check. It returns null when there is no `ATdPlayerController` or while loading (`engine.cpp:661-662, 665-688`). The defaults are `VK_ADD`, `VK_SUBTRACT`, `VK_UP` and `VK_DOWN` (`dolly.cpp:468-471, 821-823`), handled in an ordinary input callback (`dolly.cpp:845`).
- **Impact:** Pressing Up or Down in a menu or loading screen can crash. Whether menus lack a controller was not verified.

### C8. Medium: unvalidated character index from the network (reported)
- **Where:** `Client/addons/client.cpp:461-466` checks only `is_number_integer` before `player->Character = msgCharacter`. `SpawnCharacter` (`client.cpp:649`) then indexes `meshes[]` and `materials[]` with no range check (`engine.cpp:499, 503`). The server does validate characters (`Server/client.go:59`). Exploiting this needs a malicious or man-in-the-middle server, since the protocol is plaintext to a hardcoded IP (`client.cpp:108`).
- **Fix direction:** Range-check the value on receipt.

### C9. Medium: UDP `recvfrom` overwrites the server address (reported)
- **Where:** `Client/addons/client.cpp:291` passes `&Server` as the from-address. Any datagram from anyone rewrites the global `Server` used by `sendto` at `client.cpp:920`.
- **Impact:** One spoofed packet redirects position and bone data until the next reconnect.
- **Fix direction:** Use a separate variable for the sender address, and drop packets not from the server.

### C10. Medium: non-object JSON from the server terminates the process (reported)
- **Where:** `Client/addons/client.cpp:353-357` and `443` use non-const `msg["type"]` on parsed JSON. A top-level `[]` or `5` throws `type_error 305` outside any try block, inside a detached thread, which calls `std::terminate`.

### C11. Medium: framing and out-of-bounds read in `RecvJsonMessage` (reported)
- **Where:** `Client/addons/client.cpp:138-174`. `recv` fills up to 4095 bytes with no terminator. If a full buffer ends without NUL, `strlen` (line 148) reads past the static array. A message split across two `recv` calls parses as garbage, and the parse failure is treated as a disconnect (`client.cpp:441`).
- **Fix direction:** Accumulate into a growable buffer and split on the delimiter.

### C12. Medium: a malformed settings file prevents injection (reported)
- **Where:** `Client/settings.cpp:238-244, 255-267` call `operator[]` on non-object JSON, which throws. The getters also throw on type mismatch (`client.cpp:1562, 1568`, `menu.cpp:211`, `misc.cpp:485`). These run in `DllMain` (`main.cpp:22-40`), so a hand-edited file such as `"Menu": 5` causes an uncaught exception during injection. An out-of-range `Client.Character` is not range-checked (`client.cpp:1568`) and indexes `Engine::Characters` (9 entries) at `client.cpp:1313`.
- **Fix direction:** Catch parse and type errors per setting and fall back to defaults; range-check the character.

### C13. Medium: unchecked `Spawn` and `SkeletalMeshComponent` results (reported)
- **Where:** `Client/engine.cpp:490-497`. The result of `player->Spawn` is dereferenced at line 494 with no check, and so is `actor->SkeletalMeshComponent` at 496-497.

### C14. Medium: full object scan every tick (reported)
- **Where:** `Client/engine.cpp:524` calls `GetPlayerPawn(true)`, which forces `GetWorld(true)` (`engine.cpp:632-653`). That scans the whole `GObjects` table every frame until a world with a controller is found.
- **Impact:** A performance cost each frame while no controller is present.

### C15. Medium: engine objects touched off the game thread, and unsynchronized dolly state (reported, thread assignment inferred)
- **Where:** `client.cpp:271, 723, 752, 790` call `Engine::Despawn` from the network thread. `client.cpp:813, 826, 879` write controller fields from it too. `PlayerHandler` (`client.cpp:296-310`) and `OnPreLevelLoad` (`client.cpp:1658`) write player fields under `lock_shared`, while readers in `OnActorTick` and `OnBonesTick` use the same shared lock. Dolly has no locking at all. For example, a Delete on the UI thread can erase recordings while `OnActorTick` iterates them (`dolly.cpp:867`).

### C16. Medium: menl hook handling can silently drop hooks (reported)
- **Where:** `Client/engine.cpp:310-336`. `LoadLibraryAHook` removes three hooks, then a detached thread waits for a single `0xE9` byte at `death.PostBase` before re-hooking. If the other module patches differently, the hooks stay removed for the rest of the session and the thread spins forever (`Sleep(1)`). Separately, `strstr(module, ...)` at `engine.cpp:311` crashes if `LoadLibraryA` is called with `NULL`.

### C17. Medium: dolly file loads are unchecked (reported)
- **Where:** `Client/addons/dolly.cpp:502-514, 553-574`. Stream read results are ignored. `frameCount` from the file goes straight into `resize` (`564`), and marker and recording counts drive unbounded loops. Truncated or corrupt files cause `bad_alloc`, an exception, or out-of-range `Character` values (used at `415` and `899`).

### C18. Medium: `main.cpp` may hang or misbehave during injection (reported, low-medium confidence on the hang)
- **Where:** `Client/main.cpp:36-45` falls through to cleanup and deletes every addon after a successful `Initialize`. This is harmless today but would dangle if any callback used per-addon member state. `main.cpp:18-20` spins on `GetModuleHandle("d3d9.dll")` while holding the loader lock. The launcher injects after `openal32` loads (`Launcher/main.cpp:24-60`), so `d3d9` may not be loaded yet, which risks a hang.

### C19. Low: leak in `Setup`, and wrong unsigned checks (reported)
- **Where:** `Client/addons/client.cpp:115-123`. The loop advances `result` to null, so `freeaddrinfo(result)` frees nothing. `ClientListener` calls `Setup` every ~500 ms while the server is down (`client.cpp:376-412`), so this leaks steadily. `client.cpp:391` and `400` test an unsigned `SOCKET` with `< 0`, which never fires.

### C20. Low: F9 bound to two actions (reported; default values read in source)
- **Where:** `Client/addons/dolly.cpp:818-819` binds both StartStop and Record to `VK_F9`. The DollyTab reset default for StartStop is `VK_F7` (`dolly.cpp:465`), while `Initialize` uses F9.

### C21. Low: unchecked pawn sub-objects (reported)
- **Where:** `dolly.cpp:228, 588, 596, 666, 858-860`. `misc.cpp:152, 175, 323` (`WorldInfo`) and `276-277` (`GetWorld()->Game->TdGameData`). Line 666 also copies 108 atoms from `LocalAtoms` with no size check.

### C22. Low: `world->Game` dereferenced without a check (reported)
- **Where:** `Client/engine.cpp:724, 755`. `GetTimeTrialGame` is used at `trainer.cpp:899`.

### C23. Low: crash handler can deadlock (reported, medium confidence)
- **Where:** `Client/debug.cpp:92-141`. `ExceptionHandler` suspends all other threads and then uses the heap and `printf`. If a suspended thread holds the heap lock, the handler deadlocks instead of reporting.

### C24. Low: `UnTrampolineHook` would restore a wrong relative jump for E9-type trampolines (reported, not reached today)
- **Where:** `Client/hook.cpp:218-238`. Every hooked site currently starts with non-E9 bytes, so this is not reached by current flows.

### Client core areas checked and clean (per reviewer)
- `pattern.cpp`: `CheckMask` and `FindPattern` stay within the module image.
- All FindPattern pattern and mask pairs in `engine.cpp`, `debug.cpp` and `dolly.cpp` have matching lengths.
- The x86 length-decoder tables in `hook.cpp` reference only in-range sub-tables.
- The D3D9 `EndScene` and `Reset` vtable indices match `d3d9.h`.
- The server validates characters on connect and on change, and frames messages with a NUL terminator.

---

## Client: trainer (`Client/addons/trainer.h`, `trainer.cpp`)

### T1. High: stale state survives a level load (reported)
- **Where:** `Client/addons/trainer.cpp:1700` (the pre-level-load hook clears only `KickglitchToolActivated` and `BeamerToolActivated`). `HasSave`, the save snapshot, `HasCheckpoint` and `CheckpointLocation`, `Fly.Enabled` and `Fly.Location`, and `StrangToolActivated` all survive a map change (`trainer.cpp:23, 1381, 1391, 1408, 1639`).
- **Impact:** If Fly is on during a map change, the next tick (`trainer.cpp:1448`) pins the pawn at the old map's coordinates with physics disabled until Fly is toggled. Loading in the new map restores the old map's location, health and physics (`trainer.cpp:584-594`).

### T2. Medium: God and Fly change engine fields that are never restored (reported)
- **Where:** `Client/addons/trainer.cpp:1408, 1451, 1606`. While God or Fly is active, `EnterFallingHeight` is set to `-1e30` and `Physics` to `PHYS_None` each tick. Nothing restores `EnterFallingHeight` when God or Fly turns off. Save snapshots these transient values (`trainer.cpp:1378-1381`), and Load writes them back (`trainer.cpp:584-600`).
- **Impact:** Save while flying, then turn Fly off and Load: the pawn is left with no physics and no collision.

### T3. Medium: the master Enabled checkbox can soft-lock Fly (reported)
- **Where:** `trainer.cpp:1507-1510, 1326-1330, ~1346, 871-877`. Unticking master Enabled does not clear `Fly.Enabled`. `OnTick` returns at the `!Enabled` gate before the Fly branch, and `OnInput` returns at line 1507 when disabled, so the Fly key cannot turn Fly off.
- **Impact:** The player floats with no collision until Enabled is re-ticked.

### T4. Medium: the speedometer's saved settings are overwritten on every launch (verified)
- **Where:** `Client/addons/trainer.cpp:785-786`. `foundSettings` is computed as `Settings::GetSetting({"Speedometer","Item"}, json::object()).is_null()`. `Settings::GetSetting` (`Client/settings.cpp:55-77`) inserts the default when the key is missing and otherwise returns the stored value. Neither is null, so `foundSettings` is always false and the block at `trainer.cpp:794-811` always runs.
- **Impact:** Customized Factor, Multiply, ModifyDisplayedValue and AddSpaceBelow values are replaced with defaults and saved to disk on every launch. The `if (!foundSettings)` branch should run only when the key is absent.
- **Fix direction:** Test for absence (for example, `contains`) rather than `is_null()` of a value that is never null.

### T5. Medium: unchecked dereferences in Save, Load and the Refill button (reported)
- **Where:** `trainer.cpp:399-405` (Save), `561-566, 591-597` (Load), `908-909` (Refill in `TrainerTab`). `pawn->CylinderComponent`, `pawn->Mesh`, `pawn->SwanNeck1p`, `controller->myHUD`, `tdhud->EffectManager` and `EffectManager->ReactionTimeEffect` are dereferenced without null checks. Only `pawn` and `controller` are checked.
- **Impact:** Save, Load or Refill during partial construction, or before the HUD exists, can crash the host game. Reachability was not checked against the SDK.

### T6. Low: render-path writes to game objects (reported, medium confidence)
- **Where:** `trainer.cpp:908-909` (`TrainerTab`), `OnRenderScene`, and the flags written from the window message handler. `TrainerTab` writes `controller->ReactionTimeEnergy` from the present hook while `OnTick` mutates the same objects. The flags are plain non-atomic values read on the tick thread.

### T7. Low: user-editable format strings and config types are not validated (reported)
- **Where:** `trainer.cpp:129` (`Item::Draw`) passes the user-editable `Format` string directly to `ImGui::TextColored` as a printf format, so a value like `%s` is undefined behaviour. `trainer.cpp:294, 297` call `.get<std::string>()`, which throws on a wrong JSON type. `trainer.cpp:1285` uses `%d` for a `size_t`.
- **Impact:** A hand-edited settings file can crash on first render. Severity is low because the input is local.

### Trainer areas checked and clean
- No network-driven input.
- Speedometer item array is fixed size and bounds-safe.
- `OnTick` null-checks pawn and controller.
- Move-array access is guarded.
- The `StateHandler` byte pattern and mask have matching lengths.

---

## Client: chaos effects (`Client/addons/chaos/`)

### X1. High: teleport index can be one past the end of the AI controller array (verified)
- **Where:** `Client/addons/chaos/effects/teleport.cpp:54`: `aicontrollers[RandomInt(0, aicontrollers.Num())]`. `RandomInt` is inclusive (`effect.cpp:12-13` uses `uniform_int_distribution(min, max)`), so the index can equal `Num()`. `TArray::operator[]` is unchecked.
- **Impact:** Index `Num()` reads past the array. Lines 55-58 then dereference the result. The effect retries every tick, so the bad read recurs with probability 1/(N+1) per pick.
- **Fix direction:** Use `RandomInt(0, Num() - 1)` and guard against `Num() == 0`.

### X2. High: aimbot shutdown dereferences null controller or pawn (reported)
- **Where:** `Client/addons/chaos/effects/aimbot.cpp:152-158` (`SetPlayerAimbot`) dereferences `controller` and `pawn` with no null check. `Shutdown` (`aimbot.cpp:47-57`) calls it unguarded. `chaos.cpp:129-131` triggers `Restart()` when Enabled is unchecked, and `Restart` calls `Shutdown`. `Engine::GetPlayerController` returns null while loading (`engine.cpp:659-661`).
- **Impact:** Unchecking Enabled or pressing Restart during a level load, or while the pawn is null, crashes.

### X3. High: failed reverts are never retried and persist (reported)
- **Where:** `chaos.cpp:65-84` with `disableinput.cpp:58-68`, `gravity.cpp:47-58`, `camerarotation.cpp:60-73`, `health.cpp:88-103`. `Restart()` calls `Shutdown()` on each active effect, discards the return value, and then clears the list. Several `Shutdown` methods return early on null pawn or controller before reverting.
- **Impact:** DisableInput can leave the controller ignoring all input until a level reload. Gravity can leave `WorldGravityZ` modified, and CameraRotation can leave Dolly's code patch applied.

### X4. Medium: stale cached game objects (reported, same mechanism as C3)
- **Where:** `engine.cpp:625-705` (`GetWorld`, `GetPlayerController`, `GetPlayerPawn`). The static cache is replaced only on a successful lookup. `chaos.cpp:498` can receive a freed controller after a transition to a world without a player controller.

### X5. Medium: incomplete revert in subway train effect (reported)
- **Where:** `effects/level/train.cpp:193-202`. `ModifyTrains` uses `return` where it should use `continue` when one train entry's sublevel state mismatches, skipping the remaining entries for both apply and revert.
- **Impact:** On subway_p, the tunnel train keeps its modified `PlayRate` until the level reloads.

### X6. Medium: unchecked type cast of `world->Game` (reported, reachability unverified)
- **Where:** `chaos.cpp:180-181`, `effect.cpp:58-64`, `checkpoint.cpp:22-23, 39-45, 56-62`, `killbots.cpp:22-28`, `gotomainmenu.cpp:33-34`. `world->Game` is `static_cast` to `ATdSPGame` or `ATdGameInfo` without a type check. The SDK shows `ATdMPGame` derives from `ATdGameInfo`, not `ATdSPGame`, and its field layout differs at the offsets these effects use.
- **Impact:** In a multiplayer game world, checkpoint writes a bit into an unrelated string, and AI lookup reads a garbage `AIManager`. Whether chaos runs in such a world was not established.
- **Fix direction:** Use `dynamic_cast` or an explicit type check before the cast.

### X7. Medium: chaos state is touched from the UI thread without locks (reported, thread split inferred)
- **Where:** `chaos.cpp:401-448` (`OnRender`), `486-606` (`OnTick`), `65-84` and `86-257` (UI). `ActiveEffects`, `EnabledEffects` and effect fields are accessed without locks from the tick hook and the EndScene hook. `Restart` clears `ActiveEffects` and calls `Shutdown`, which writes game objects, from the UI side.
- **Impact:** Clicking Restart during the tick's iteration can invalidate iterators or interleave `Shutdown` with `Tick`.

### X8. Low: settings loaded from file are not clamped (reported)
- **Where:** `chaos.cpp:25, 617, 422, 628`, `effects/camerarotation.cpp:40`. Only UI paths clamp. `TimeUntilNewEffect <= 0` fires the timer every tick, and `chaos.cpp:422` divides by it. `DurationTime = 0` makes `camerarotation.cpp:40` divide by zero (`65536/0`).

### X9. Low: reverts write hard-coded defaults instead of saved originals (reported)
- **Where:** `aimbot.cpp:119-122, 97`, `health.cpp:98-99`, `gamespeed.cpp:43`. Aimbot falls back to generic weapon values for any weapon not in its 11-entry table, so Shutdown overwrites those weapons' spread and range. Health's no-regen revert writes 5 and 25, which overlaps `misc.cpp`'s OHKO and regen writes. GameSpeed sets `TimeDilation` to 1.0, overriding `misc.cpp:295-297` until Misc re-applies it.

### X10. Low: jump timing uses wall-clock time and is not paused (reported)
- **Where:** `effects/jump.cpp:31, 63, 88`. `GetTickCount64` is stored as `float` and is not paused by chaos Pause or by the UI being open, so the next jump fires immediately on resume.

### X11. Low: unchecked array access (reported)
- **Where:** `effect.cpp:85-87` (`SequenceObjects[i]` without a null check, unlike `NestedSequences` at line 96), `effect.cpp:134` (`GameSequences[0]` without a `Num` check), `train.cpp:211`.

### Chaos areas checked and clean
- Reverse-iterator erase in `chaos.cpp:521-548` removes the correct element.
- `group.h` operator precedence and empty-group path are correct, with no division or modulo by zero.
- Capturing reverts (Framerate, InvertedControls, Gravity, Fov, ColorScale, GameSpeed) restore correct values and are idempotent.
- Mutually exclusive effects share `GetClass()`, so none double-apply.
- `GetTdAIControllers` returns a shallow copy of a `TArray`, which has no destructor, so there is no double free.

---

## Recommended order of work

1. **Server S1 and S2** (remote crash and memory exhaustion), then **S3 and S4** (deadlocks), and the **UDP binding in S5 and S6**.
2. **Client C4 and C5** (remote-triggered overflow and uninitialized send), **C6 and C7** (uninitialized pointers and null controller in dolly), and **C10 and C11** (framing and non-object JSON from the server).
3. **C2 and C3** (dangling queued spawns and stale cached objects), which affect every level transition.
4. **T4** (settings overwritten each launch), a user-visible regression that is simple to fix.
5. **X1 and X2** (chaos out-of-bounds and null deref), then the remaining medium and low items.
6. **B1 and B3** (build mismatch and CI permissions), then **B4** (a policy decision for the maintainers).

## Notes on method

- Reviewers were read-only. They traced each candidate from a real caller or input before reporting it, and they discarded speculative or style-only items.
- Key findings were re-checked by hand: S1, S3, S5 (mechanism), C4 (overflow), T4, X1, B1 and B3.
- Reviewer line numbers are reported as found. Where they disagreed with the file, the corrected location is used above.
- Runtime behaviour was not tested. The Windows client was not built or run in this environment, and the Go server was checked in a scratch copy only.
