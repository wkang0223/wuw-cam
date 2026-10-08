# BWO: naming your pilgrim

The BWO character is Q until the player picks a name. Rules (the same on the camera and in the
browser preview): A-Z, 0-9, space, `-`, `_` and `.`; upper case; no leading, trailing or doubled
spaces; at most 12 characters; empty means Q.

- **Camera:** BWO > STYLE > NAME row opens the on-screen keyboard (empty + OK resets to Q). The name
  is kept in flash (`Preferences` namespace `bwoworld`, key `name`), survives a reboot and a world
  reset, and is drawn as a small tag under the portrait box in the game HUD.
- **Browser preview** (`site/game-preview`): WORLD STYLE > Pilgrim name field, saved in
  `localStorage.wuwBwoName`, shown above VITAL in the HUD.
- Rules live in `bwo_name.h` (C++) and `site/game-preview/pilgrim-name.js`; tests:
  `g++ -std=c++17 -Wall -Wextra -I. tools/bwo_name_test.cpp -o /tmp/t && /tmp/t` and
  `node tools/test_pilgrim_name.cjs`.
- Checked: both tests pass, the firmware compiles, and the preview was driven in headless Chrome
  (default Q, a typed name is cleaned, saved, and still shown after a reload). The on-screen keyboard
  and the name tag on the real TFT have not been tried on a camera.
