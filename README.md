# Spotify Now Playing for OBS

An OBS Studio source (Windows only) that shows the song currently playing in Spotify, styled after the now-playing corner of the Spotify desktop player bar: rounded album art, the title in white and the artist in grey on a black background.

Song info and album art come from Windows' media controls (the same data as the volume flyout), so there's no Spotify login or API key.

## Install

1. Download the `windows-x64` zip from the latest build on the **Actions** tab (or a release).
2. Close OBS.
3. Extract the zip into `C:\ProgramData\obs-studio\plugins\` so you end up with `C:\ProgramData\obs-studio\plugins\obs-spotify-now-playing\bin\64bit\obs-spotify-now-playing.dll`.
4. Start OBS and add a **Spotify Now Playing** source.

## Settings

| Setting | Default | Notes |
|---|---|---|
| Width | 384 | Width of the bar in 1x units; long titles are cut off with an ellipsis |
| Scale | 1.0 | Renders at this multiple, so text stays sharp when the bar is large |
| Title font / size / colour | Segoe UI Semibold, 15 px, white | |
| Artist font / size / colour | Segoe UI Semibold, 13 px, #B3B3B3 | |
| Background colour | Black, opaque | Supports transparency |
| Background corner radius | 0 | |
| Album art corner radius | 4 | |
| Hide when paused | Off | |
| Hide when Spotify isn't running | On | |

## Building

GitHub Actions builds the plugin on every push to `main` (see `.github/workflows`). To build locally you need Visual Studio 2022 with the C++ workload and CMake 3.28+:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```
