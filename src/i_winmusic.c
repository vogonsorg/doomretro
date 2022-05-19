﻿/*
========================================================================

                           D O O M  R e t r o
         The classic, refined DOOM source port. For Windows PC.

========================================================================

  Copyright © 1993-2022 by id Software LLC, a ZeniMax Media company.
  Copyright © 2013-2022 by Brad Harding <mailto:brad@doomretro.com>.

  DOOM Retro is a fork of Chocolate DOOM. For a list of acknowledgments,
  see <https://github.com/bradharding/doomretro/wiki/ACKNOWLEDGMENTS>.

  This file is a part of DOOM Retro.

  DOOM Retro is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation, either version 3 of the license, or (at your
  option) any later version.

  DOOM Retro is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with DOOM Retro. If not, see <https://www.gnu.org/licenses/>.

  DOOM is a registered trademark of id Software LLC, a ZeniMax Media
  company, in the US and/or other countries, and is used without
  permission. All other trademarks are the property of their respective
  holders. DOOM Retro is in no way affiliated with nor endorsed by
  id Software.

========================================================================
*/

#if defined(_WIN32)

#include <Windows.h>
#include <mmsystem.h>

#include "c_console.h"
#include "doomtype.h"
#include "m_misc.h"
#include "midifile.h"
#include "SDL.h"
#include "SDL_mixer.h"

static HMIDISTRM        hMidiStream;
static HANDLE           hBufferReturnEvent;
static HANDLE           hExitEvent;
static HANDLE           hPlayerThread;

// This is a reduced Windows MIDIEVENT structure for MEVT_F_SHORT type of events.
typedef struct
{
    DWORD               dwDeltaTime;
    DWORD               dwStreamID; // always 0
    DWORD               dwEvent;
} native_event_t;

typedef struct
{
    native_event_t      *native_events;
    int                 num_events;
    int                 position;
    bool                looping;
} win_midi_song_t;

static win_midi_song_t  song;

typedef struct
{
    midi_track_iter_t   *iter;
    int                 absolute_time;
} win_midi_track_t;

static float    volume_factor = 1.0f;

// Save the last volume for each MIDI channel.
static int      channel_volume[MIDI_CHANNELS_PER_TRACK];

// Macros for use with the Windows MIDIEVENT dwEvent field.
#define MIDIEVENT_CHANNEL(x)    (x & 0x0000000F)
#define MIDIEVENT_TYPE(x)       (x & 0x000000F0)
#define MIDIEVENT_DATA1(x)     ((x & 0x0000FF00) >> 8)
#define MIDIEVENT_VOLUME(x)    ((x & 0x007F0000) >> 16)

// Maximum of 4 events in the buffer for faster volume updates.
#define STREAM_MAX_EVENTS       4

typedef struct
{
    native_event_t  events[STREAM_MAX_EVENTS];
    int             num_events;
    MIDIHDR         MidiStreamHdr;
} buffer_t;

static buffer_t buffer;

// Message for midiStream errors.
static void MidiErrorMessage(DWORD dwError)
{
    char    szErrorBuf[MAXERRORLENGTH];

    midiOutGetErrorText(dwError, (LPSTR)szErrorBuf, MAXERRORLENGTH);
    C_Warning(2, "midiStream Error: %s", szErrorBuf);
}

// Fill the buffer with MIDI events, adjusting the volume as needed.
static void FillBuffer(void)
{
    int i;

    for (i = 0; i < STREAM_MAX_EVENTS; i++)
    {
        native_event_t  *event = &buffer.events[i];

        if (song.position >= song.num_events)
        {
            if (song.looping)
                song.position = 0;
            else
                break;
        }

        *event = song.native_events[song.position];

        if (MIDIEVENT_TYPE(event->dwEvent) == MIDI_EVENT_CONTROLLER
            && MIDIEVENT_DATA1(event->dwEvent) == MIDI_CONTROLLER_MAIN_VOLUME)
        {
            int volume = MIDIEVENT_VOLUME(event->dwEvent);

            channel_volume[MIDIEVENT_CHANNEL(event->dwEvent)] = volume;
            event->dwEvent = ((event->dwEvent & 0xFF00FFFF)
                | (((int)((float)volume * volume_factor) & 0x7F) << 16));
        }

        song.position++;
    }

    buffer.num_events = i;
}

// Queue MIDI events.
static void StreamOut(void)
{
    MIDIHDR     *hdr = &buffer.MidiStreamHdr;
    MMRESULT    mmr;
    int         num_events = buffer.num_events;

    if (!num_events)
        return;

    hdr->lpData = (LPSTR)buffer.events;
    hdr->dwBytesRecorded = num_events * sizeof(native_event_t);

    if ((mmr = midiStreamOut(hMidiStream, hdr, sizeof(MIDIHDR))) != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);
}

// midiStream callback.
static void CALLBACK MidiStreamProc(HMIDIIN hMidi, UINT uMsg,
    DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    if (uMsg == MOM_DONE)
        SetEvent(hBufferReturnEvent);
}

// The Windows API documentation states: "Applications should not call any
// multimedia functions from inside the callback function, as doing so can
// cause a deadlock." We use a thread to avoid possible deadlocks.
static DWORD WINAPI PlayerProc(void)
{
    HANDLE  events[2] = { hBufferReturnEvent, hExitEvent };

    while (true)
        switch (WaitForMultipleObjects(2, events, FALSE, INFINITE))
        {
            case WAIT_OBJECT_0:
                FillBuffer();
                StreamOut();

                break;

            case WAIT_OBJECT_0 + 1:
                return 0;
        }

    return 0;
}

// Convert a multi-track MIDI file to an array of Windows MIDIEVENT structures.
static void MIDItoStream(midi_file_t *file)
{
    int                 num_tracks = MIDI_NumTracks(file);
    int                 current_time = 0;
    win_midi_track_t    *tracks = malloc(num_tracks * sizeof(win_midi_track_t));

    for (int i = 0; i < num_tracks; i++)
    {
        tracks[i].iter = MIDI_IterateTrack(file, i);
        tracks[i].absolute_time = 0;
    }

    song.native_events = calloc(MIDI_NumEvents(file), sizeof(native_event_t));

    while (true)
    {
        midi_event_t    *event;
        DWORD           data = 0;
        int             min_time = INT_MAX;
        int             idx = -1;

        // Look for an event with a minimal delta time.
        for (int i = 0; i < num_tracks; i++)
        {
            int time;

            if (!tracks[i].iter)
                continue;

            time = tracks[i].absolute_time + MIDI_GetDeltaTime(tracks[i].iter);

            if (time < min_time)
            {
                min_time = time;
                idx = i;
            }
        }

        // No more MIDI events left, end the loop.
        if (idx == -1)
            break;

        tracks[idx].absolute_time = min_time;

        if (!MIDI_GetNextEvent(tracks[idx].iter, &event))
        {
            free(tracks[idx].iter);
            tracks[idx].iter = NULL;

            continue;
        }

        switch (event->event_type)
        {
            case MIDI_EVENT_META:
                if (event->data.meta.type == MIDI_META_SET_TEMPO)
                    data = (event->data.meta.data[2]
                        | (event->data.meta.data[1] << 8)
                        | (event->data.meta.data[0] << 16)
                        | (MEVT_TEMPO << 24));

                break;

            case MIDI_EVENT_NOTE_OFF:
            case MIDI_EVENT_NOTE_ON:
            case MIDI_EVENT_AFTERTOUCH:
            case MIDI_EVENT_CONTROLLER:
            case MIDI_EVENT_PITCH_BEND:
                data = (event->event_type
                    | event->data.channel.channel
                    | (event->data.channel.param1 << 8)
                    | (event->data.channel.param2 << 16)
                    | (MEVT_SHORTMSG << 24));

                break;

            case MIDI_EVENT_PROGRAM_CHANGE:
            case MIDI_EVENT_CHAN_AFTERTOUCH:
                data = (event->event_type
                    | event->data.channel.channel
                    | (event->data.channel.param1 << 8)
                    | (MEVT_SHORTMSG << 24));

                break;

            case MIDI_EVENT_SYSEX:
            case MIDI_EVENT_SYSEX_SPLIT:
                break;
        }

        if (data)
        {
            native_event_t  *native_event = &song.native_events[song.num_events];

            native_event->dwDeltaTime = min_time - current_time;
            native_event->dwStreamID = 0;
            native_event->dwEvent = data;

            song.num_events++;
            current_time = min_time;
        }
    }

    if (tracks)
        free(tracks);
}

bool I_Windows_InitMusic(void)
{
    UINT        MidiDevice = MIDI_MAPPER;
    MIDIHDR     *hdr = &buffer.MidiStreamHdr;
    MMRESULT    mmr;

    if ((mmr = midiStreamOpen(&hMidiStream, &MidiDevice, (DWORD)1, (DWORD_PTR)&MidiStreamProc,
        (DWORD_PTR)NULL, CALLBACK_FUNCTION)) != MMSYSERR_NOERROR)
    {
        MidiErrorMessage(mmr);
        return false;
    }

    hdr->lpData = (LPSTR)buffer.events;
    hdr->dwBytesRecorded = 0;
    hdr->dwBufferLength = STREAM_MAX_EVENTS * sizeof(native_event_t);
    hdr->dwFlags = 0;
    hdr->dwOffset = 0;

    if ((mmr = midiOutPrepareHeader((HMIDIOUT)hMidiStream, hdr, sizeof(MIDIHDR))) != MMSYSERR_NOERROR)
    {
        MidiErrorMessage(mmr);
        return false;
    }

    hBufferReturnEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    hExitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    return true;
}

void I_Windows_SetMusicVolume(int volume)
{
    volume_factor = (float)volume / MIX_MAX_VOLUME;

    // Send MIDI controller events to adjust the volume.
    for (int i = 0; i < MIDI_CHANNELS_PER_TRACK; i++)
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (MIDI_CONTROLLER_MAIN_VOLUME << 8)
            | ((int)((float)channel_volume[i] * volume_factor) << 16)));
}

void I_Windows_StopSong(void)
{
    MMRESULT    mmr;

    if (hPlayerThread)
    {
        SetEvent(hExitEvent);
        WaitForSingleObject(hPlayerThread, INFINITE);

        CloseHandle(hPlayerThread);
        hPlayerThread = NULL;
    }

    for (int i = 0; i < MIDI_CHANNELS_PER_TRACK; i++)
    {
        // RPN sequence to adjust pitch bend range (RPN value 0x0000)
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x65 << 8)));
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x64 << 8)));

        // reset pitch bend range to central tuning +/- 2 semitones and 0 cents
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x06 << 8) | (0x02 << 16)));
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x26 << 8)));

        // end of RPN sequence
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x64 << 8) | (0x7F << 16)));
        midiOutShortMsg((HMIDIOUT)hMidiStream, (MIDI_EVENT_CONTROLLER | i | (0x65 << 8) | (0x7F << 16)));
    }

    if ((mmr = midiStreamStop(hMidiStream)) != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);

    if ((mmr = midiOutReset((HMIDIOUT)hMidiStream)) != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);
}

void I_Windows_PlaySong(bool looping)
{
    MMRESULT    mmr;

    song.looping = looping;

    if ((hPlayerThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)&PlayerProc, 0, 0, 0)))
        SetThreadPriority(hPlayerThread, THREAD_PRIORITY_TIME_CRITICAL);

    if ((mmr = midiStreamRestart(hMidiStream)) != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);
}

void I_Windows_PauseSong(void)
{
    MMRESULT    mmr = midiStreamPause(hMidiStream);

    if (mmr != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);
}

void I_Windows_ResumeSong(void)
{
    MMRESULT    mmr = midiStreamRestart(hMidiStream);

    if (mmr != MMSYSERR_NOERROR)
        MidiErrorMessage(mmr);
}

void I_Windows_RegisterSong(void *data, int size)
{
    SDL_RWops       *rwops = SDL_RWFromMem(data, size);
    midi_file_t     *file = MIDI_LoadFile(rwops);
    MIDIPROPTIMEDIV timediv;
    MIDIPROPTEMPO   tempo;
    MMRESULT        mmr;

    if (!file)
    {
        C_Warning(2, "I_Windows_RegisterSong: Failed to load MID.");
        return;
    }

    // Initialize channels volume.
    for (int i = 0; i < MIDI_CHANNELS_PER_TRACK; i++)
        channel_volume[i] = 100;

    timediv.cbStruct = sizeof(MIDIPROPTIMEDIV);
    timediv.dwTimeDiv = MIDI_GetFileTimeDivision(file);

    if ((mmr = midiStreamProperty(hMidiStream, (LPBYTE)&timediv, (MIDIPROP_SET | MIDIPROP_TIMEDIV))) != MMSYSERR_NOERROR)
    {
        MidiErrorMessage(mmr);
        return;
    }

    // Set initial tempo.
    tempo.cbStruct = sizeof(MIDIPROPTIMEDIV);
    tempo.dwTempo = 500000; // 120 BPM

    if ((mmr = midiStreamProperty(hMidiStream, (LPBYTE)&tempo, (MIDIPROP_SET | MIDIPROP_TEMPO))) != MMSYSERR_NOERROR)
    {
        MidiErrorMessage(mmr);
        return;
    }

    MIDItoStream(file);

    MIDI_FreeFile(file);

    ResetEvent(hBufferReturnEvent);
    ResetEvent(hExitEvent);

    FillBuffer();
    StreamOut();
}

void I_Windows_UnregisterSong(void)
{
    if (song.native_events)
    {
        free(song.native_events);
        song.native_events = NULL;
    }

    song.num_events = 0;
    song.position = 0;
}

void I_Windows_ShutdownMusic(void)
{
    I_Windows_StopSong();
    I_Windows_UnregisterSong();

    midiOutUnprepareHeader((HMIDIOUT)hMidiStream, &buffer.MidiStreamHdr, sizeof(MIDIHDR));

    midiStreamClose(hMidiStream);

    hMidiStream = NULL;

    CloseHandle(hBufferReturnEvent);
    CloseHandle(hExitEvent);
}

#endif
