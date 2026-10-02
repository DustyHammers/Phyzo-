# Installing a Phyzo build on your Mac

Every push to `main` and every pull request builds the plugin on GitHub Actions (Apple Silicon, VST3 and AU,
ad-hoc signed). This guide takes you from that build to Phyzo loaded in REAPER.

## 1. Download the build from GitHub

You must be signed in to GitHub to download builds.

1. Open https://github.com/DustyHammers/Phyzo-/actions
2. In the left column, click **CI**.
3. Click the newest run with a green tick for the branch you want: `main` for merged work, or the pull request
   you are testing (its title is shown on the run).
4. On the run's page, scroll down to **Artifacts** and click the file named like
   `Phyzo-0.2.0-macOS-1a2b3c4.zip`. The last part is the commit it was built from.

From a pull request page you can also get there with the **Checks** tab, then **CI** on the left, then **Summary**.

Builds are kept for 30 days. If the newest run has a red cross, its plugin was not built; use an older green run.

## 2. Unzip

Double-click the zip in your Downloads folder (Safari may already have unzipped it). You get a folder containing
`Phyzo.vst3`, `Phyzo.component` and `INSTALL.txt`.

## 3. Install

1. Quit REAPER.
2. In Finder, choose **Go > Go to Folder…** (Shift-Command-G), paste `~/Library/Audio/Plug-Ins/VST3` and press Return.
   If Finder says the folder does not exist, go to `~/Library/Audio/Plug-Ins` instead and create a folder named `VST3`.
3. Drag `Phyzo.vst3` into it. If an older Phyzo is there, choose **Replace**.
4. Do the same for `Phyzo.component` with the folder `~/Library/Audio/Plug-Ins/Components`.

## 4. Clear the quarantine flag

macOS marks everything downloaded from the internet as "quarantined" and refuses to load plugins that are not
notarised by Apple. Phyzo is ad-hoc signed, not notarised, so the mark must be removed:

1. Open **Terminal** (Applications > Utilities > Terminal).
2. Paste this line and press Return (it prints nothing when it works):

       xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Phyzo.vst3 ~/Library/Audio/Plug-Ins/Components/Phyzo.component

3. Paste this line and press Return, so macOS re-reads its Audio Unit list ("No matching processes" is fine):

       killall -9 AudioComponentRegistrar

Repeat step 4 every time you install a new build.

## 5. Load it in REAPER

1. Start REAPER. It scans new plugins at start-up.
2. Insert a track, click its **FX** button and type `Phyzo` in the filter box. You should see
   **VST3i: Phyzo (DHammers)** and an **AUi** entry for Phyzo.
3. If Phyzo is missing: **REAPER > Settings… > Plug-ins > VST**, click **Re-scan** (or **Clear cache/re-scan**), and
   restart REAPER. For the AU, check **Plug-ins > AU** is enabled.

If REAPER reports that the plugin failed to load, repeat step 4 and restart REAPER.

## Uninstalling

Quit REAPER and move `~/Library/Audio/Plug-Ins/VST3/Phyzo.vst3` and
`~/Library/Audio/Plug-Ins/Components/Phyzo.component` to the Bin.
