# Chezz — setting it up

Everything you need to build Chezz, run its tests, and connect it to Firebase, Stripe and an image
host. For what Chezz *is*, see [README.md](README.md).

Contents: [1 Build](#1-build) · [2 Folders](#2-folders) · [3 Firebase](#3-firebase-accounts-and-online-play) · [4 Plans](#4-membership-plans) · [5 Payments](#5-taking-payments) · [6 Image hosting](#6-image-hosting) · [7 Sounds](#7-sounds) · [8 Names](#8-the-150-names) · [9 Art](#9-pieces-board-pictures) · [10 Features](#10-how-each-feature-works) · [11 Tests](#11-tests) · [12 Releasing](#12-releasing-on-github) · [13 Sharing the app](#13-giving-the-app-to-other-people) · [14 Limits](#14-known-limits)

---

## 1. Build

**You need:** MinGW-w64 `g++` and `windres` on PATH (WinLibs: `winget install BrechtSanders.WinLibs.POSIX.UCRT`).

```bat
build.bat
```

It produces one self-contained `Chezz.exe` (about 6 MB). Everything under `assets\` — all four piece sets, the sounds, the icon — is compiled into it by `tools\make_rc.ps1`, so the exe runs on its own in an empty folder.

A loose file in `assets\` still wins over the copy inside the exe, so you can try new artwork or a new sound without rebuilding. Run `build.bat` again to bake it in, and after changing anything in `src\` or `src\config.h`.

`py tools\make_icon.py` redraws `assets\icon.ico` from the classic knight.

## 2. Folders

| Path | What it holds |
|---|---|
| `Chezz.exe` | The app. |
| `assets\` | Your art and sounds (sections 7 and 9). **Optional:** a copy of everything here is inside the exe. |
| `data\` | Everything personal to this PC: sign-in (`account.ini`), the brain (`brain.bin`, `memory.bin`), habits (`habits.bin`, `yourmoves.bin`), settings, puzzle progress, picture cache. Delete a file to reset that thing. Made next to the exe, or in `%LOCALAPPDATA%\Chezz` when that folder cannot be written to. |
| `src\` | Source. `main.cpp` includes the `.inl` screens. |
| `firestore.rules`, `firebase.json`, `.firebaserc` | Server rules and project link. |
| `names.txt` | Optional, next to the exe or in `data\`: 150 bot names (section 8). |

## 3. Firebase (accounts and online play)

The app is already connected to the project **`chezz-game3`**, with email sign-in, guest sign-in, Firestore and the rules in place. You only need this section to move to another project or to redeploy.

**From scratch on a new project**

1. Create a project at https://console.firebase.google.com.
2. Project settings → Your apps → add a **Web app**. Copy its `apiKey` and `projectId`.
3. Put them in `src\firebase.cpp`, near the top:
   ```cpp
   static const char* API_KEY = "...";
   static const char* DOCS = "/v1/projects/YOUR-PROJECT-ID/databases/(default)/documents";
   ```
4. Put the project id in `.firebaserc`.
5. Enable the Firestore API: open `https://console.developers.google.com/apis/api/firestore.googleapis.com/overview?project=YOUR-PROJECT-ID` and press **Enable**. Wait a couple of minutes.
6. Create the database, turn on sign-in, deploy the rules:
   ```bat
   npx -y firebase-tools@latest login
   npx -y firebase-tools@latest firestore:databases:create "(default)" --location nam5 --edition standard --project YOUR-PROJECT-ID
   npx -y firebase-tools@latest deploy --only auth,firestore --project YOUR-PROJECT-ID
   ```
   `firebase.json` turns on **email/password** (members) and **anonymous** (guests).
7. `build.bat`.

**After editing `firestore.rules`**

```bat
npx -y firebase-tools@latest deploy --only firestore --project chezz-game3
```

**What is stored where** (Firestore collections)

| Collection | Contents | Who can write |
|---|---|---|
| `users/{uid}` | Member card: name, motto, picture, banner, rating, record | That member |
| `users/{uid}/friends`, `/blocked` | Their lists | That member |
| `users/{uid}/requests` | Friend-request inbox | One note per sender, unless blocked |
| `chats/{a_b}` + `/messages` | Direct messages | The two members, unless blocked |
| `games/{id}` | Online tables and games | The two players |
| `plans/{uid}` | That member's plan | **Only you** (console) |
| `config/app` | Developer chat account, upgrade link | **Only you** (console) |
| `engines/{id}` | Member-built engines | Ruby owner; anyone may add 1 to the play count |
| `puzzles/{id}` | Published puzzles | Silver and up |
| `teams/{id}` | Teams | Silver and up; captain manages |
| `tournaments/{id}` + `/results` | Events and results | Ruby host; Silver and up register; results must match the finished game |

## 4. Membership plans

| | Bronze (free) | Silver 4.99/mo | Gold 9.99/mo | Ruby 17.99/mo |
|---|---|---|---|---|
| 150 ladder bots, member-built engines | yes | yes | yes | yes |
| Daily puzzles | 1 | 5 | unlimited | unlimited |
| Publish puzzles | no | 1 a day | 12 a day | unlimited |
| Profile pictures | 24 presets | 48 + your own photo + banners | same | same |
| Online play | random opponent, friends | plus choose any waiting opponent | same | same |
| Tournaments | no | play | play | play and **host** |
| Teams | no | found, join, manage | same | same |
| Your own piece images | no | no | yes | yes |
| Build engines | no | no | no | yes |
| Chat with the developers | no | no | no | yes |
| Badge | none | Silver | Gold | Ruby |

**Founding members: Ruby for life**

The first five member accounts ever created have Ruby permanently, with no plan document needed.

- Founders 1 and 2 are the accounts that already existed (GoobieChezz and DadMan3000). Their ids are written into `firestore.rules` (`firstFounders`) and `srcirebase.cpp` (`FIRST_FOUNDERS`).
- Founders 3, 4 and 5 are the next three people to **join** with this version. Joining adds their id to the document `config/founders` automatically.
- The server lets a member add only their own id, only while fewer than three are listed, and never lets an entry be removed or replaced. After the third, the list is closed.
- Guests never count. To see who holds the places, open Firestore → `config` → `founders`.
- To take a place back, delete that id from `config/founders` in the Firebase console (the app cannot).

**Give someone a plan (by hand)**

1. Find their user id: Firebase console → Authentication → Users → copy the **User UID**.
2. Firestore → Data → **Start collection** `plans` (first time only) → **Document ID** = that UID.
3. Add a field `plan` (string) with value `silver`, `gold` or `ruby`.
4. They see it the next time they sign in or open the Plans page (gem icon).

To end a plan, delete the document or set `plan` to `bronze`.

**Set up the developer chat and the upgrade button**

Firestore → collection `config` → document `app`, string fields:

| Field | Value |
|---|---|
| `devUid` | The UID of the account Ruby members should message |
| `devName` | The name shown, e.g. `Chezz developers` |
| `upgradeUrl` | Your payment page (must start with `https://`). Leave it out to hide the button. |

## 5. Taking payments

Members pick a plan on the Plans page (the gem icon). The app opens your payment page in their
browser with their membership id attached, then watches for the plan to switch on. Chezz itself
never sees a card number and holds no payment keys.

Three things have to exist: payment pages to send people to, a webhook that hears back from the
payment provider, and the links written into `config/app` so the app knows where to send them.

### 5.1 Stripe: prices, payment links and the customer portal

1. Create a Stripe account and switch to **Test mode** while you set this up.
2. Product catalogue → add three products, each with a **recurring monthly** price:
   Chezz Silver 4.99, Chezz Gold 9.99, Chezz Ruby 17.99. Note each **price id** (`price_...`).
3. Payment links → create one link per price. Under *After payment*, show a confirmation page that
   says the member can return to Chezz. Note the three link URLs.
4. Settings → Billing → Customer portal: turn it on, allow cancelling and switching plan, and copy
   the portal login link. This is what "Manage or cancel" opens.

### 5.2 The webhook (free, no card needed)

`server/worker.js` is a complete Cloudflare Worker. It checks Stripe's signature, works out the plan
from the price id and writes `plans/{uid}` in Firestore with a Google service account.

1. Google service account: Firebase console → Project settings → Service accounts → **Generate new
   private key**. Keep the JSON file safe; it can write your whole database.
2. Install the tool and sign in (a free Cloudflare account, no card):
   ```bat
   npm install -g wrangler
   wrangler login
   ```
3. Put the three price ids and your project id into `server/wrangler.toml`.
4. From the `server` folder, set the two secrets and deploy:
   ```bat
   wrangler secret put GOOGLE_SERVICE_ACCOUNT
   wrangler secret put STRIPE_WEBHOOK_SECRET
   wrangler deploy
   ```
   For `GOOGLE_SERVICE_ACCOUNT` paste the whole JSON file as one line. For the Stripe secret, first
   create the webhook (next step) and paste the signing secret it gives you, then run that command
   again.
5. Stripe → Developers → Webhooks → add an endpoint at the URL `wrangler deploy` printed, listening
   for `checkout.session.completed`, `customer.subscription.created`, `customer.subscription.updated`
   and `customer.subscription.deleted`. Copy its signing secret (`whsec_...`) into the secret above.
6. Visit the worker's URL in a browser: it should answer *"Chezz billing webhook is running."*

### 5.3 Tell the app where to send people

Firestore → `config` → document `app`, string fields:

| Field | Value |
|---|---|
| `buySilver` | the Silver payment link |
| `buyGold` | the Gold payment link |
| `buyRuby` | the Ruby payment link |
| `portalUrl` | the customer portal login link |
| `devUid`, `devName` | the account Ruby members message (section 4) |

The app reads these each time the Plans page opens, so prices and links can change without a new
build. With no links set, the Plans page says paid plans are not for sale yet and falls back to you
setting `plans/{uid}` by hand.

### 5.4 Try it

With Stripe still in test mode, open Plans in the app, choose Gold, and pay with the test card
`4242 4242 4242 4242`, any future expiry date and any CVC. Within a few seconds the app should say
*"Thank you. You are a Gold member now."* If it does not:

- Stripe → Developers → Webhooks shows each delivery and the worker's reply.
- `wrangler tail` shows the worker's own log live.
- A `503` reply is normal once: Stripe can send the subscription event before the checkout event, so
  the worker asks for it again a moment later.

When it all works, repeat steps 5.1 and 5.5 in **live mode** (new links, new webhook, new signing
secret) and put the live links in `config/app`.

### 5.5 What this does and does not do

- Cancelling is done by the member in the customer portal. Stripe sends
  `customer.subscription.deleted` when the subscription actually ends, and the worker sets them back
  to Bronze.
- Failed renewals arrive as `customer.subscription.updated` with a status like `past_due`, and the
  worker treats anything that is not `active` or `trialing` as Bronze.
- Nobody can give themselves a plan: `plans/{uid}` is read-only to every client, and the worker is
  the only writer. The app's `--nettest` checks this.
- Founding members (the first five accounts) are Ruby regardless of any payment, and the Plans page
  offers them nothing to buy.
- Refunds, VAT and invoices are Stripe's side of things; Chezz only reads the resulting plan.

## 6. Image hosting

**Recommended service: Cloudinary** (https://cloudinary.com). Its free plan needs no card, gives 25 credits a month (1 credit = 1 GB of storage, or 1 GB of image delivery, or 1,000 transformations), and it accepts uploads straight from a desktop app.

Firebase's own Cloud Storage was the other candidate, but since late 2024 it only works on projects with the paid Blaze plan.

**Setup (three steps)**

1. Create a Cloudinary account. On the dashboard, note your **cloud name**.
2. Settings → Upload → **Upload presets** → Add:
   - Signing mode: **Unsigned**
   - Folder: `chezz`
   - Allowed formats: `png, jpg`
   - Max file size: 3 MB; incoming transformation: limit to 512 × 512
   - Save, and note the preset's name.
3. Put both in `src\config.h` and rebuild:
   ```cpp
   #define CHEZZ_CLOUDINARY_CLOUD "your-cloud-name"
   #define CHEZZ_CLOUDINARY_PRESET "your-preset-name"
   ```

**What uses it**

- **Profile pictures** (Silver and up): My Card → UPLOAD A PICTURE. The app uploads the file, stores the returned address on the member's card, and other members' apps download it once into `data\cache\`. The app only ever downloads pictures from `res.cloudinary.com`.
- **Piece images** (Gold and up) are **not** uploaded: Settings → MY PIECE IMAGES picks a folder on that member's PC. Only they see their pieces, so nothing needs hosting. If you later want piece sets to follow a member between PCs, the same Cloudinary account can hold them.

**Keep in mind:** an unsigned preset lets anyone who extracts the preset name upload to your account. The limits in step 2 cap the damage; review the `chezz` folder now and then, and delete the preset if it is abused.

## 7. Sounds

Put sound files in `assets\sounds\` and run `build.bat`; they are compiled into the exe.

| File | Plays when |
|---|---|
| `move.wav` | A piece moves |
| `capture.wav` | A capture |
| `check.wav` | Check |
| `castle.wav` | Castling |
| `promote.wav` | Promotion |
| `start.wav` | A game starts |
| `gameover.wav` | A game ends |
| `notify.wav` | A message or friend request arrives |

- Your `move`, `capture` and `check` files are in use. They are MP3 audio with a `.wav` name, which is fine: anything that is not true WAV data is decoded by Windows' own decoder (MP3, AAC/M4A, FLAC, WAV).
- The other five still use the built-in synthesised sounds. Add files with those names to replace them.
- A file in `assets\sounds\` beats the copy inside the exe, so you can try a new sound without rebuilding.
- Volume: Settings → Sound volume.

## 8. The 150 names

The bots are named from your `names.txt`, top line = AI 1, bottom line = AI 150.

**Your file has 140 names, not 150.** To reach 150, ten names in the same style were added as AI 130 to 139, just before your closing run from "The Chessmaster" to "The Perfect Player":

> Grandmaster Hawthorne, Master Pembroke, Sir Hastings, Lord Sterling, Professor Ashford, Dr. Blackwood, Edmund the Relentless, Winston the Unshaken, Maximilian the Precise, Augustus the Undefeated

To use your own ten instead: make `names.txt` exactly 150 lines and put it next to `Chezz.exe` or in `data\`. A file with 150 or more lines replaces the whole list at startup, with no rebuild. A shorter file is ignored. To change the built-in list, edit `NAMES` in `src\levels.cpp`.

## 9. Pieces, board, pictures

See `ASSETS.md` for every accepted file name. In short:

- **Pieces:** `assets\white_king.png` … `black_pawn.png` (12 files).
- **Board:** `assets\board.png`, or `light.png` + `dark.png`. Settings → Board texture → "your image" shows them.
- **Background:** `assets\background.png`.
- **Bot portraits:** `assets\avatars\level_001.png` … or `tier_01.png` … `tier_15.png`.
- **Preset profile pictures:** `assets\avatars\preset_01.png` … `preset_48.png` (1 to 24 for everyone, 25 to 48 for Silver and up).
- **Window icon:** `assets\icon.ico`.

## 10. How each feature works

**Top bar icons** (hover for the name): ladder, engines, puzzles, online room, tournaments, teams, friends, messages, my card, plans, settings, light/dark switch, sign out.

**Ladder.** 150 bots rated 250 to 3500 in steps of about 20. Filter by typing part of a name or rating, by tier, and by beaten / not beaten. Arrow keys and Enter work on the filtered list. The list is drawn once into an off-screen strip, so scrolling only copies pixels.

**The club brain.** The network lives on the server, not on one computer, and every member's games train the same one. Signing in fetches the newest version; finishing a game trains on it here and pushes the result back. The weights go into Firestore as `brain/w0`..`brain/w3` (824 KB is more than one document can hold) with a version note in `brain/meta`. A push is one all-or-nothing write guarded by the version it started from, so two members finishing at the same moment can never blend half of one brain into the other: the loser is refused and takes the winner's work instead. **Share the club brain** in Settings turns it off, and the brain stays on your computer. Training itself always runs on a player's machine, because nothing here needs a server that is awake all the time.

**How it learns.** A neural network (768 inputs → 5 layers of 150 → 1 output, 206,101 weights) adds a learned correction to the hand-written evaluation. After each game it trains on that game until the loss settles, then sweeps everything remembered. A second model learns your habits from your own moves and lets bots anticipate your replies.

**Clocks.** Untimed or 1+0 to 30+0. Bots budget their thinking against their clock.

**Pre-moves** (games against bots and engines, not online). While the bot is thinking, click or drag one of your pieces and pick a square: the move is queued, both squares turn blue, and it plays the instant the bot has moved. If it is no longer legal it is quietly dropped. A pre-moved pawn reaching the last rank always becomes a queen. Right-click, Esc or taking back cancels it.

**Planning with the right button.** Right-drag from one square to another to draw an arrow; right-click a single square for a ring. There is no limit to how many you add, and knight moves are drawn as an L. Hold **Shift** for red, **Ctrl** for blue or **Alt** for orange. Drawing the same mark again rubs it out. Everything is wiped by a left click on the board or by your opponent's move. The marks are yours alone: they are never sent anywhere.

**Resigning** asks "Are you sure you want to resign?" first. Leaving a live online game asks the same, because leaving concedes.

**Online room.**
- RANDOM OPPONENT seats you with someone waiting, or opens a table in your name.
- Host a table for **anyone** or for **one friend** (only they can sit down).
- Silver and up see everyone waiting and choose. Bronze and guests see only tables opened for them.

**Puzzles.**
- Daily puzzles are mate-in-2 or mate-in-3 positions the engine finds in practice games seeded by the date. Every copy of the app produces the same puzzles on the same day, so no server is involved.
- Your move is right if it mates, or keeps a forced mate in the moves that remain. HINT circles the piece; SHOW SOLUTION plays it out.
- Publishing (Silver and up): paste a FEN, or press USE MY BOARD to take the position from your last game. The engine must find a forced mate in 1 to 3 before it is accepted.

**Engines** (Ruby builds, everyone plays). Six dials: how far it looks ahead, thinking time, misjudgements, outright blunders, positional sense, aggression. The app shows an approximate rating. Up to six engines per member. Games against them do not count on the ladder.

**Teams** (Silver and up). Name, 2 to 5 letter tag, motto, up to 20 members. The captain removes members and can disband.

**Tournaments** (Ruby hosts; Silver and up play).
1. The host announces an event: solo or team battle, 3/5/7 rounds, clock, player limit.
2. Members register. For a team battle you must be in a team; your tag is recorded.
3. The host presses START ROUND 1. Pairings are Swiss-style: similar scores meet, no rematches while avoidable, the odd player out gets a bye worth a full point.
4. Each player presses PLAY MY GAME. The first to arrive opens a private table; the second sits down.
5. When a game ends the result is filed automatically. The server only accepts it if it matches the finished game.
6. When every game of the round is in, the host starts the next round, and finally FINISH EVENT.
7. Team battles also rank teams by their best four scores.

**Friends, blocks, messages.** Look members up by the start of their name. A blocked member cannot send you requests or messages and their tables are hidden.

**Settings.** Light / dark / high contrast, accent, table, fonts, text size, board colours and texture, piece size, volume, gliding, coordinates, last-move highlight, legal-move dots, your own piece images (Gold).

## 11. Tests

None of these open a window, and `--data` keeps them away from your real `data\` folder.

```bat
Chezz.exe --data C:\Temp\chezztest --selftest
Chezz.exe --data C:\Temp\chezztest --shots C:\Temp\chezztest\shots
Chezz.exe --data C:\Temp\chezztest --nettest
```

| Command | What it checks | Result file |
|---|---|---|
| `--selftest` | Move generation, levels and names, the brain, search, bot-vs-bot play, habits, audio (including your sound files), puzzle generator, tournament pairings, engine dials, themes | `selftest.txt` |
| `--shots <folder>` | Renders 26 screens to PNG | the PNG files |
| `--nettest` | Live server, read-mostly, using the `account.ini` you copy into the data folder: profile, lookup, lists, plan, and that the rules refuse what they should | `nettest.txt` |
| `--perft` | Move generation only | `perft.txt` |
| `--selfplay N` | The engine plays itself N times and the brain learns | `selfplay.log` |

**Two-account features on a local emulator** (friend requests, messages, tournaments end to end) are not automated yet. The app has a hook for it (`fb::useEmulator` in `src\firebase.h`); the Firebase emulator is a download of about 100 MB (`npx -y firebase-tools@latest emulators:start --only auth,firestore`).

## 12. Releasing on GitHub

`.github/workflows/release.yml` builds the exe on GitHub's own Windows machines, checks it runs on
its own, and attaches it to a release.

```bat
cd C:\Users\Ewan\OneDrive\Desktop\Chezz
git remote add origin https://github.com/<you>/chezz.git
git push -u origin main
git tag v1.0.0
git push origin v1.0.0
```

The Actions tab shows the build. When it finishes, the release page has **Chezz.exe** for anyone to
download. You can also start the workflow by hand from the Actions tab, which leaves the exe as an
artifact without making a release.

`.gitignore` keeps `data\` out of the repository, so your sign-in and your brain are never pushed.

## 13. Giving the app to other people

1. Send them `Chezz.exe`, or a link to your GitHub release. That one file is everything: nothing to install, and no `assets\` folder to copy.
2. Do **not** send your `data\` folder: it holds your sign-in and your brain. The app creates a fresh one.
3. They join at the membership desk (email + password), or sign the visitors' book to play online rooms as a guest.
4. Windows may warn about an unknown publisher the first time, because the exe is not code-signed.

## 14. Known limits

- **Payments are not automated** (section 5). Plans are set in the console.
- **Daily limits are counted on each PC.** The server enforces *who* may publish puzzles, host tournaments, build engines and join teams, but the "1 / 5 puzzles a day" and "1 / 12 published a day" counts live in `data\puzzles_state.txt`.
- **"Early access" (Gold) is listed but gates nothing yet**, because there are no early features to hold back.
- **The club brain is trusted too.** Any member's app can push a new version of it. The app refuses weights of the wrong size or with damaged numbers in them, but a determined member could still push a badly trained brain, and everyone's next push would build on it.
- **Online moves are trusted.** The server checks seats and results, not move legality, so a modified client could send illegal moves.
- **Ratings are targets, not measured Elo.** The 5 × 150 brain costs about 90,000 multiply-adds per position, so the top bots search less deeply than a lean engine would.
- **Tournament hosts must be online to advance rounds**; nothing advances by itself.
- **Team rosters and tournament entries are small lists in one document.** Two people joining in the same second can collide; the second is refused and should press the button again.
