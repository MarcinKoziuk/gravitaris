# ADR 0003: IAudioBackend abstraction over OpenAL and miniaudio

Status: accepted, deliberately transitional (see "When to remove this" below)

## Decision

Gameplay code (`AudioSystem`) never touches `Magnum::Audio` or `miniaudio`
directly. It goes through `IAudioBackend`, an interface of opaque handles
(`SoundBufferHandle`, `VoiceHandle`) and verbs (`UploadBuffer`, `AcquireVoice`,
`PlayOneShot`, `PlayLooping`, ...). Two implementations exist:

- `MagnumOpenALBackend` — the original OpenAL Soft (via Magnum::Audio) code.
- `MiniaudioBackend` — miniaudio (single vendored header, `extlibs/miniaudio/`).

`CreateAudioBackend(AudioBackendPreference)` constructs exactly one backend,
no fallback: `Auto` resolves once to a platform-fixed choice (miniaudio on
`__APPLE__`, OpenAL elsewhere -- see "Why miniaudio as the macOS default"
below) and if THAT backend's `Init()` fails, audio disables outright rather
than silently substituting a different backend the caller didn't ask for.
`PreferOpenAL`/`PreferMiniaudio` force one explicitly, same no-fallback rule.
Selectable at runtime via the debug UI's "Audio" tab
(`AudioSystem::SetBackendPreference`), which tears down the current backend
and rebuilds it, re-uploading every still-live `AudioClip`.

## Why

Positional audio (bullets/hits/thruster) crashed/produced no sound on macOS.
Root cause, confirmed by instrumentation this session: `Magnum::Audio::
Context::tryCreate()` calls `alcMakeContextCurrent()` but never checks its
return value. On this machine's CoreAudio setup that call reports success
internally (Magnum's own bookkeeping believes the context is current) while
`alcGetCurrentContext()` reads back `nullptr` -- every AL call afterward then
fails with `AL_INVALID_OPERATION`. A hand-written, Magnum-free reproduction of
the identical `alcOpenDevice`/`alcCreateContext`/`alcMakeContextCurrent`
sequence succeeded every time on the same machine, ruling out a systemic
CoreAudio/OpenAL-Soft problem -- this is specifically a Magnum packaging/
correctness gap, not something fixable from our side beyond working around it.

`MagnumOpenALBackend::Init()` verifies `alcGetCurrentContext() != nullptr`
directly after `tryCreate()` and returns `false` if it isn't -- turning a
silent, always-fails-later bug into a clean, immediate fallback.

## Why miniaudio as the macOS default (not just "disable audio on macOS")

Single-file, public-domain, no build-system surface (drop-in header +
`MINIAUDIO_IMPLEMENTATION` translation unit, no FetchContent, no separate
dylib) -- which directly rules out the "two independent link units disagree
about backend state" class of bug this session hit. Native backends cover
macOS, iOS, Windows, Linux, Android in one dependency. See the session's chat
log for the fuller backend comparison (SoLoud, FMOD, SDL2 audio) that led
here; miniaudio won on "zero packaging surface" + broad platform coverage
+ no license to track.

No silent fallback: trying OpenAL first and falling back to miniaudio on
failure meant paying a failed-`Init()` cost on every macOS launch for a
backend known not to work there, and it made "what's actually running" one
step removed from what was asked for. `Auto` now resolves the platform choice
once, up front; if that specific backend fails, audio disables rather than
quietly landing on a different one.

## API-shape differences that mattered for the implementation

OpenAL's `Source` is buffer-agnostic (`setBuffer()` rebinds anytime).
miniaudio's `ma_sound` is bound to exactly one data source at
`ma_sound_init_from_data_source()` time -- there is no rebind call.
`MiniaudioBackend::PlayOneShot`/`PlayLooping` re-init the underlying
`ma_sound` on every call to keep `VoiceHandle`'s cross-backend contract
("assign any buffer, anytime") identical; cheap at our call rate (a handful
of plays/second).

**`ma_audio_buffer` owns a single playback cursor** (its own docs: "An audio
buffer has a playback cursor just like a decoder"). The first implementation
uploaded one `ma_audio_buffer` per `AudioClip` and bound every voice playing
that clip to the same buffer object -- so concurrent instances of one clip
shared one cursor. Symptom: firing repeatedly, only the first bullet was
audible (later shots' voices read from wherever the first shot's cursor had
already advanced to); two or more ships thrusting simultaneously produced
torn/distorted audio (their loop-voices raced the same cursor every mix
callback). Fix: `BufferSlot` now owns the decoded PCM directly (a
`std::vector<int16_t>`, not an `ma_audio_buffer`); each *voice* gets its own
`ma_audio_buffer_ref` -- a lightweight, non-owning data source with an
independent cursor -- pointing at that shared read-only memory, created fresh
(`ma_audio_buffer_ref_init` unconditionally zeroes the struct, cursor
included) on every `RebindVoice` call. N concurrent voices playing the same
clip now advance independently. Verified via distinct-address logging across
repeated one-shot fires (8 pool voices round-robining, fresh zeroed cursor
each time) and 3 simultaneous thruster loops on the same clip.

Known residual edge case, not yet handled: if a `BufferSlot` is released and
its index reused for a *different* clip's upload while some old voice still
holds an `ma_audio_buffer_ref` into the original data, that ref would dangle.
Not currently reachable in practice -- `AudioClip`s are loaded once at startup
and never reloaded mid-game, and a backend switch releases every voice before
any buffer -- but worth a generation-counter guard (same pattern as
`PhysicsRef`, `docs/adr/0002-physics-ownership.md`) if clips ever become
hot-reloadable.

`ma_sound`/`ma_audio_buffer_ref` (and OpenAL's `Buffer`/`Source`) are
referenced by raw pointer once initialized (miniaudio's/OpenAL's own internal
bookkeeping), so -- same lesson as `docs/adr/0002-physics-ownership.md` --
each lives in an individually heap-allocated slot (`std::unique_ptr`) inside a
growable vector, never directly as a vector element that could be relocated
by growth.

## AudioClip: reactive loading via ResourceLoader

Sound assets are `AudioClip` resources (`IResource`, decoded PCM16, mono),
loaded through `ResourceLoader` and uploaded reactively via
`OnCreate<AudioClip>`/`OnDestroy<AudioClip>` observers -- mirroring
`ModelRenderer2`'s `OnCreate<Model>`/`OnDestroy<Model>` pattern -- rather than
the previous ad hoc `IFilesystem::ReadBytes` + inline WAV parse. A failed
load silently substitutes `AudioClip::Placeholder()` (an empty/silent clip),
same graceful-degrade convention as `Model`/`Body`.

Music is explicitly out of scope for `AudioClip`/`IAudioBackend` as they
stand: a multi-minute track needs streaming decode (compressed source,
decoded a chunk at a time) rather than a fully-resident `ResourceLoader`
resource, and isn't positional. That's a separate abstraction for when music
assets actually exist (see IDEAS.md) -- likely a few additional
`IAudioBackend` methods (`OpenMusicStream`/`PlayMusic`/...) rather than a
second parallel system.

## When to remove this

This abstraction is scaffolding, not a permanent two-backend commitment.
Once miniaudio is confirmed solid across target platforms (or the Magnum
context-current bug is fixed upstream and verified), delete
`MagnumOpenALBackend`, `AudioBackendPreference`, `CreateAudioBackend`, and the
debug UI's backend picker; `AudioSystem` keeps just `IAudioBackend` and
whichever single implementation remains.
