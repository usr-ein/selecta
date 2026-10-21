# v1 QA checklist (Road to v1, step 3)

Run on a fresh, isolated data folder against the local test network (`selecta-devnet`). Where a line says "live
services", run it with `--metadata-network` (D35). A line passes only when the expected result is seen: a screenshot
or log line is kept in `local/qa/`. Any failure is a bug to fix before step 4.

## 0. Setup
- [ ] `cmake --workflow --preset dev` passes, lint clean, CI on main green (step 2).
- [ ] Launch with a new `--data-dir`. The app opens on Get tracks. The sidebar has every view; the icon shows in the
      Dock and Finder.
- [ ] Settings → Soulseek: user, password, server = the devnet; Connect → "Online as …". The port-mapping line shows
      its state; isolated runs say it's off.
- [ ] Settings → Library: a tracks folder and an albums folder; the scan finishes; Collection shows the numbers.

## 1. The main flow (D27)
- [ ] Get tracks: paste 10 lines from the devnet tracklist → each resolves to a release with a length (the devnet has
      no Spotify; live services: real lines).
- [ ] Live services: a Spotify playlist link imports its tracks, each with its release (META-3).
- [ ] The quality menu shows the presets; changing it ranks the found lines again without new searches.
- [ ] Find Them: lines go Searching → Ready / Check / Not found. "Finding · N left · about M min" counts down. The
      toolbar's search budget falls and refills (2 a minute, bursts of 6).
- [ ] Space on a Ready line previews (YouTube with live services; the peer's file on the devnet). The waveform fills;
      ⌥←/⌥→ seek. K keeps the file and D discards it. A previewed file plays from the cache after a relaunch, with no
      new transfer.
- [ ] Get N: downloads spread over peers that have the same file. Activity → Transfers shows the states. Each line
      arrives in Inbox/Singles as an MP3 320 with tags, cover and the quarantine attribute.
- [ ] A FLAC made from a 128 kbps MP3 arrives with "⚠ Likely from a ≈ 128 kbps MP3", and the inspector shows the
      spectrogram with the cut-off.
- [ ] Export Not Found writes the unresolved lines.

## 2. Search and Browse
- [ ] ⌘F search: the release card, the results streamed and ranked, "N of M shown" with the reasons.
- [ ] The Filters and the Presets menu: save, rename, delete, use by default; the minimum peer speed hides slow peers.
- [ ] Download a file; Download Folder as Album → one album folder in Inbox/Albums with its cover; DL-12 offer when
      several tracks of one folder are picked.
- [ ] Browse a peer's files from a result; download a folder; the list is kept a day.
- [ ] Add to Wishlist keeps the preset; a run counts only files that pass it.

## 3. Inbox, Collection, History
- [ ] The inspector shows the tags, cover, provenance and spectrum. Edit tags; choose another release; Convert….
- [ ] File: singles go to the tracks folder and albums to the albums folder, with the naming templates; the source is
      gone from the Inbox; the collection file is never touched again. Collection counts one album per album folder.
- [ ] Move to Trash, then Undo. An album that holds an Inbox single trashes the single, with Undo. A filed single only
      gets the "also on" flag.
- [ ] Match corrections (LIB-6): "Not What I Have" removes a badge, and it survives a relaunch and a rescan.
- [ ] History lists the session's downloads with where each is now.

## 4. Network views
- [ ] Activity: Transfers (cancel, retry), Peers (today, limits), Log (every search, request and refusal with a
      reason). Pause network activity stops new searches and transfers and survives a relaunch.
- [ ] Shares: what's shared, uploads to a devnet peer, the ratio. Ban a peer → it gets "Banned".
- [ ] Messages: write to a devnet peer and get its answer; unread counts in the sidebar and on the Dock; People:
      buddy online status, ignore, ban.
- [ ] Rooms: list, join "selecta-devnet", post, a mention notifies, leave; rejoined after a relaunch.
- [ ] Notifications (a real session): a message while Selecta is in the background shows a banner, and clicking it
      opens the conversation.

## 5. Converter, Settings, caches
- [ ] Converter: FLAC 24/96 → AIFF 16/44.1 with dither; → ALAC; MP3 320 → MP3 320 is copied as is; the warnings show
      before converting.
- [ ] Settings → Caches: sizes and Empty for each.
- [ ] Spotify (live services): the terms notice once; Sign In in the Browser, with Open Again if the page was lost.

## 6. Accessibility and keyboard (UI-5)
- [ ] VoiceOver reads the sidebar, toolbar, player transport, badges and the waveform ("1:23 of 6:12").
- [ ] Every view works from the keyboard: Tab order, Return, Space, ⌫, D, K.

## 7. Performance (NFR-2) and safety
- [ ] Filter + sort of 10 000 results < 50 ms (benchmark); waveform analysis of a 6-minute track < 300 ms.
- [ ] The governor's limits are in the Log; no automated searches against the live network (CLAUDE.md).
