#pragma once
#include "StdInclude.hpp"

// BO2's theater as core sees a demo: whether a film plays, its name, file, time and length, and its pause kept in step
// with the editor's. Addresses come from Signatures.hpp; what they hold is in RE-NOTES.md.
namespace IWXMVM::T6::Theater
{
    // a film plays and has loaded (the theater's structure and cg exist)
    bool IsFilmPlaying();

    // the film's time in ms: the frame's (cg->time), 0 while none plays
    std::int32_t GetFilmTime();

    // the time of the film's latest snapshot, which steps 10 times a second
    std::int32_t GetSnapshotTime();

    // the film's title as the theater has it ("Film" unless it was named)
    std::string GetFilmTitle();

    // the film's file's name without ".demo", or its title when the file wasn't found
    std::string GetFilmName();

    // the film's file in Redacted's film folder, empty when it isn't there
    std::filesystem::path GetFilmPath();

    // when the film starts and ends (ms, film time), from its file; none when it can't be read
    struct FilmTimes
    {
        std::int32_t start;
        std::int32_t end;
    };
    std::optional<FilmTimes> GetFilmTimes();

    // Moves the film to a time (ms, film time): back with the theater's demo_back when it's earlier (to the last
    // snapshot before it), then on at a raised timescale to the time itself; the speed and pause are put back after.
    // Asked for as often as every frame while the timeline is dragged; OnFrame carries it out.
    void SeekTo(std::int32_t filmTime);

    // the time a seek is going to, while one is asked for or under way: the timeline shows it, not the film's way there
    // (a jump to the start and the fast play on), so its playhead stays where it was put
    std::optional<std::int32_t> GetSeekTarget();

    // a seek is asked for or under way
    bool IsSeeking();
    // the timeline is being dragged (seeks asked one after another): the film is shown as it goes, not held
    bool IsScrubbing();

    // The theater's HUD (what F2 toggles: the killfeed, score and the rest) and its controls (F4: the bar with the
    // timeline and buttons), as hidden flags in its structure (theater + 0x63569 and + 0x6356A, 1 hidden; its key
    // handler 0x5F3E30 toggles them, and its screenshot sets both to 1 for a moment, 0x92B5BA). While the editor is
    // shown or a recording runs, OnFrame keeps the controls hidden and the HUD as the Look tab's 2D Elements says;
    // otherwise (F1) the theater's own settings are back, so F2 and F4 work as in the game.
    void SetHudShown(bool shown);

    // Once a frame: tells core when a film starts and stops (PreDemoLoad, PostDemoLoad) and keeps its pause and the
    // theater's demo_pause the same, whichever of them changed. While core records (CaptureManager), the film plays
    // (demo_pause off) and each frame is the capture's frame time with fixedtime (0x2562B40; Com_Frame, 0x88CBC0, then
    // uses it as the frame's msec in place of the real time and the timescale); during a seek fixedtime is 0 again, so
    // the seek's fast play works, and it's 0 once the capture ends.
    void OnFrame();

    // Plays a film from the Theater's lobby the way its Start Film does: ui_demoname set to the film, then
    // xpartyplaydemo, which sets up the lobby's players for it (0x675E90) and sends demo_play <ui_demoname>. demo_play
    // without the lobby's players (from the main menu) crashed the game, so outside the lobby nothing is sent. Returns
    // why it didn't play, empty when it did.
    std::string PlayFilmFromLobby(const std::filesystem::path& file);
    // the hook that keeps the party of the lobby the Demo tab opened from starting a match (xpartygo)
    void InstallHooks();

    // Reads a film's start and end from its file: the last chunk is the metadata, which starts u32 version,
    // u32 start, u32 end. Exposed for the probe and tests.
    std::optional<FilmTimes> ReadFilmTimes(const std::filesystem::path& path);
}  // namespace IWXMVM::T6::Theater
