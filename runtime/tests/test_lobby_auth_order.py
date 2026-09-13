#!/usr/bin/env python3
from pathlib import Path


root = Path(__file__).resolve().parents[2]
main = (root / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")

sync_start = main.index("void ae_np_account_sync(void)")
sync_end = main.index("/* ---- list scope", sync_start)
sync = main[sync_start:sync_end]
assert "player_file_path(" in sync
assert '"netplay_secret"' in sync
assert "rnet_account_init(url.c_str());" in sync

pump_start = main.index("void ae_np_pump(void*)")
pump_end = main.index("void ae_np_set_player_name", pump_start)
pump = main[pump_start:pump_end]
assert pump.index("ae_np_account_sync();") < pump.index("rnet_account_pump();")
assert pump.index("rnet_account_pump();") < pump.index("psx_lobby_pump();")
assert "rnet_account_init(" not in pump
assert "psx_lobby_set_display_name(handle);" in pump

assert "return_to_netplay_room\n            ?" in main
assert "ae_np_leave_automatch_room_after_match() ? 0 : 1" in main

print("Lobby authentication ordering and durable-path test passed")
